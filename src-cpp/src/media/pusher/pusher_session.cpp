#include "media/pusher/pusher_session.h"

#include <memory>
#include <utility>

#include "media/pusher/ffmpeg_pusher.h"

PusherSession::PusherSession() : PusherSession(std::make_unique<FFmpegPusher>()) {    

}

PusherSession::PusherSession(std::unique_ptr<IPusher> pusher) : pusher_(std::move(pusher)) {

}

PusherSession::~PusherSession() {
    Close();
}

PusherError PusherSession::MakeError(PusherErrorCategory category, const char* message) {
    return PusherError{category, message, false};
}

PusherResult PusherSession::Open(const PusherSessionConfig& config) {
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

    const PusherResult open_result = pusher_->Open(config.pusher);
    if (!open_result.Succeed()) {
        state_ = PusherSessionState::Failed;
        return open_result;
    }

    // 输出容器已经准备好，但任意 P/B 帧都不能作为新视频的起点。先等待
    // IDR 等关键帧，确保 MP4 和后续网络输出可以独立解码。
    state_ = PusherSessionState::WaitingForKeyframe;
    return PusherResult::Success();
}

PusherPublishResult PusherSession::Publish(const MediaPacket& packet) {
    if (state_ == PusherSessionState::Closed) {
        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::InvalidState,
            "PusherSession::Publish called before a successful Open"));
    }

    if (state_ == PusherSessionState::Failed) {
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

    const PusherResult push_result = pusher_->Push(packet);
    if (!push_result.Succeed()) {
        // InvalidPacket 等调用方输入错误不会破坏已经打开的输出容器，等待
        // 下一包仍然合理；真正的底层 WriteFailed 才表示当前输出会话失效。
        if (push_result.error.has_value() && push_result.error->category == PusherErrorCategory::WriteFailed) {
            state_ = PusherSessionState::Failed;
        }

        if (push_result.error.has_value()) {
            return PusherPublishResult::Failed(*push_result.error);
        }

        return PusherPublishResult::Failed(MakeError(PusherErrorCategory::Internal,
            "Pusher returned a failed result without an error"));
    }

    if (state_ == PusherSessionState::WaitingForKeyframe) {
        // 只有关键帧已经被底层 Pusher 成功接收，才能认为输出流真正启动。
        // 如果写入失败，状态会保留 WaitingForKeyframe 或转为 Failed，而不
        // 会出现“尚未写入关键帧却已经 Running”的状态错乱。
        state_ = PusherSessionState::Running;
    }

    return PusherPublishResult::Published();
}

PusherResult PusherSession::Close() {
    if (!pusher_) {
        state_ = PusherSessionState::Closed;
        return PusherResult::Success();
    }

    // 当底层 Pusher 本来就没有打开时，不再重复调用 Close。这样析构、
    // 显式 Close 和失败清理可以安全叠加，又不会重复写输出容器 trailer。
    if (state_ == PusherSessionState::Closed && !pusher_->IsOpen()) {
        return PusherResult::Success();
    }

    const PusherResult close_result = pusher_->Close();
    state_ = close_result.Succeed() ? PusherSessionState::Closed : PusherSessionState::Failed;
    return close_result;
}
