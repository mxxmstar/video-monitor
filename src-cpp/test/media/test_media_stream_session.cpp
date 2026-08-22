/// @file test_media_stream_session.cpp
/// @brief MediaStreamSession 的 RTSP 正常路径测试。

#include "media/puller/ffmpeg_puller.h"
#include "media/stream/stream_session.h"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>

namespace {

constexpr const char* kRtspUri =
    "rtsp://192.168.66.83/live/mainstream";
constexpr int kExpectedPacketCount = 10;

int RunSessionTest() {
    FFmpegPullerConfig puller_config;
    puller_config.io.connect_timeout = std::chrono::seconds(5);
    puller_config.io.read_timeout = std::chrono::seconds(5);
    puller_config.latency = LatencyMode::Low;

    RtspInputOptions rtsp;
    rtsp.transport = "tcp";
    puller_config.rtsp = rtsp;

    InputEndpointConfig endpoint;
    endpoint.uri = kRtspUri;
    endpoint.puller_kind = PullerKind::FFmpeg;

    boost::asio::io_context io;
    auto session = std::make_shared<MediaStreamSession>(io);
    session->SetPuller(std::make_unique<FFmpegPuller>(std::move(puller_config)));
    session->SetEndpoint(endpoint);

    std::mutex mutex;
    std::condition_variable condition;
    int packet_count = 0;
    bool stream_info_received = false;

    session->SetStreamInfoCallback(
        [&mutex, &condition, &stream_info_received](const MultiStreamInfo& info) {
            if (!info.HasVideoStream() && !info.HasAudioStream()) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                stream_info_received = true;
            }
            condition.notify_all();
        });
    session->SetPacketCallback(
        [&mutex, &condition, &packet_count](std::shared_ptr<MediaPacket> packet) {
            if (!packet || !packet->buffer || packet->buffer->Size() == 0) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++packet_count;
            }
            condition.notify_all();
        });

    if (!session->Start()) {
        std::cerr << "Session start failed" << std::endl;
        return 1;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(10), [&] {
                return packet_count >= kExpectedPacketCount;
            })) {
            std::cerr << "Timed out waiting for session packets" << std::endl;
            session->Stop();
            return 1;
        }
    }

    session->Stop();
    const MediaStreamSession::Stats stats = session->GetStats();

    if (!stream_info_received || stats.packets_received < kExpectedPacketCount) {
        std::cerr << "Session statistics or stream info are incomplete"
                  << std::endl;
        return 1;
    }

    std::cout << "Session packets: " << stats.packets_received
              << ", bytes: " << stats.bytes_received << std::endl;
    std::cout << "MediaStreamSession RTSP test passed" << std::endl;
    return 0;
}

}  // namespace

int main() {
    return RunSessionTest();
}
