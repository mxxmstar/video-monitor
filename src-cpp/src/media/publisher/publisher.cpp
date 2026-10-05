#include "media/publisher/publisher.h"

#include <algorithm>
#include <utility>

Publisher::Publisher() = default;

Publisher::Publisher(std::unique_ptr<PusherSession> session) : injected_sessions_(true) {
    sessions_.push_back({{}, std::move(session), nullptr});
}

Publisher::Publisher(std::unique_ptr<AsyncPusherSession> session) : injected_sessions_(true) {
    sessions_.push_back({{}, nullptr, std::move(session)});
}

Publisher::Publisher(std::vector<std::unique_ptr<PusherSession>> sessions) : injected_sessions_(true) {
    for (auto& session : sessions) {
        sessions_.push_back({{}, std::move(session), nullptr});
    }
}

Publisher::Publisher(std::vector<std::unique_ptr<AsyncPusherSession>> sessions) : injected_sessions_(true) {
    for (auto& session : sessions) {
        sessions_.push_back({{}, nullptr, std::move(session)});
    }
}

Publisher::~Publisher() {
    Close();
}

PusherError Publisher::MakeError(PusherErrorCategory category, const char* message) {
    return PusherError{category, message, false};
}

PusherError Publisher::ErrorWithTarget(PusherError error, const std::string& target_id) {
    error.message = "Publisher target '" + target_id + "': " + error.message;
    return error;
}

PusherResult Publisher::Open(const PublisherConfig& config) {
    if (!config.is_valid()) {
        return PusherResult::Failed(MakeError(PusherErrorCategory::InvalidConfiguration, "Publisher received an invalid output configuration"));
    }
    const auto mode = config.targets.front().session.mode;
    if (injected_sessions_ && (sessions_.size() != config.targets.size() ||
         (mode == PusherSessionMode::Synchronous && std::any_of(sessions_.begin(), sessions_.end(), [](const SessionEntry& entry) { return !entry.synchronous; })) ||
         (mode == PusherSessionMode::Asynchronous && std::any_of(sessions_.begin(), sessions_.end(), [](const SessionEntry& entry) { return !entry.asynchronous; })))) {
        return PusherResult::Failed(MakeError(PusherErrorCategory::InvalidConfiguration, "Publisher injected sessions do not match the configured targets and mode"));
    }
    const PusherResult close_result = Close();
    if (!close_result.Succeed()) {
        return close_result;
    }
    if (!injected_sessions_) {
        sessions_.clear();
        sessions_.reserve(config.targets.size());
        for (const auto& target : config.targets) {
            if (mode == PusherSessionMode::Asynchronous) {
                sessions_.push_back({target.target_id, nullptr, std::make_unique<AsyncPusherSession>()});
            } else {
                sessions_.push_back({target.target_id, std::make_unique<PusherSession>(), nullptr});
            }
        }
    }
    active_mode_ = mode;
    for (std::size_t i = 0; i < sessions_.size(); ++i) {
        auto& entry = sessions_[i];
        entry.target_id = config.targets[i].target_id;
        const auto& session_config = config.targets[i].session;
        PusherResult result = mode == PusherSessionMode::Asynchronous ? entry.asynchronous->Open(session_config) : entry.synchronous->Open(session_config);
        if (!result.Succeed()) {
            PusherError error = ErrorWithTarget(std::move(*result.error), entry.target_id);
            (void)Close();
            return PusherResult::Failed(std::move(error));
        }
    }
    opened_ = true;
    close_failed_ = false;
    return PusherResult::Success();
}

PusherPublishResult Publisher::Publish(const MediaPacket& packet) {
    if (!opened_ || active_mode_ != PusherSessionMode::Synchronous) {
        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::InvalidState, "Publisher is not open in synchronous mode"));
    }
    last_publish_results_.clear();  ///< 清空最近一次发布结果
    PusherPublishResult aggregate = PusherPublishResult::Published();
    for (auto& entry : sessions_) {
        PusherPublishResult result = entry.synchronous->Publish(packet);
        if (!result.Succeed() && aggregate.Succeed()) {
            // 有一个目标发布失败，聚合结果也失败
            aggregate = PusherPublishResult::Failed(ErrorWithTarget(*result.error, entry.target_id));
        } else if (result.status == PusherPublishStatus::DroppedAwaitingKeyframe && aggregate.status == PusherPublishStatus::Published) {
            // 有一个目标发布等待关键帧，聚合结果也等待关键帧
            aggregate = PusherPublishResult::DroppedAwaitingKeyframe();
        }
        last_publish_results_.push_back({entry.target_id, std::move(result)});
    }
    return aggregate;
}

