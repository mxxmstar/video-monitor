#include "media/stream/stream_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

#include "common/log/logger.h"

namespace {

const char* StateNameImpl(MediaStreamSession::State state) {
    switch (state) {
        case MediaStreamSession::State::KIDLE:         return "IDLE";
        case MediaStreamSession::State::KCONNECTING:   return "CONNECTING";
        case MediaStreamSession::State::KCONNECTED:    return "CONNECTED";
        case MediaStreamSession::State::KRECONNECTING: return "RECONNECTING";
        case MediaStreamSession::State::KSTOPPED:      return "STOPPED";
        case MediaStreamSession::State::KERROR:        return "ERROR";
    }
    return "UNKNOWN";
}

}  // namespace

MediaStreamSession::MediaStreamSession(boost::asio::io_context& io) : io_(io) {
}

MediaStreamSession::~MediaStreamSession() {
    Stop();
}

void MediaStreamSession::SetPuller(std::unique_ptr<IPuller> puller) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!running_) {
        puller_ = std::move(puller);
    }
}

void MediaStreamSession::SetEndpoint(const InputEndpointConfig& endpoint) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!running_) {
        endpoint_ = endpoint;
    }
}

void MediaStreamSession::SetSessionConfig(const SessionConfig& config) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!running_) {
        session_config_ = config;
    }
}

void MediaStreamSession::SetUrl(const std::string& url) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!running_) {
        endpoint_.uri = url;
    }
}

void MediaStreamSession::SetReconnectIntervalMs(int ms) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    session_config_.reconnect.initial_delay = std::chrono::milliseconds(std::max(ms, 0));
}

void MediaStreamSession::SetMaxReconnectCount(int count) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    session_config_.reconnect.max_attempts = count;
}

void MediaStreamSession::SetJitterBufferIntervalMs(int ms) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    session_config_.jitter.enabled = ms > 0;
    if (ms > 0) {
        session_config_.jitter.release_interval = std::chrono::milliseconds(ms);
    }
}

void MediaStreamSession::SetJitterBufferConfig(
    const AdaptiveJitterBuffer::Config& config) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    session_config_.jitter.enabled = true;
    session_config_.jitter.capacity_packets = config.capacity_packets;
    session_config_.jitter.min_delay = std::chrono::milliseconds(
        static_cast<int>(std::max(config.min_delay_ms, 0.0)));
    session_config_.jitter.max_delay = std::chrono::milliseconds(
        static_cast<int>(std::max(config.max_delay_ms, 0.0)));
    session_config_.jitter.safety_margin = std::chrono::milliseconds(
        static_cast<int>(std::max(config.safety_margin_ms, 0.0)));
    session_config_.jitter.alpha = config.alpha;
}

void MediaStreamSession::SetWatchdogIntervalMs(int ms) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    session_config_.watchdog.enabled = ms > 0;
    if (ms > 0) {
        session_config_.watchdog.check_interval = std::chrono::milliseconds(ms);
    }
}

void MediaStreamSession::SetPacketCallback(PacketCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    packet_cb_ = std::move(cb);
}

void MediaStreamSession::SetStreamInfoCallback(StreamInfoCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    streaminfo_cb_ = std::move(cb);
}

void MediaStreamSession::SetStateCallback(StateCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    state_cb_ = std::move(cb);
}

bool MediaStreamSession::Start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_ || !puller_ || endpoint_.uri.empty()) {
        return false;
    }

    setState(State::KCONNECTING);
    const PullOpenResult result = puller_->Open(endpoint_);
    if (!result.Succeed()) {
        setState(State::KERROR);
        if (result.error.has_value()) {
            LOG_ERROR("Session open failed: {}", result.error->message);
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_ = {};
    }
    running_.store(true);
    setState(State::KCONNECTED);

    notifyStreamInfo();

    read_thread_ = std::thread(&MediaStreamSession::readLoop, this);
    return true;
}

void MediaStreamSession::notifyStreamInfo() {
    const MultiStreamInfo stream_info = puller_->GetStreamInfo();
    StreamInfoCallback stream_info_callback;
    {
        std::lock_guard<std::mutex> callback_lock(cb_mutex_);
        stream_info_callback = streaminfo_cb_;
    }
    if (stream_info_callback) {
        stream_info_callback(stream_info);
    }
}

