#include "media/publisher/publisher.h"

#include <memory>
#include <utility>

Publisher::Publisher() : Publisher(std::make_unique<PusherSession>()) {

}

Publisher::Publisher(std::unique_ptr<PusherSession> session)
    : session_(std::move(session)) {}

Publisher::~Publisher() {
    Close();
}

PusherError Publisher::MakeError(PusherErrorCategory category, const char* message) {
    return PusherError{category, message, false};
}

PusherResult Publisher::Open(const PublisherConfig& config) {
    if (!config.is_valid()) {
        return PusherResult::Failed(MakeError(
            PusherErrorCategory::InvalidConfiguration,
            "Publisher received an invalid output configuration"));
    }

    if (!session_) {
        return PusherResult::Failed(MakeError(
            PusherErrorCategory::Internal,
            "Publisher has no PusherSession implementation"));
    }

    // 当前注册表只包含 FFmpegFile。把类型检查保留在门面层，后续增加
    // RTSP Server 或其它 PublisherKind 时，可以在此创建不同的 Session，
    // 而不让上层调用方接触具体 Pusher 类型。
    if (config.kind != PublisherKind::FFmpegFile) {
        return PusherResult::Failed(MakeError(PusherErrorCategory::InvalidConfiguration,
            "Publisher does not support the requested output kind"));
    }

    return session_->Open(config.session);
}

PusherPublishResult Publisher::Publish(const MediaPacket& packet) {
    if (!session_) {
        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::Internal,
            "Publisher has no PusherSession implementation"));
    }

    // 关键帧等待、包校验和底层写入错误都由 Session 返回。Publisher 不应
    // 复制这套策略，否则不同输出目标的行为会开始分叉。
    return session_->Publish(packet);
}

PusherResult Publisher::Close() {
    if (!session_) {
        return PusherResult::Success();
    }
    return session_->Close();
}

PublisherState Publisher::State() const noexcept {
    if (!session_) {
        return PublisherState::Failed;
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
