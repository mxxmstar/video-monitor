#include "media/converter/media_frame_converter.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/publisher/publisher.h"
#include "media/simple_buffer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

namespace {

constexpr int kWidth = 16;
constexpr int kHeight = 16;
constexpr int kFrameRate = 25;
constexpr int kExpectedFrameCount = 10;
constexpr int64_t kStartPtsUs = 1'000'000;
constexpr int64_t kFrameDurationUs = 40'000;
constexpr int64_t kExpectedDurationUs =
    kExpectedFrameCount * kFrameDurationUs;

FramePtr MakeFrame(int index) {
    auto frame = std::make_shared<MediaFrame>();
    frame->type = MediaType::VIDEO;
    frame->time.pts_us = kStartPtsUs + index * kFrameDurationUs;
    frame->time.dts_us = frame->time.pts_us;
    frame->time.duration_us = kFrameDurationUs;

    VideoFrameMeta meta{};
    meta.pixel_format = PixelFormat::kI420;
    meta.width = kWidth;
    meta.height = kHeight;
    meta.plane_count = 3;
    meta.plane_info[0] = PlaneInfo{0, kWidth, kWidth * kHeight};
    meta.plane_info[1] = PlaneInfo{kWidth * kHeight,
                                   kWidth / 2,
                                   kWidth * kHeight / 4};
    meta.plane_info[2] = PlaneInfo{kWidth * kHeight * 5 / 4,
                                   kWidth / 2,
                                   kWidth * kHeight / 4};
    frame->meta = meta;

    std::vector<std::uint8_t> pixels(kWidth * kHeight * 3 / 2, 128);
    std::fill_n(pixels.begin(), kWidth * kHeight,
                static_cast<std::uint8_t>(32 + index * 16));
    frame->buffer = std::make_shared<SimpleBuffer>(std::move(pixels));
    return frame;
}

EncoderConfig MakeEncoderConfig() {
    EncoderConfig config{};
    config.media_type = MediaType::VIDEO;
    config.codec_type = CodecType::H264;
    config.encoder_name = "libx264";
    config.time_base_num = 1;
    config.time_base_den = kFrameRate;
    config.global_header = true;
    config.video().width = kWidth;
    config.video().height = kHeight;
    config.video().fps_num = kFrameRate;
    config.video().fps_den = 1;
    config.video().pixel_format = PixelFormat::kI420;
    config.video().gop_size = 25;
    config.video().max_b_frames = 0;
    return config;
}

MediaTrackConfig MakeMuxerConfig(const EncodedTrackInfo& encoded) {
    MediaTrackConfig config{};
    config.media_type = MediaType::VIDEO;
    config.codec_type = CodecType::H264;
    config.time_base_num = encoded.time_base.num;
    config.time_base_den = encoded.time_base.den;
    config.extra_data = encoded.extra_data;
    config.video().width = encoded.video().width;
    config.video().height = encoded.video().height;
    config.video().fps = encoded.video().fps;
    return config;
}

bool VerifyLocalFileTimeline(const std::filesystem::path& output_path) {
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, output_path.string().c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Failed to reopen timestamp test output" << std::endl;
        return false;
    }

    // avformat_open_input() 仅打开容器；必须继续探测流信息，才能读取可靠的
    // codec、分辨率、帧率和时长。这相当于用 FFmpeg API 替代手工执行 ffprobe，
    // 因此测试没有外部命令行依赖。
    if (avformat_find_stream_info(input, nullptr) < 0) {
        std::cerr << "Failed to probe timestamp test output" << std::endl;
        avformat_close_input(&input);
        return false;
    }

    // 当前输出链路只创建一条 H.264 视频流。这里明确检查该前提，以便未来
    // 接入音频或多轨后，测试能提示需要升级断言，而不是悄悄只检查第一条流。
    if (input->nb_streams != 1 || !input->streams[0] ||
        input->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
        input->streams[0]->codecpar->codec_id != AV_CODEC_ID_H264 ||
        input->streams[0]->codecpar->width != kWidth ||
        input->streams[0]->codecpar->height != kHeight) {
        std::cerr << "Output stream metadata does not match the encoded video" << std::endl;
        avformat_close_input(&input);
        return false;
    }

    AVStream* const video_stream = input->streams[0];
    const AVRational reported_frame_rate =
        av_guess_frame_rate(input, video_stream, nullptr);
    const double frame_rate = av_q2d(reported_frame_rate);
    if (reported_frame_rate.num <= 0 || reported_frame_rate.den <= 0 ||
        std::abs(frame_rate - static_cast<double>(kFrameRate)) > 0.01) {
        std::cerr << "Unexpected output frame rate: " << frame_rate << std::endl;
        avformat_close_input(&input);
        return false;
    }

    // MP4 容器时长以 AV_TIME_BASE（微秒）表示。允许一个输出帧的误差，避免
    // 不同 FFmpeg 版本在尾帧 duration 推导上的取整差异造成不稳定测试。
    if (input->duration == AV_NOPTS_VALUE) {
        std::cerr << "Output duration is unavailable" << std::endl;
        avformat_close_input(&input);
        return false;
    }