bool MediaStreamSession::tryReconnect(int& reconnect_attempts) {
    while (running_.load()) {
        ReconnectPolicy reconnect_config;
        InputEndpointConfig endpoint;
        {
            // 将本次尝试需要的配置复制为快照，保证一次退避计算和 Open()
            // 使用同一份 endpoint / 重连策略。
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            if (!running_.load()) {
                return false;
            }
            reconnect_config = session_config_.reconnect;
            endpoint = endpoint_;
        }

        if (!reconnect_config.enabled ||
            (reconnect_config.max_attempts >= 0 &&
             reconnect_attempts >= reconnect_config.max_attempts)) {
            return false;
        }

        setState(State::KRECONNECTING);
        ++reconnect_attempts;
        {
            // 统计的是每次重新 Open 的尝试，而不是只有成功连接才计数。
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            ++stats_.reconnect_count;
        }

        const std::chrono::milliseconds delay =
            reconnectDelay(reconnect_config, reconnect_attempts);
        if (!waitForReconnectDelay(delay)) {
            return false;
        }

        // ReadPacket() 已经返回，因此此时可以关闭上一代底层连接，再用
        // 同一个 Puller 和同一个 endpoint 建立新的连接。与 Stop() 共用
        // lifecycle_mutex_，避免 Close() 和 Open() 并发访问同一个 Puller。
        PullOpenResult open_result;
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            if (!running_.load()) {
                return false;
            }
            puller_->Close();
            open_result = puller_->Open(endpoint);
        }

        // Stop() 可能与 Open() 并发发生。若 Stop 已经生效，不能再发布
        // 重连成功或 StreamInfo，立即释放刚打开的资源。
        if (!running_.load()) {
            puller_->Close();
            return false;
        }

        if (open_result.Succeed()) {
            setState(State::KCONNECTED);
            notifyStreamInfo();
            return true;
        }

        if (open_result.error.has_value()) {
            LOG_WARN("Session reconnect attempt {} failed: {}",
                     reconnect_attempts,
                     open_result.error->message);
        } else {
            LOG_WARN("Session reconnect attempt {} failed without error detail",
                     reconnect_attempts);
        }

        // Open 失败只有在明确标记为可重试时才继续消耗重连预算；认证、
        // 配置和媒体格式等不可恢复错误直接交给 readLoop 转为 KERROR。
        if (!open_result.error.has_value() || !open_result.error->retryable) {
            return false;
        }
    }

    return false;
}

std::chrono::milliseconds MediaStreamSession::reconnectDelay(
    const ReconnectPolicy& config, int reconnect_attempt) {
    if (config.initial_delay.count() <= 0) {
        return std::chrono::milliseconds::zero();
    }

    // 第一次重连等待 initial_delay；第二次及以后再逐次乘 multiplier。
    // 非法或小于 1 的 multiplier 会退化为 1，保证延迟不会因错误配置变小。
    const double multiplier = std::isfinite(config.multiplier)
        ? std::max(config.multiplier, 1.0)
        : 1.0;
    const int multiplier_count = std::max(reconnect_attempt - 1, 0);

    long double delay_ms = static_cast<long double>(config.initial_delay.count());
    const long double max_duration = static_cast<long double>(
        (std::numeric_limits<std::chrono::milliseconds::rep>::max)());
    for (int index = 0; index < multiplier_count && delay_ms < max_duration;
         ++index) {
        delay_ms = std::min(delay_ms * multiplier, max_duration);
    }

    // max_delay 为正时才启用上限；非正值按“不额外封顶”处理，避免把
    // 误配的 0 解释成完全跳过等待。
    if (config.max_delay.count() > 0) {
        delay_ms = std::min(
            delay_ms,
            static_cast<long double>(config.max_delay.count()));
    }

    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(delay_ms));
}

