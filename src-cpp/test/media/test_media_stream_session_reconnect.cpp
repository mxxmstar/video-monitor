/// @file test_media_stream_session_reconnect.cpp
/// @brief 使用假 Puller 验证 MediaStreamSession 的最小重连流程。

#include "media/puller/i_puller.h"
#include "media/simple_buffer.h"
#include "media/stream/stream_session.h"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace {

/// @brief 记录每次 Open() 的开始时间，用于验证重连退避间隔。
struct OpenHistory {
    std::mutex mutex;
    std::vector<std::chrono::steady_clock::time_point> timestamps;
};

/// @brief 可以控制每次 Open() 成功或失败的测试 Puller。
///
/// open_results_ 控制每一次 Open() 调用的结果，connection_scripts_ 控制
/// 每次成功连接后 ReadPacket() 返回的结果。例如：
///
///     open_results_       = [true, true]
///     connection_scripts_ = [[Packet, RetryableError], [Packet, EOS]]
///
/// 这表示第一次连接读到包后断线，第二次连接成功并正常结束。Session
/// 重连测试因此不依赖真实网络，也不会受到 RTSP 服务状态影响。
class ReconnectPuller final : public IPuller {
public:
    ReconnectPuller(std::vector<bool> open_results,
                    std::vector<std::vector<PullReadResult>> connection_scripts,
                    std::shared_ptr<OpenHistory> open_history = nullptr)
        : open_results_(std::move(open_results)),
          connection_scripts_(std::move(connection_scripts)),
          open_history_(std::move(open_history)) {}

    /// @brief 按 open_results_ 模拟第 n 次 Open() 的结果。
    PullOpenResult Open(const InputEndpointConfig& endpoint) override {
        if (open_history_) {
            std::lock_guard<std::mutex> lock(open_history_->mutex);
            open_history_->timestamps.push_back(std::chrono::steady_clock::now());
        }

        if (endpoint.uri.empty()) {
            return PullOpenResult::Failed({
                PullErrorCategory::InvalidConfiguration,
                0,
                "reconnect endpoint URI is empty",
                false,
            });
        }

        const std::size_t attempt = open_attempt_++;
        const bool succeed = attempt < open_results_.size()
            ? open_results_[attempt]    // 根据 open_results 决定成功/失败
            : false;

        closed_.store(true);
        if (!succeed) {
            // 第 2 次 Open 失败走这里
            return PullOpenResult::Failed({
                PullErrorCategory::Network,
                0,
                "scripted reconnect open failed",
                true,
            });
        }

        // 每次成功 Open() 对应一份新的读取脚本。这里使用移动操作，表示
        // 当前连接独占自己的结果序列，避免连接之间共享读取游标。
        if (successful_connection_ >= connection_scripts_.size()) {
            return PullOpenResult::Failed({
                PullErrorCategory::Internal,
                0,
                "missing scripted connection results",
                false,
            });
        }
        // 只有成功才会使用 scripts
        active_results_ = std::move(connection_scripts_[successful_connection_++]);
        position_ = 0;
        closed_.store(false);
        return PullOpenResult::Success();
    }

    /// @brief 让当前连接停止，模拟底层资源被关闭。
    void Close() override {
        closed_.store(true);
    }

    /// @brief 返回当前连接脚本中的下一条读取结果。
    PullReadResult ReadPacket() override {
        if (closed_.load()) {
            return {
                PullError{
                    PullErrorCategory::Cancelled,
                    0,
                    "reconnect puller was closed",
                    false,
                },
                nullptr,
                PullReadStatus::Stopped,
            };
        }

        if (position_ >= active_results_.size()) {
            return {
                PullError{
                    PullErrorCategory::EndOfInput,
                    0,
                    "scripted connection input exhausted",
                    false,
                },
                nullptr,
                PullReadStatus::EOS,
            };
        }

        return active_results_[position_++];
    }

    /// @brief 返回固定流信息，验证重连成功后 Session 会再次发布流信息。
    MultiStreamInfo GetStreamInfo() const override {
        return stream_info_;
    }

    /// @brief 保存接口要求的事件回调；本测试暂不主动生成事件。
    void SetEventCallback(EventCallback cb) override {
        event_callback_ = std::move(cb);
    }

private:
    /// 每次 Open() 的成功/失败计划。
    std::vector<bool> open_results_;

    /// 每次成功连接对应的一组 ReadPacket() 返回值。
    std::vector<std::vector<PullReadResult>> connection_scripts_;