PusherEnqueueResult Publisher::Enqueue(const MediaPacket& packet) {
    if (!opened_ || active_mode_ != PusherSessionMode::Asynchronous) {
        return PusherEnqueueResult::Rejected(MakeError(PusherErrorCategory::InvalidState, "Publisher is not open in asynchronous mode"));
    }
    last_enqueue_results_.clear();  ///< 清空最近一次入队结果
    PusherEnqueueResult aggregate = PusherEnqueueResult::AcceptedResult();
    for (auto& entry : sessions_) {
        PusherEnqueueResult result = entry.asynchronous->Enqueue(packet);
        if (result.status == PusherEnqueueStatus::Rejected && aggregate.status != PusherEnqueueStatus::Rejected) {
            // 有一个目标入队失败，聚合结果也失败
            aggregate = PusherEnqueueResult::Rejected(ErrorWithTarget(*result.error, entry.target_id));
        } else if (result.status == PusherEnqueueStatus::QueueFull && aggregate.Succeed()) {
            // 有一个目标入队队列已满，聚合结果也失败
            aggregate = PusherEnqueueResult::QueueFull();
            aggregate.error = PusherError{PusherErrorCategory::InvalidState, "Publisher target '" + entry.target_id + "' queue is full", false};
        } else if (result.status == PusherEnqueueStatus::DroppedAwaitingKeyframe && aggregate.status == PusherEnqueueStatus::Accepted) {
            // 有一个目标入队等待关键帧，聚合结果也等待关键帧
            aggregate = PusherEnqueueResult::DroppedAwaitingKeyframe();
        }
        last_enqueue_results_.push_back({entry.target_id, std::move(result)});
    }
    return aggregate;
}

PusherResult Publisher::Close() {
    PusherResult aggregate = PusherResult::Success();
    for (auto& entry : sessions_) {
        PusherResult result = entry.asynchronous ? entry.asynchronous->Close() : entry.synchronous->Close();
        if (!result.Succeed() && aggregate.Succeed()) {
            aggregate = PusherResult::Failed(ErrorWithTarget(*result.error, entry.target_id));
        }
    }
    opened_ = false;
    close_failed_ = !aggregate.Succeed();
    last_publish_results_.clear();
    last_enqueue_results_.clear();
    return aggregate;
}

std::vector<Publisher::TargetState> Publisher::TargetStates() const {
    std::vector<TargetState> states;
    states.reserve(sessions_.size());
    for (const auto& entry : sessions_) {
        states.push_back({entry.target_id, entry.asynchronous ? entry.asynchronous->State() : entry.synchronous->State()});
    }
    return states;
}

PublisherState Publisher::State() const noexcept {
    if (close_failed_) {
        return PublisherState::Failed;
    }
    if (!opened_) {
        return PublisherState::Closed;
    }
    bool waiting = false;
    for (const auto& entry : sessions_) {
        const auto state = entry.asynchronous ? entry.asynchronous->State() : entry.synchronous->State();
        if (state == PusherSessionState::Failed || state == PusherSessionState::Closed) {
            return PublisherState::Failed;
        }
        waiting |= state == PusherSessionState::WaitingForKeyframe;
    }
    return waiting ? PublisherState::WaitingForKeyframe : PublisherState::Running;
}

PusherResult Publisher::RequestStop() {
    PusherResult aggregate = PusherResult::Success();
    for (auto& entry : sessions_) {
        PusherResult result = entry.asynchronous ? entry.asynchronous->RequestStop() : entry.synchronous->RequestStop();
        if (!result.Succeed() && aggregate.Succeed()) {
            aggregate = PusherResult::Failed(ErrorWithTarget(*result.error, entry.target_id));
        }
    }
    return aggregate;
}