bool MediaStreamSession::waitForReconnectDelay(
    std::chrono::milliseconds delay) {
    if (delay.count() <= 0) {
        return running_.load();
    }

    std::unique_lock<std::mutex> lock(reconnect_wait_mutex_);
    const bool stopped = reconnect_wait_cv_.wait_for(
        lock,
        delay,
        [this] {
            return !running_.load();
        });
    return !stopped;
}

void MediaStreamSession::Stop() {
    // 先修改原子状态并唤醒退避等待。不能等到拿到 lifecycle_mutex_ 后
    // 再通知，否则 Open() 期间的锁竞争会推迟等待线程退出。
    running_.store(false);
    reconnect_wait_cv_.notify_all();

    std::thread thread_to_join;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (puller_) {
            puller_->Close();
        }
        thread_to_join = std::move(read_thread_);
    }

    if (thread_to_join.joinable()) {
        if (thread_to_join.get_id() == std::this_thread::get_id()) {
            thread_to_join.detach();
        } else {
            thread_to_join.join();
        }
    }
    setState(State::KSTOPPED);
}

void MediaStreamSession::readLoop() {
    int reconnect_attempts = 0;
    auto connected_since = std::chrono::steady_clock::now();

    while (running_.load()) {
        // reconnect_attempts 是当前稳定连接周期内的重连预算。连接持续
        // 稳定达到 reset_after_stable 后，只清零本地预算，不清零累计统计。
        ReconnectPolicy reconnect_config;
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            reconnect_config = session_config_.reconnect;
        }
        const auto now = std::chrono::steady_clock::now();
        if (reconnect_attempts > 0 &&
            reconnect_config.reset_after_stable.count() > 0 &&
            now - connected_since >= reconnect_config.reset_after_stable) {
            reconnect_attempts = 0;
            connected_since = now;
            LOG_INFO("Session reconnect budget reset after stable connection");
        }

        const PullReadResult result = puller_->ReadPacket();

        if (!running_.load()) {
            break;
        }

        switch (result.status) {
            case PullReadStatus::Packet: {
                if (!result.packet || !result.packet->buffer) {
                    running_.store(false);
                    setState(State::KERROR);
                    break;
                }

                {
                    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                    ++stats_.packets_received;
                    stats_.bytes_received += result.packet->buffer->Size();
                }

                PacketCallback packet_callback;
                {
                    std::lock_guard<std::mutex> callback_lock(cb_mutex_);
                    packet_callback = packet_cb_;
                }
                if (packet_callback) {
                    packet_callback(result.packet);
                }
                break;
            }
            case PullReadStatus::NoData:
                std::this_thread::sleep_for(session_config_.no_data_backoff);
                break;
            case PullReadStatus::EOS:
                running_.store(false);
                setState(State::KSTOPPED);
                break;
            case PullReadStatus::Stopped:
                running_.store(false);
                setState(State::KSTOPPED);
                break;
            case PullReadStatus::RetryableError: {
                const bool retryable =
                    result.error.has_value() && result.error->retryable;
                if (retryable && tryReconnect(reconnect_attempts)) {
                    // tryReconnect() 返回 true 代表已经成功建立了新的
                    // 连接代次，稳定计时应从这一刻重新开始。
                    connected_since = std::chrono::steady_clock::now();
                    break;
                }

                if (!running_.load()) {
                    break;
                }

                running_.store(false);
                setState(State::KERROR);
                if (result.error.has_value()) {
                    LOG_ERROR("Session read failed: {}", result.error->message);
                } else {
                    LOG_ERROR("Session read returned RetryableError without "
                              "retryable error detail");
                }
                break;
            }
            case PullReadStatus::FatalError:
                running_.store(false);
                setState(State::KERROR);
                if (result.error.has_value()) {
                    LOG_ERROR("Session read failed: {}", result.error->message);
                }
                break;
        }
    }
}

MediaStreamSession::State MediaStreamSession::GetState() const {
    return state_.load();
}

MediaStreamSession::Stats MediaStreamSession::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void MediaStreamSession::setState(State state) {
    const State old_state = state_.exchange(state);
    if (old_state == state) {
        return;
    }

    StateCallback callback;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        callback = state_cb_;
    }
    if (callback) {
        callback(state);
    }
}

const char* MediaStreamSession::StateName(State state) {
    return StateNameImpl(state);
}
