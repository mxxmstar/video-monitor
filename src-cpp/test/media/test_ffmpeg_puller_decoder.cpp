/// @file test_ffmpeg_puller.cpp
/// @brief FFmpegPuller 的 RTSP 集成测试。

#include "media/puller/ffmpeg_puller.h"
#include "media/decoder/ffmpeg_decoder.h"
#include <chrono>
#include <iostream>
#include <utility>

namespace {

constexpr const char* kRtspUri = "rtsp://192.168.66.83/live/mainstream";
constexpr int kExpectedPacketCount = 10;
constexpr int kMaxReadAttempts = 10000;

int RunFfmpegPullerTest() {
    FFmpegPullerConfig config;
    config.io.connect_timeout = std::chrono::seconds(5);
    config.io.read_timeout = std::chrono::seconds(5);
    config.latency = LatencyMode::Low;

    RtspInputOptions rtsp;
    rtsp.transport = "tcp";
    config.rtsp = rtsp;

    InputEndpointConfig endpoint;
    endpoint.uri = kRtspUri;
    endpoint.puller_kind = PullerKind::FFmpeg;

    FFmpegPuller puller(std::move(config));

    std::cout << "Opening RTSP stream: " << endpoint.uri << std::endl;
    const PullOpenResult open_result = puller.Open(endpoint);
    if (!open_result.Succeed()) {
        std::cerr << "Open failed: "
                  << (open_result.error.has_value()
                          ? open_result.error->message
                          : "unknown error")
                  << std::endl;
        return 1;
    }

    const MultiStreamInfo stream_info = puller.GetStreamInfo();
    if (!stream_info.HasVideoStream() && !stream_info.HasAudioStream()) {
        std::cerr << "Open succeeded, but no audio or video stream was found"
                  << std::endl;
        puller.Close();
        return 1;
    }

    std::cout << "Stream info: " << stream_info.stream_infos.size()
              << " audio/video stream(s)" << std::endl;

    int packet_count = 0;
    for (int attempt = 0;
         attempt < kMaxReadAttempts && packet_count < kExpectedPacketCount;
         ++attempt) {
        const PullReadResult read_result = puller.ReadPacket();

        if (read_result.status == PullReadStatus::NoData) {
            continue;
        }

        if (read_result.status != PullReadStatus::Packet ||
            !read_result.packet ||
            !read_result.packet->buffer ||
            read_result.packet->buffer->Size() == 0) {
            std::cerr << "Read failed at attempt " << (attempt + 1)
                      << ": status="
                      << static_cast<int>(read_result.status);
            if (read_result.error.has_value()) {
                std::cerr << ", error=" << read_result.error->message;
            }
            std::cerr << std::endl;
            puller.Close();
            return 1;
        }

        ++packet_count;
        std::cout << "Packet " << packet_count
                  << ": type="
                  << static_cast<int>(read_result.packet->type)
                  << ", codec="
                  << static_cast<int>(read_result.packet->codec)
                  << ", stream_index=" << read_result.packet->stream_index
                  << ", size=" << read_result.packet->buffer->Size()
                  << std::endl;
    }

    puller.Close();

    if (packet_count != kExpectedPacketCount) {
        std::cerr << "Only received " << packet_count << " packet(s), expected "
                  << kExpectedPacketCount << std::endl;
        return 1;
    }

    std::cout << "FFmpegPuller RTSP test passed" << std::endl;
    return 0;
}

int RunFfmpegPullerDecoderTest() {
    // 本测试只验证一条最小视频解码链路：
    //
    //     RTSP -> FFmpegPuller -> H.264 MediaPacket -> FFmpegDecoder -> MediaFrame
    //
    // 该 RTSP 输入同时包含 H.264 视频和 G.711A 音频。一个 FFmpegDecoder
    // 实例一次只能按一种 codec/轨道工作，所以循环中必须显式筛选视频包，
    // 不能把音频包一并送给 H.264 解码器。
    FFmpegPullerConfig config;
    config.io.connect_timeout = std::chrono::seconds(5);
    config.io.read_timeout = std::chrono::seconds(5);
    config.latency = LatencyMode::Low;

    RtspInputOptions rtsp;
    rtsp.transport = "tcp";
    config.rtsp = rtsp;

    InputEndpointConfig endpoint;
    endpoint.uri = kRtspUri;
    endpoint.puller_kind = PullerKind::FFmpeg;

    FFmpegPuller puller(std::move(config));

    std::cout << "Opening RTSP stream: " << endpoint.uri << std::endl;
    const PullOpenResult open_result = puller.Open(endpoint);
    if (!open_result.Succeed()) {
        std::cerr << "Open failed: "
                  << (open_result.error.has_value()
                          ? open_result.error->message
                          : "unknown error")
                  << std::endl;
        return 1;
    }

    const MultiStreamInfo stream_info = puller.GetStreamInfo();
    // 这是“视频解码”测试，因此音频流存在与否不影响本测试；但必须能够
    // 从 MultiStreamInfo 中找到视频流，否则无法创建 H.264 decoder。
    if (!stream_info.HasVideoStream()) {
        std::cerr << "Open succeeded, but no video stream was found"
                  << std::endl;
        puller.Close();
        return 1;
    }

    std::cout << "Stream info: " << stream_info.stream_infos.size()
              << " audio/video stream(s)" << std::endl;

    // video_stream_idx_ 是 stream_infos 容器中的位置；
    // video_stream_info.stream_index 才是 FFmpeg/MediaPacket 使用的真实流号。
    // 不能直接写 stream_infos[0]，因为其他输入中的音频、字幕等轨道可能
    // 排在视频流之前。
    const MediaStreamInfo& video_stream_info =
        stream_info.stream_infos[stream_info.video_stream_idx_];

    FFmpegDecoder decoder;
    int decoded_frames = 0;
    decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        // Decode() 可能因为 B 帧重排序而一次输出零帧或多帧，因此成功条件
        // 应以真正收到的 MediaFrame 为准，不能只看送入 decoder 的包数量。
        if (!frame || frame->type != MediaType::VIDEO) {
            return;
        }

        ++decoded_frames;
        std::cout << "Decoded frame " << decoded_frames
                  << ": " << frame->Width() << "x" << frame->Height()
                  << ", pixel_format="
                  << static_cast<int>(frame->GetPixelFormat())
                  << ", pts_us=" << frame->time.pts_us
                  << std::endl;
    });

    // Open() 会根据 H.264 的 codec、time_base 和 SPS/PPS extra_data 创建
    // AVCodecContext。若这一步失败，继续读取包没有意义。
    if (!decoder.Open(video_stream_info)) {
        std::cerr << "Failed to open video decoder" << std::endl;
        puller.Close();
        return 1;
    }

    int input_packet_count = 0;
    int video_packet_count = 0;
    // 本测试的通过条件是解码出 10 帧视频，而不是收到 10 个视频包。
    // 对 H.264 而言，编码包和解码帧并非一一对应：接入直播流后可能要先
    // 等待关键帧，且 B 帧重排序也会影响某个包何时产生输出帧。
    constexpr int kExpectedDecodedFrameCount = 10;

    // 限制的是“读取尝试次数”，防止服务异常或始终没有关键帧时测试无限
    // 等待。循环退出的正常条件是收到了足够的视频解码帧，而不是读到了
    // 固定数量的音视频混合包。
    for (int attempt = 0;
         attempt < kMaxReadAttempts &&
         decoded_frames < kExpectedDecodedFrameCount;
         ++attempt) {
        const PullReadResult read_result = puller.ReadPacket();

        if (read_result.status == PullReadStatus::NoData) {
            // NoData 不代表输入结束；live RTSP 在暂无可交付包时可正常返回它。
            continue;
        }

        if (read_result.status != PullReadStatus::Packet ||
            !read_result.packet ||
            !read_result.packet->buffer ||
            read_result.packet->buffer->Size() == 0) {
            std::cerr << "Read failed at attempt " << (attempt + 1)
                      << ": status="
                      << static_cast<int>(read_result.status);
            if (read_result.error.has_value()) {
                std::cerr << ", error=" << read_result.error->message;
            }
            std::cerr << std::endl;
            puller.Close();
            return 1;
        }

        ++input_packet_count;
        std::cout << "Input packet " << input_packet_count
                  << ": type="
                  << static_cast<int>(read_result.packet->type)
                  << ", codec="
                  << static_cast<int>(read_result.packet->codec)
                  << ", stream_index=" << read_result.packet->stream_index
                  << ", size=" << read_result.packet->buffer->Size()
                  << std::endl;

        // 当前 RTSP 流有 audio(stream_index=1) 和 video(stream_index=0)。
        // 音频 G.711A 数据没有 H.264 NAL 起始码；若送入 H.264 decoder，
        // FFmpeg 必然打印 "No start code is found"。因此这里直接跳过
        // 不属于当前视频轨道的包。
        if (read_result.packet->stream_index != video_stream_info.stream_index) {
            continue;
        }

        // stream_index 是首要依据；type/codec 的检查是测试层防御，能够在
        // Puller 的媒体元数据与实际视频轨道不一致时给出清晰报错。
        if (read_result.packet->type != MediaType::VIDEO ||
            read_result.packet->codec != video_stream_info.codec_type) {
            std::cerr << "Video stream packet metadata does not match decoder"
                      << std::endl;
            decoder.Close();
            puller.Close();
            return 1;
        }

        ++video_packet_count;
        if (!decoder.Decode(read_result.packet)) {
            std::cerr << "Video decode failed at video packet "
                      << video_packet_count << std::endl;
            decoder.Close();
            puller.Close();
            return 1;
        }
    }

    // H.264 可能缓存 B 帧。即使读取循环已经停止，也要先向 decoder 发送
    // 空包进行 drain，才能拿到所有已接收编码包对应的尾部输出帧。
    if (!decoder.Flush()) {
        std::cerr << "Failed to flush video decoder" << std::endl;
        decoder.Close();
        puller.Close();
        return 1;
    }

    // 先 Flush 再 Close：Close 只释放 AVCodecContext，不会主动输出缓存帧。
    decoder.Close();
    puller.Close();

    if (decoded_frames < kExpectedDecodedFrameCount) {
        std::cerr << "Only decoded " << decoded_frames << " video frame(s) from "
                  << video_packet_count << " video packet(s) after "
                  << input_packet_count << " input packet(s); expected "
                  << kExpectedDecodedFrameCount << std::endl;
        return 1;
    }

    // 成功结果只表达测试真正验证的条件：已经解码出目标数量的视频帧。
    // video_packet_count 是过程诊断数据，不能作为“10 帧必须来自 10 包”
    // 的断言依据。
    std::cout << "FFmpegPuller -> FFmpegDecoder test passed: "
              << decoded_frames << " video frame(s) decoded" << std::endl;
    std::cout << "Diagnostic: " << video_packet_count
              << " video packet(s) consumed from " << input_packet_count
              << " input packet(s)" << std::endl;
    return 0;
}

}  // namespace

int main() {
    //return RunFfmpegPullerTest();
    return RunFfmpegPullerDecoderTest();
}