    const int64_t duration_difference = input->duration - kExpectedDurationUs;
    if (duration_difference < -kFrameDurationUs ||
        duration_difference > kFrameDurationUs) {
        std::cerr << "Unexpected output duration: " << input->duration << std::endl;
        avformat_close_input(&input);
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    bool valid = packet != nullptr;
    int packet_index = 0;
    int64_t packet_duration_us = 0;
    while (valid && av_read_frame(input, packet) >= 0) {
        // 只要输出容器中混入了其它 stream，就说明当前单视频轨 Publisher 的
        // 路由边界被破坏。不能继续把它当作第 N 个视频包计算。
        if (packet->stream_index != video_stream->index) {
            valid = false;
            av_packet_unref(packet);
            break;
        }

        int64_t earliest = AV_NOPTS_VALUE;
        if (packet->pts != AV_NOPTS_VALUE) {
            earliest = packet->pts;
        }
        if (packet->dts != AV_NOPTS_VALUE &&
            (earliest == AV_NOPTS_VALUE || packet->dts < earliest)) {
            earliest = packet->dts;
        }

        // 输入给编码器的第一个帧从 1 秒开始，但 FFmpegMuxer 对本地文件
        // 按首包时间戳归零。因此重新读取 MP4 后，第 N 包必须从 N 帧开始。
        const AVRational source_time_base{1, kFrameRate};
        const int64_t expected = av_rescale_q(
            packet_index, source_time_base, video_stream->time_base);
        valid = earliest == expected;

        if (packet->duration == AV_NOPTS_VALUE) {
            valid = false;
        } else {
            packet_duration_us += av_rescale_q(
                packet->duration, video_stream->time_base,
                AVRational{1, AV_TIME_BASE});
        }

        ++packet_index;
        av_packet_unref(packet);
    }
    valid = valid && packet_index == kExpectedFrameCount &&
            packet_duration_us >= kExpectedDurationUs - kFrameDurationUs &&
            packet_duration_us <= kExpectedDurationUs + kFrameDurationUs;

    av_packet_free(&packet);
    avformat_close_input(&input);
    return valid;
}

}  // namespace

int main() {
    FFmpegEncoder encoder;
    const EncoderConfig encoder_config = MakeEncoderConfig();
    if (!encoder.Open(encoder_config)) {
        std::cerr << "Failed to open timestamp test encoder" << std::endl;
        return 1;
    }

    std::vector<PacketPtr> packets;
    for (int index = 0; index < kExpectedFrameCount; ++index) {
        if (!encoder.Encode(MakeFrame(index), packets)) {
            std::cerr << "Failed to encode timestamp test frame" << std::endl;
            return 1;
        }
    }
    if (!encoder.Flush(packets)) {
        std::cerr << "Failed to flush timestamp test encoder" << std::endl;
        return 1;
    }

    if (packets.size() != kExpectedFrameCount) {
        std::cerr << "Expected " << kExpectedFrameCount
                  << " encoded packets, got " << packets.size() << std::endl;
        return 1;
    }
    for (std::size_t index = 0; index < packets.size(); ++index) {
        const int64_t expected_pts = 25 + static_cast<int64_t>(index);
        if (packets[index]->pts != expected_pts || packets[index]->duration != 1) {
            std::cerr << "Timestamp conversion mismatch: pts=" << packets[index]->pts
                      << ", duration=" << packets[index]->duration << std::endl;
            return 1;
        }
    }

    const std::filesystem::path output_path =
        std::filesystem::current_path() / "ffmpeg_timestamp_normalization_test.mp4";
    std::error_code remove_error;
    std::filesystem::remove(output_path, remove_error);

    // 使用真实编码包经过 Publisher 写入。实际链路为：
    // Publisher -> PusherSession -> FFmpegPusher -> FFmpegMuxer。
    // 该测试同时验证首个关键帧能启动会话、Publisher 会转发后续包、Muxer
    // 仍会将本地 MP4 的起始时间戳归零，以及 Close() 会写出可读取的尾部。
    PublisherConfig publisher_config;
    publisher_config.kind = PublisherKind::FFmpegFile;
    publisher_config.session.pusher.output_url = output_path.string();
    publisher_config.session.pusher.video_track =
        MakeMuxerConfig(encoder.GetOutputInfo());

    Publisher publisher;
    const PusherResult open_result = publisher.Open(publisher_config);
    if (!open_result.Succeed()) {
        std::cerr << "Failed to open timestamp test publisher: "
                  << (open_result.error.has_value()
                          ? open_result.error->message
                          : "unknown pusher error")
                  << std::endl;
        return 1;
    }
    int published_packet_count = 0;
    for (const auto& packet : packets) {
        const PusherPublishResult publish_result = publisher.Publish(*packet);
        if (!publish_result.Succeed() || !publish_result.WasPublished()) {
            std::cerr << "Failed to publish timestamp test packet: "
                      << (publish_result.error.has_value()
                              ? publish_result.error->message
                              : "packet was unexpectedly dropped")
                      << std::endl;
            publisher.Close();
            return 1;
        }
        ++published_packet_count;
    }

    // Close() 会下沉到 FFmpegMuxer::Close() 写入 MP4 trailer。只有成功关闭后，
    // 下方重新打开文件验证时长、帧率和包时间戳才有意义。
    if (!publisher.Close().Succeed()) {
        std::cerr << "Failed to finalize timestamp test output" << std::endl;
        return 1;
    }

    if (published_packet_count != kExpectedFrameCount) {
        std::cerr << "Publisher wrote " << published_packet_count
                  << " packets instead of " << kExpectedFrameCount << std::endl;
        return 1;
    }

    const bool normalized = VerifyLocalFileTimeline(output_path);
    std::filesystem::remove(output_path, remove_error);
    if (!normalized) {
        std::cerr << "Local output timeline, duration, frame count, or frame rate is invalid"
                  << std::endl;
        return 1;
    }

    std::cout << "FFmpeg timestamp handling test passed" << std::endl;
    return 0;
}
