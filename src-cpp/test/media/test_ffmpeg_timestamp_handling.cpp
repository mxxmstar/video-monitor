#include "media/converter/media_frame_converter.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/publisher/publisher.h"
#include "media/simple_buffer.h"

#include <algorithm>
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
constexpr int64_t kStartPtsUs = 1'000'000;
constexpr int64_t kFrameDurationUs = 40'000;

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
    config.time_base_den = 25;
    config.global_header = true;
    config.video().width = kWidth;
    config.video().height = kHeight;
    config.video().fps_num = 25;
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

    AVPacket* packet = av_packet_alloc();
    bool valid = packet != nullptr;
    int packet_index = 0;
    while (valid && av_read_frame(input, packet) >= 0) {
        int64_t earliest = AV_NOPTS_VALUE;
        if (packet->pts != AV_NOPTS_VALUE) {
            earliest = packet->pts;
        }
        if (packet->dts != AV_NOPTS_VALUE &&
            (earliest == AV_NOPTS_VALUE || packet->dts < earliest)) {
            earliest = packet->dts;
        }

        const AVRational source_time_base{1, 25};
        const int64_t expected = av_rescale_q(
            packet_index, source_time_base, input->streams[packet->stream_index]->time_base);
        valid = earliest == expected;
        ++packet_index;
        av_packet_unref(packet);
    }
    valid = valid && packet_index == 3;

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
    for (int index = 0; index < 3; ++index) {
        if (!encoder.Encode(MakeFrame(index), packets)) {
            std::cerr << "Failed to encode timestamp test frame" << std::endl;
            return 1;
        }
    }
    if (!encoder.Flush(packets)) {
        std::cerr << "Failed to flush timestamp test encoder" << std::endl;
        return 1;
    }

    if (packets.size() != 3) {
        std::cerr << "Expected 3 encoded packets, got " << packets.size() << std::endl;
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
    for (const auto& packet : packets) {
        const PusherPublishResult publish_result = publisher.Publish(*packet);
        if (!publish_result.Succeed()) {
            std::cerr << "Failed to publish timestamp test packet: "
                      << (publish_result.error.has_value()
                              ? publish_result.error->message
                              : "unknown pusher error")
                      << std::endl;
            publisher.Close();
            return 1;
        }
    }
    publisher.Close();

    const bool normalized = VerifyLocalFileTimeline(output_path);
    std::filesystem::remove(output_path, remove_error);
    if (!normalized) {
        std::cerr << "Local output did not start at timestamp zero" << std::endl;
        return 1;
    }

    std::cout << "FFmpeg timestamp handling test passed" << std::endl;
    return 0;
}