    /// 已经调用 Open() 的次数，用于选择 open_results_。
    std::size_t open_attempt_{0};

    /// 已经成功建立的连接数，用于选择 connection_scripts_。
    std::size_t successful_connection_{0};

    /// 当前连接正在使用的读取脚本。
    std::vector<PullReadResult> active_results_;

    /// 当前连接脚本中下一条待读取结果的下标。
    std::size_t position_{0};

    /// Close() 和 ReadPacket() 可能并发，因此使用原子关闭标志。
    std::atomic<bool> closed_{true};

    EventCallback event_callback_;
    std::shared_ptr<OpenHistory> open_history_;

    MultiStreamInfo stream_info_ = [] {
        MultiStreamInfo info;
        MediaStreamInfo video;
        video.media_type = MediaType::VIDEO;
        video.codec_type = CodecType::H264;
        video.stream_index = 0;
        video.detail = VideoStreamInfo{1280, 720, 25.0F};
        info.stream_infos.push_back(std::move(video));
        info.video_stream_idx_ = 0;
        return info;
    }();
};

/// @brief 创建一个带有效载荷的媒体包结果。
PullReadResult MakePacketResult(std::size_t size) {
    auto packet = std::make_shared<MediaPacket>();
    packet->type = MediaType::VIDEO;
    packet->codec = CodecType::H264;
    packet->stream_index = 0;
    packet->pts = 100;
    packet->dts = 100;
    packet->duration = 40;
    packet->time_base = {1, 1000};
    packet->keyframe = true;
    packet->buffer = std::make_shared<SimpleBuffer>(
        std::vector<std::uint8_t>(size, 0x5A));

    return {
        std::nullopt,
        std::move(packet),
        PullReadStatus::Packet,
    };
}

/// @brief 创建一个带结构化错误信息的非 Packet 结果。
PullReadResult MakeStatusResult(PullReadStatus status,
                                PullErrorCategory category,
                                bool retryable,
                                const char* message) {
    return {
        PullError{category, 0, message, retryable},
        nullptr,
        status,
    };
}

/// @brief 创建一个暂时没有可交付包的结果。
///
/// Session 收到 NoData 后会按 no_data_backoff 让出一小段时间，再继续
/// 读取。测试用它模拟连接仍然存活但暂时没有媒体包的稳定阶段。
PullReadResult MakeNoDataResult() {
    return {
        std::nullopt,
        nullptr,
        PullReadStatus::NoData,
    };
}

/// @brief 记录 Session 回调，供两个重连场景共同使用。
struct Observations {
    std::mutex mutex;
    std::condition_variable condition;
    int packet_count{0};
    std::size_t packet_bytes{0};
    int stream_info_count{0};
    std::vector<MediaStreamSession::State> states;
    std::optional<MediaStreamSession::State> terminal_state;
};

void AttachCallbacks(const std::shared_ptr<MediaStreamSession>& session,
                     Observations& observations) {
    session->SetStreamInfoCallback(
        [&observations](const MultiStreamInfo& info) {
            if (!info.HasVideoStream()) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(observations.mutex);
                ++observations.stream_info_count;
            }
            observations.condition.notify_all();
        });

    session->SetPacketCallback(
        [&observations](std::shared_ptr<MediaPacket> packet) {
            if (!packet || !packet->buffer || packet->buffer->Size() == 0) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(observations.mutex);
                ++observations.packet_count;
                observations.packet_bytes += packet->buffer->Size();
            }
            observations.condition.notify_all();
        });

    session->SetStateCallback(
        [&observations](MediaStreamSession::State state) {
            {
                std::lock_guard<std::mutex> lock(observations.mutex);
                observations.states.push_back(state);
                if (!observations.terminal_state.has_value() &&
                    (state == MediaStreamSession::State::KSTOPPED ||
                     state == MediaStreamSession::State::KERROR)) {
                    observations.terminal_state = state;
                }
            }
            observations.condition.notify_all();
        });
}

bool WaitForTerminal(const std::shared_ptr<MediaStreamSession>& session,
                     Observations& observations) {
    bool finished = false;
    {
        std::unique_lock<std::mutex> lock(observations.mutex);
        finished = observations.condition.wait_for(
            lock,
            std::chrono::seconds(2),
            [&observations] {
                return observations.terminal_state.has_value();
            });
    }
    if (!finished) {
        std::cerr << "Timed out waiting for reconnect test" << std::endl;
        session->Stop();
        return false;
    }
    return true;
}

