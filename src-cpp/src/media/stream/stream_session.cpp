#include "media/stream/stream_session.h"

#include <algorithm>
#include <chrono>
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
            // Setter 只允许在 Session 未运行时修改配置，因此在这里复制一份
            // 快照，避免重连过程中反复读取可变配置。
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

        // 本阶段先使用固定的 initial_delay。multiplier、max_delay 和
        // reset_after_stable 留到后续退避策略阶段实现。
        if (reconnect_config.initial_delay.count() > 0) {
            std::this_thread::sleep_for(reconnect_config.initial_delay);
        }
        if (!running_.load()) {
            return false;
        }

        // ReadPacket() 已经返回，因此此时可以关闭上一代底层连接，再用
        // 同一个 Puller 和同一个 endpoint 建立新的连接。
        puller_->Close();
        const PullOpenResult open_result = puller_->Open(endpoint);

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

void MediaStreamSession::Stop() {
    std::thread thread_to_join;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        running_.store(false);
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

    while (running_.load()) {
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
