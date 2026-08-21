/// @file test_ffmpeg_puller.cpp
/// @brief FFmpegPuller 的 RTSP 集成测试。

#include "media/puller/ffmpeg_puller.h"

#include <chrono>
#include <iostream>
#include <utility>

namespace {

constexpr const char* kRtspUri = "rtsp://192.168.66.83/live/mainstream";
constexpr int kExpectedPacketCount = 10;
constexpr int kMaxReadAttempts = 100;

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

}  // namespace

int main() {
    return RunFfmpegPullerTest();
}