bool WaitForState(Observations& observations,
                  MediaStreamSession::State expected_state) {
    std::unique_lock<std::mutex> lock(observations.mutex);
    return observations.condition.wait_for(
        lock,
        std::chrono::seconds(2),
        [&observations, expected_state] {
            for (const MediaStreamSession::State state : observations.states) {
                if (state == expected_state) {
                    return true;
                }
            }
            return false;
        });
}

bool ContainsState(const Observations& observations,
                   MediaStreamSession::State expected) {
    for (const MediaStreamSession::State state : observations.states) {
        if (state == expected) {
            return true;
        }
    }
    return false;
}

std::vector<std::chrono::steady_clock::time_point> SnapshotOpenTimes(
    const std::shared_ptr<OpenHistory>& open_history) {
    std::lock_guard<std::mutex> lock(open_history->mutex);
    return open_history->timestamps;
}

bool RunReconnectSuccessTest() {
    /// 创建测试脚本，模拟重连成功场景
    std::vector<std::vector<PullReadResult>> scripts;
    scripts.push_back({
        MakePacketResult(16),
        MakeStatusResult(
            PullReadStatus::RetryableError,
            PullErrorCategory::Network,
            true,
            "first connection lost"),
    });
    scripts.push_back({
        MakePacketResult(8),
        MakeStatusResult(
            PullReadStatus::EOS,
            PullErrorCategory::EndOfInput,
            false,
            "reconnected input ended"),
    });

    boost::asio::io_context io;
    auto session = std::make_shared<MediaStreamSession>(io);
    session->SetPuller(std::make_unique<ReconnectPuller>(
        std::vector<bool>{true, true}, std::move(scripts)));

    InputEndpointConfig endpoint;
    endpoint.uri = "scripted://reconnect-success";
    endpoint.puller_kind = PullerKind::FFmpeg;
    session->SetEndpoint(endpoint);

    SessionConfig config;
    config.reconnect.enabled = true;
    config.reconnect.initial_delay = std::chrono::milliseconds(0);
    config.reconnect.max_attempts = 1;
    session->SetSessionConfig(config);
    

    // 记录 Session 回调，供两个重连场景共同使用。
    Observations observations;
    AttachCallbacks(session, observations);

    if (!session->Start() || !WaitForTerminal(session, observations)) {
        return false;
    }

    session->Stop();
    const MediaStreamSession::Stats stats = session->GetStats();

    if (*observations.terminal_state != MediaStreamSession::State::KSTOPPED ||
        stats.packets_received != 2 || stats.bytes_received != 24 ||
        stats.reconnect_count != 1 || observations.packet_count != 2 ||
        observations.packet_bytes != 24 || observations.stream_info_count != 2 ||
        !ContainsState(observations,
                       MediaStreamSession::State::KRECONNECTING)) {
        std::cerr << "Reconnect success case assertions failed" << std::endl;
        return false;
    }

    std::cout << "Reconnect success: packets=" << stats.packets_received
              << ", bytes=" << stats.bytes_received
              << ", reconnect_attempts=" << stats.reconnect_count << std::endl;
    return true;
}

bool RunReconnectExhaustedTest() {
    std::vector<std::vector<PullReadResult>> scripts;
    scripts.push_back({
        MakePacketResult(4),
        MakeStatusResult(
            PullReadStatus::RetryableError,
            PullErrorCategory::Network,
            true,
            "connection lost permanently"),
    });

    boost::asio::io_context io;
    auto session = std::make_shared<MediaStreamSession>(io);
    session->SetPuller(std::make_unique<ReconnectPuller>(
        std::vector<bool>{true, false, false}, std::move(scripts)));

    InputEndpointConfig endpoint;
    endpoint.uri = "scripted://reconnect-exhausted";
    endpoint.puller_kind = PullerKind::FFmpeg;
    session->SetEndpoint(endpoint);

    SessionConfig config;
    config.reconnect.enabled = true;
    config.reconnect.initial_delay = std::chrono::milliseconds(0);
    config.reconnect.max_attempts = 2;
    session->SetSessionConfig(config);

    Observations observations;
    AttachCallbacks(session, observations);

    if (!session->Start() || !WaitForTerminal(session, observations)) {
        return false;
    }

    session->Stop();
    const MediaStreamSession::Stats stats = session->GetStats();

    if (*observations.terminal_state != MediaStreamSession::State::KERROR ||
        stats.packets_received != 1 || stats.bytes_received != 4 ||
        stats.reconnect_count != 2 || observations.packet_count != 1 ||
        observations.packet_bytes != 4 || observations.stream_info_count != 1 ||
        !ContainsState(observations,
                       MediaStreamSession::State::KRECONNECTING)) {
        std::cerr << "Reconnect exhausted case assertions failed" << std::endl;
        return false;
    }

    std::cout << "Reconnect exhausted: packets=" << stats.packets_received
              << ", reconnect_attempts=" << stats.reconnect_count << std::endl;
    return true;
}

