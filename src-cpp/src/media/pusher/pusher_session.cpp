#include "media/pusher/pusher_session.h"

#include <limits>
#include <memory>
#include <utility>

#include "media/pusher/ffmpeg_pusher.h"

namespace {
/// @brief 从 packet 中提取最早的 timestamp 值，包括 pts 和 dts
std::optional<std::int64_t> EarliestTimestamp(const MediaPacket& packet) {
    std::optional<std::int64_t> earliest;
    // 优先考虑 pts，再考虑 dts
    if (IsValidTimestamp(packet.pts)) earliest = packet.pts;
    if (IsValidTimestamp(packet.dts) && (!earliest.has_value() || packet.dts < *earliest)) {
        earliest = packet.dts;
    }
    return earliest;
}

bool SameTimeBase(const Rational& left, const Rational& right) {
    return static_cast<std::int64_t>(left.num) * right.den ==
           static_cast<std::int64_t>(right.num) * left.den;
}

/// @brief 检查是否可以安全地从 timestamp 中减去 offset，而不会导致溢出或下溢
/// @param value 输入的 timestamp 值
/// @param offset 要减去的偏移量
/// @return 如果可以安全地减去 offset，返回 true；否则返回 false
bool CanSubtractTimestamp(std::int64_t value, std::int64_t offset) {
    const auto minimum = (std::numeric_limits<std::int64_t>::min)();
    const auto maximum = (std::numeric_limits<std::int64_t>::max)();
    return (offset > 0 && value >= minimum + offset) ||
           (offset < 0 && value <= maximum + offset) || offset == 0;
}

}  // namespace

PusherSession::PusherSession() : PusherSession(std::make_unique<FFmpegPusher>()) {    

}

PusherSession::PusherSession(std::unique_ptr<IPusher> pusher) : pusher_(std::move(pusher)) {
    if (pusher_) {
        pusher_->SetEventCallback([this](const PusherEvent& event) {
            std::lock_guard<std::mutex> lock(event_mutex_);
            // 如果事件队列已停止或未激活，直接返回。
            if (event_thread_stopping_ || !event_active_) {
                return;
            }
            event_queue_.push(QueuedEvent{event, event_generation_});
        });
    }
    event_thread_ = std::thread(&PusherSession::eventLoop, this);
}

PusherSession::~PusherSession() {
    Close();
    if (pusher_) {
        pusher_->SetEventCallback({});
    }

    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_thread_stopping_ = true;
        event_queue_.push(std::nullopt);  // 发送 nullopt 通知事件线程退出
    }
    event_thread_.join();
}

PusherError PusherSession::MakeError(PusherErrorCategory category, const char* message) {
    return PusherError{category, message, false};
}

PusherResult PusherSession::Open(const PusherSessionConfig& config) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    // 一个 Session 同一时刻只管理一个输出会话。
    // 重新打开时先完成旧容器的 trailer 写入，避免旧容器与新容器共用同一个输出上下文。
    if (state_ != PusherSessionState::Closed) {
        const PusherResult close_result = Close();
        if (!close_result.Succeed()) {
            return close_result;
        }
    }

    if (!pusher_) {
        state_ = PusherSessionState::Failed;
        return PusherResult::Failed(MakeError(PusherErrorCategory::Internal,
            "PusherSession has no pusher implementation"));
    }

    if (!config.is_valid()) {
        return PusherResult::Failed(MakeError(PusherErrorCategory::InvalidConfiguration,
            "PusherSession received an invalid output configuration"));
    }

    stop_requested_.store(false);
    {
        std::lock_guard<std::mutex> event_lock(event_mutex_);
        ++event_generation_;
        event_active_ = true;
    }
    last_event_.reset();
    const PusherResult open_result = pusher_->Open(config.pusher);
    if (!open_result.Succeed()) {
        {
            std::lock_guard<std::mutex> event_lock(event_mutex_);
            event_active_ = false;
            ++event_generation_;
        }
        state_ = PusherSessionState::Failed;
        return open_result;
    }
    // 输出容器已经准备好，但任意 P/B 帧都不能作为新视频的起点。先等待
    // IDR 等关键帧，确保 MP4 和后续网络输出可以独立解码。
    state_ = PusherSessionState::WaitingForKeyframe;
    if (stop_requested_.load()) {
        state_ = PusherSessionState::Failed;
        return PusherResult::Failed(MakeError(PusherErrorCategory::Cancelled, "PusherSession stopped during Open"));
    }

    timestamp_policy_ = config.timestamp_policy;
    timestamp_epoch_.reset();
    timestamp_time_base_ = Rational{config.pusher.video_track.time_base_num,
                                    config.pusher.video_track.time_base_den};
    return PusherResult::Success();
}

