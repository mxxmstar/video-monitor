#include "media/publisher/publisher.h"

#include <memory>
#include <utility>

Publisher::Publisher() = default;

Publisher::Publisher(std::unique_ptr<PusherSession> session)
    : session_(std::move(session)) {}

Publisher::Publisher(std::unique_ptr<AsyncPusherSession> session)
    : async_session_(std::move(session)) {}

Publisher::~Publisher() {
    Close();
}

PusherError Publisher::MakeError(PusherErrorCategory category, const char* message) {
    return PusherError{category, message, false};
}

PusherResult Publisher::Open(const PublisherConfig& config) {
    if (!config.is_valid()) {
        return PusherResult::Failed(MakeError(PusherErrorCategory::InvalidConfiguration,
            "Publisher received an invalid output configuration"));
    }

    // 类型检查保留在门面层；当前 Client 是唯一已实现的发布路线。
    // Session 的同步/异步选择只由下面的 session.mode 配置决定。
    if (config.kind != PublisherKind::Client) {
        return PusherResult::Failed(MakeError(PusherErrorCategory::InvalidConfiguration,
            "Publisher does not support the requested output kind"));
    }

    const PusherResult close_result = Close();
    if (!close_result.Succeed()) {
        return close_result;
    }
    active_mode_ = config.session.mode;
    if (active_mode_ == PusherSessionMode::Asynchronous) {
        if (!async_session_) {
            async_session_ = std::make_unique<AsyncPusherSession>();
        }
        return async_session_->Open(config.session);
    }
    if (!session_) {
        session_ = std::make_unique<PusherSession>();
    }
    return session_->Open(config.session);
}

PusherPublishResult Publisher::Publish(const MediaPacket& packet) {
    if (active_mode_ == PusherSessionMode::Asynchronous) {
        return PusherPublishResult::Failed(MakeError(
            PusherErrorCategory::InvalidState,
            "Publisher is asynchronous; use Enqueue instead of Publish"));
    }
    if (!session_) {
        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::Internal,
            "Publisher has no PusherSession implementation"));
    }

    // 关键帧等待、包校验和底层写入错误都由 Session 返回。Publisher 不应
    // 复制这套策略，否则不同输出目标的行为会开始分叉。
    return session_->Publish(packet);
}

PusherEnqueueResult Publisher::Enqueue(const MediaPacket& packet) {
    if (active_mode_ != PusherSessionMode::Asynchronous) {
        return PusherEnqueueResult::Rejected(MakeError(
            PusherErrorCategory::InvalidState,
            "Publisher is synchronous; use Publish instead of Enqueue"));
    }
    if (!async_session_) {
        return PusherEnqueueResult::Rejected(MakeError(
            PusherErrorCategory::Internal,
            "Publisher has no asynchronous PusherSession implementation"));
    }
    return async_session_->Enqueue(packet);
}

PusherResult Publisher::Close() {
    if (active_mode_ == PusherSessionMode::Asynchronous) {
        if (!async_session_) {
            return PusherResult::Success();
        }
        return async_session_->Close();
    }
    if (!session_) {
        return PusherResult::Success();
    }
    return session_->Close();
}

PublisherState Publisher::State() const noexcept {
    if (active_mode_ == PusherSessionMode::Asynchronous) {
        if (!async_session_) {
            return PublisherState::Closed;
        }
        switch (async_session_->State()) {
        case PusherSessionState::Closed:
            return PublisherState::Closed;
        case PusherSessionState::WaitingForKeyframe:
            return PublisherState::WaitingForKeyframe;
        case PusherSessionState::Running:
            return PublisherState::Running;
        case PusherSessionState::Failed:
            return PublisherState::Failed;
        }
        return PublisherState::Failed;
    }
    if (!session_) {
        return PublisherState::Closed;
    }

    switch (session_->State()) {
    case PusherSessionState::Closed:
        return PublisherState::Closed;
    case PusherSessionState::WaitingForKeyframe:
        return PublisherState::WaitingForKeyframe;
    case PusherSessionState::Running:
        return PublisherState::Running;
    case PusherSessionState::Failed:
        return PublisherState::Failed;
    }
    
    return PublisherState::Failed;
}

PusherResult Publisher::RequestStop() {
    if (active_mode_ == PusherSessionMode::Asynchronous) {
        if (!async_session_) {
            return PusherResult::Failed(MakeError(PusherErrorCategory::Internal,
                "Publisher has no asynchronous PusherSession implementation"));
        }
        return async_session_->RequestStop();
    }
    if (!session_) {
        return PusherResult::Success();
    }
    return session_->RequestStop();
}