bool RunExponentialBackoffTest() {
    // 初始连接成功后立刻读到 RetryableError。第一次重连 Open() 失败，
    // 第二次重连 Open() 成功并 EOS，从而得到两段可测量的等待时间。
    std::vector<std::vector<PullReadResult>> scripts;
    scripts.push_back({
        MakeStatusResult(
            PullReadStatus::RetryableError,
            PullErrorCategory::Network,
            true,
            "backoff first connection lost"),
    });
    scripts.push_back({
        MakeStatusResult(
            PullReadStatus::EOS,
            PullErrorCategory::EndOfInput,
            false,
            "backoff reconnected input ended"),
    });

    const auto open_history = std::make_shared<OpenHistory>();
    boost::asio::io_context io;
    auto session = std::make_shared<MediaStreamSession>(io);
    session->SetPuller(std::make_unique<ReconnectPuller>(
        std::vector<bool>{true, false, true},
        std::move(scripts),
        open_history));

    InputEndpointConfig endpoint;
    endpoint.uri = "scripted://reconnect-backoff";
    endpoint.puller_kind = PullerKind::FFmpeg;
    session->SetEndpoint(endpoint);

    SessionConfig config;
    config.reconnect.enabled = true;
    config.reconnect.initial_delay = std::chrono::milliseconds(50);
    config.reconnect.multiplier = 2.0;
    config.reconnect.max_delay = std::chrono::milliseconds(200);
    config.reconnect.max_attempts = 2;
    session->SetSessionConfig(config);

    Observations observations;
    AttachCallbacks(session, observations);
    if (!session->Start() || !WaitForTerminal(session, observations)) {
        return false;
    }

    session->Stop();
    const MediaStreamSession::Stats stats = session->GetStats();
    const auto open_times = SnapshotOpenTimes(open_history);
    if (*observations.terminal_state != MediaStreamSession::State::KSTOPPED ||
        stats.reconnect_count != 2 || open_times.size() != 3) {
        std::cerr << "Exponential backoff setup assertions failed" << std::endl;
        return false;
    }

    const auto first_delay = std::chrono::duration_cast<std::chrono::milliseconds>(
        open_times[1] - open_times[0]);
    const auto second_delay = std::chrono::duration_cast<std::chrono::milliseconds>(
        open_times[2] - open_times[1]);

    // 调度本身存在少量误差，因此验证下界：首次约 25ms，第二次按 2 倍
    // 计算本应为 50ms，但被 max_delay 截断到约 40ms。
    if (first_delay < std::chrono::milliseconds(15) ||
        second_delay < std::chrono::milliseconds(30)) {
        std::cerr << "Exponential backoff delays were too short: first="
                  << first_delay.count() << "ms, second="
                  << second_delay.count() << "ms" << std::endl;
        return false;
    }

    std::cout << "Exponential backoff: first=" << first_delay.count()
              << "ms, second=" << second_delay.count() << "ms" << std::endl;
    return true;
}