PusherPublishResult PusherSession::Publish(const MediaPacket& packet) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    if (stop_requested_.load()) {
        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::Cancelled,
            "PusherSession has been asked to stop"));
    }
    if (state_ == PusherSessionState::Closed) {
        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::InvalidState,
            "PusherSession::Publish called before a successful Open"));
    }

    if (state_ == PusherSessionState::Failed) {
        if (last_event_.has_value()) {
            return PusherPublishResult::Failed(last_event_->error);
        }
        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::InvalidState,
            "PusherSession is failed; close and open it before publishing again"));
    }

    // 初版 session 只建立一条 H.264 视频轨道。
    // 这里先拒绝不属于该轨道的包，避免音频或未知格式被“等待关键帧”策略静默吞掉。
    if (packet.type != MediaType::VIDEO || packet.codec != CodecType::H264) {
        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::UnsupportedMedia,
            "PusherSession currently accepts H264 video packets only"));
    }

    if (state_ == PusherSessionState::WaitingForKeyframe && !packet.keyframe) {
        // 这是会话预期行为：这里不调用 Pusher，也不消费 AVPacket。上游仍可
        // 正常释放 packet；Session 只记录“当前输出尚不能从该包开始”。
        return PusherPublishResult::DroppedAwaitingKeyframe();
    }

    return forwardAcceptedPacket(packet);
}

PusherPublishResult PusherSession::forwardAcceptedPacket(const MediaPacket& packet) {
    if (!pusher_) {
        state_ = PusherSessionState::Failed;
        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::Internal,
            "PusherSession has no pusher implementation"));
    }

    // 不改写上游拥有的 MediaPacket 元数据。Pusher/Muxer 会消费其后端
    // AVPacket，但 Session 的时间轴变换只存在于这次输出使用的副本中。
    MediaPacket output_packet = packet;
    const PusherResult timestamp_result = applyPusherTimestampPolicy(output_packet);
    if (!timestamp_result.Succeed()) {
        if (timestamp_result.error.has_value()) {
            return PusherPublishResult::Failed(*timestamp_result.error);
        }
        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::Internal,
            "Timestamp policy returned a failed result without an error"));
    }

    const PusherResult push_result = pusher_->Push(output_packet);
    if (!push_result.Succeed()) {
        // InvalidPacket 等调用方输入错误不会破坏已经打开的输出容器，等待
        // 下一包仍然合理；底层超时、网络、取消等错误均使当前会话失效。
        if (push_result.error.has_value() &&
            push_result.error->category != PusherErrorCategory::InvalidPacket &&
            push_result.error->category != PusherErrorCategory::UnsupportedMedia) {
            state_ = PusherSessionState::Failed;
        }

        if (push_result.error.has_value()) {
            return PusherPublishResult::Failed(*push_result.error);
        }

        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::Internal,
            "Pusher returned a failed result without an error"));
    }
    if (stop_requested_.load()) {
        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::Cancelled,
            "PusherSession stopped during Publish"));
    }
    if (state_ == PusherSessionState::Failed) {
        return PusherPublishResult::Failed(last_event_->error);
    }

    if (state_ == PusherSessionState::WaitingForKeyframe) {
        // 对尚未建立 epoch 的 StartAtZero 会话，只有底层 Pusher 确认接收
        // 这个首包后才能提交 epoch。若首包写入失败且 Session 仍可继续，
        // 下一包必须以自己的时间戳重新建立起点。
        if (timestamp_policy_.mode == PusherTimestampMode::StartAtZero && !timestamp_epoch_.has_value()) {
            timestamp_epoch_ = EarliestTimestamp(packet);
        }

        // 只有关键帧已经被底层 Pusher 成功接收，才能认为输出流真正启动。
        // 如果写入失败，状态会保留 WaitingForKeyframe 或转为 Failed，而不
        // 会出现“尚未写入关键帧却已经 Running”的状态错乱。
        state_ = PusherSessionState::Running;
    }

    return PusherPublishResult::Published();
}