bool RunStopDuringBackoffTest() {
    std::vector<std::vector<PullReadResult>> scripts;
    scripts.push_back({
        MakeStatusResult(
            PullReadStatus::RetryableError,
            PullErrorCategory::Network,
            true,
            "stop during backoff"),
    });

    boost::asio::io_context io;
    auto session = std::make_shared<MediaStreamSession>(io);
    session->SetPuller(std::make_unique<ReconnectPuller>(
        std::vector<bool>{true}, std::move(scripts)));

    InputEndpointConfig endpoint;
    endpoint.uri = "scripted://stop-during-backoff";
    endpoint.puller_kind = PullerKind::FFmpeg;
    session->SetEndpoint(endpoint);

    SessionConfig config;
    config.reconnect.enabled = true;
    config.reconnect.initial_delay = std::chrono::milliseconds(750);
    config.reconnect.max_attempts = 1;
    session->SetSessionConfig(config);

    Observations observations;
    AttachCallbacks(session, observations);
    if (!session->Start() ||
        !WaitForState(observations, MediaStreamSession::State::KRECONNECTING)) {
        std::cerr << "Stop-during-backoff did not enter RECONNECTING"
                  << std::endl;
        session->Stop();
        return false;
    }

    const auto stop_start = std::chrono::steady_clock::now();
    session->Stop();
    const auto stop_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stop_start);
    const MediaStreamSession::Stats stats = session->GetStats();

    // 若仍使用 sleep_for(750ms)，Stop() 会明显超过该阈值；条件变量被
    // Stop() 唤醒后，应在一个较小调度窗口内完成。
    if (stop_duration >= std::chrono::milliseconds(250) ||
        stats.reconnect_count != 1) {
        std::cerr << "Stop did not interrupt reconnect backoff: duration="
                  << stop_duration.count() << "ms" << std::endl;
        return false;
    }

    std::cout << "Stop interrupted reconnect backoff in "
              << stop_duration.count() << "ms" << std::endl;
    return true;
}

bool RunStableResetTest() {
    // 第 1 条连接立即断开；第 2 条连接先保持一段稳定时间，再断开；
    // 第 3 条连接正常 EOS。若稳定重置生效，第 2 次断开仍可使用一次
    // 重连预算并成功建立第 3 条连接。
    std::vector<std::vector<PullReadResult>> scripts;
    scripts.push_back({
        MakeStatusResult(
            PullReadStatus::RetryableError,
            PullErrorCategory::Network,
            true,
            "stable reset first connection lost"),
    });

    std::vector<PullReadResult> stable_connection;
    for (int index = 0; index < 6; ++index) {
        stable_connection.push_back(MakeNoDataResult());
    }
    stable_connection.push_back(MakeStatusResult(
        PullReadStatus::RetryableError,
        PullErrorCategory::Network,
        true,
        "stable reset second connection lost"));
    scripts.push_back(std::move(stable_connection));

    scripts.push_back({
        MakeStatusResult(
            PullReadStatus::EOS,
            PullErrorCategory::EndOfInput,
            false,
            "stable reset input ended"),
    });

    boost::asio::io_context io;
    auto session = std::make_shared<MediaStreamSession>(io);
    session->SetPuller(std::make_unique<ReconnectPuller>(
        std::vector<bool>{true, true, true}, std::move(scripts)));

    InputEndpointConfig endpoint;
    endpoint.uri = "scripted://stable-reset";
    endpoint.puller_kind = PullerKind::FFmpeg;
    session->SetEndpoint(endpoint);

    SessionConfig config;
    config.reconnect.enabled = true;
    config.reconnect.initial_delay = std::chrono::milliseconds(0);
    config.reconnect.max_attempts = 1;
    config.reconnect.reset_after_stable = std::chrono::milliseconds(50);
    config.no_data_backoff = std::chrono::milliseconds(15);
    session->SetSessionConfig(config);

    Observations observations;
    AttachCallbacks(session, observations);
    if (!session->Start() || !WaitForTerminal(session, observations)) {
        return false;
    }

    session->Stop();
    const MediaStreamSession::Stats stats = session->GetStats();

    if (*observations.terminal_state != MediaStreamSession::State::KSTOPPED ||
        stats.reconnect_count != 2 || observations.stream_info_count != 3 ||
        !ContainsState(observations,
                       MediaStreamSession::State::KRECONNECTING)) {
        std::cerr << "Stable reconnect budget reset assertions failed"
                  << std::endl;
        return false;
    }

    std::cout << "Stable reconnect budget reset: reconnect_attempts="
              << stats.reconnect_count << ", stream_info_callbacks="
              << observations.stream_info_count << std::endl;
    return true;
}

}  // namespace

int main() {
    //if (!RunReconnectSuccessTest()) {
    //    return 1;
    //}
    //if (!RunReconnectExhaustedTest()) {
    //    return 1;
    //}
    if (!RunExponentialBackoffTest()) {
        return 1;
    }
    if (!RunStopDuringBackoffTest()) {
        return 1;
    }
    if (!RunStableResetTest()) {
        return 1;
    }

    std::cout << "MediaStreamSession reconnect tests passed" << std::endl;
    return 0;
}