PusherResult PusherSession::applyPusherTimestampPolicy(MediaPacket& packet) {
    if (timestamp_policy_.mode == PusherTimestampMode::Preserved) {
        return PusherResult::Success();
    }

    // epoch 是一个整数 tick，只有在统一时间基下才有确定含义。当前单轨
    // Session 因此要求 StartAtZero 的 packet 使用 Open 时声明的轨道时间基；
    // 多时基转换应由未来的 Session 时间轴转换器显式完成，不能在此静默取整。
    if (!timestamp_time_base_.has_value() || !SameTimeBase(packet.time_base, *timestamp_time_base_)) {
        return PusherResult::Failed(MakeError(PusherErrorCategory::InvalidPacket,
            "StartAtZero packet time base differs from the session track time base"));
    }

    std::optional<std::int64_t> epoch = timestamp_epoch_;
    if (!epoch.has_value()) {
        epoch = EarliestTimestamp(packet);
        if (!epoch.has_value()) {
            return PusherResult::Failed(MakeError(PusherErrorCategory::InvalidPacket,
                "StartAtZero requires PTS or DTS on the first accepted packet"));
        }
    }

    // 调整 packet 时间戳，使它们相对于 epoch 开始。
    const std::int64_t offset = *epoch;
    const auto adjust = [&](std::int64_t& timestamp) -> bool {
        if (!IsValidTimestamp(timestamp)) return true;
        if (!CanSubtractTimestamp(timestamp, offset)) return false;
        timestamp -= offset;
        return true;
    };
    if (!adjust(packet.pts) || !adjust(packet.dts)) {
        return PusherResult::Failed(MakeError(PusherErrorCategory::InvalidPacket,
            "Timestamp cannot be represented relative to the session epoch"));
    }
    return PusherResult::Success();
}

PusherResult PusherSession::Close() {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    {
        std::lock_guard<std::mutex> event_lock(event_mutex_);
        ++event_generation_;
        event_active_ = false;
    }
    if (!pusher_) {
        state_ = PusherSessionState::Closed;
        timestamp_epoch_.reset();
        timestamp_time_base_.reset();
        return PusherResult::Success();
    }

    // 当底层 Pusher 本来就没有打开时，不再重复调用 Close。这样析构、
    // 显式 Close 和失败清理可以安全叠加，又不会重复写输出容器 trailer。
    if (state_ == PusherSessionState::Closed && !pusher_->IsOpen()) {
        timestamp_epoch_.reset();
        timestamp_time_base_.reset();
        return PusherResult::Success();
    }

    const PusherResult close_result = pusher_->Close();
    state_ = close_result.Succeed() ? PusherSessionState::Closed : PusherSessionState::Failed;
    if (close_result.Succeed()) {
        timestamp_epoch_.reset();
        timestamp_time_base_.reset();
    }
    return close_result;
}

PusherResult PusherSession::RequestStop() {
    stop_requested_.store(true);
    if (!pusher_) {
        return PusherResult::Success();
    }
    return pusher_->RequestStop();
}

void PusherSession::eventLoop() {
    for (;;) {
        std::optional<QueuedEvent> queued;
        event_queue_.wait_pop(queued);
        if (!queued.has_value()) {
            return;  // 收到 nullopt 时，退出事件线程
        }

        {
            std::lock_guard<std::mutex> lock(event_mutex_);

            // 在等待 operation_lock 之前，中断被阻塞的推送。
            if (event_active_ && queued->generation == event_generation_ && pusher_ &&
                queued->event.error.category != PusherErrorCategory::Cancelled) {
                pusher_->RequestStop();
            }
        }

        std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
        std::lock_guard<std::mutex> event_lock(event_mutex_);
        if (!event_active_ || queued->generation != event_generation_) {
            continue;  // 事件代际过期不匹配，跳过
        }
        // 更新事件记录和状态
        last_event_ = std::move(queued->event);
        if (state_ != PusherSessionState::Closed &&
            !(stop_requested_.load() && last_event_->error.category == PusherErrorCategory::Cancelled)) {
            state_ = PusherSessionState::Failed;
        }
    }
}

PusherSessionState PusherSession::State() const {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    return state_;
}

std::optional<PusherEvent> PusherSession::LastEvent() const {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    return last_event_;
}
