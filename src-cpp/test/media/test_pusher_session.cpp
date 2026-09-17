#include "media/pusher/pusher_session.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

PusherError MakeError(PusherErrorCategory category, const char* message) {
    return PusherError{category, message, false};
}

PusherConfig MakeValidConfig() {
    PusherConfig config;
    config.output_url = "scripted-session-output.mp4";
    config.video_track.media_type = MediaType::VIDEO;
    config.video_track.codec_type = CodecType::H264;
    config.video_track.time_base_num = 1;
    config.video_track.time_base_den = 25;
    config.video_track.video().width = 1280;
    config.video_track.video().height = 720;
    config.video_track.video().fps = 25.0f;
    return config;
}

MediaPacket MakeVideoPacket(bool keyframe) {
    MediaPacket packet;
    packet.type = MediaType::VIDEO;
    packet.codec = CodecType::H264;
    packet.keyframe = keyframe;
    return packet;
}

/// @brief 不依赖 FFmpeg 的脚本 Pusher，用来验证 Session 的纯策略行为。
///
/// 它只记录是否被调用，并可指定下一次 Push 返回 WriteFailed。因此测试不受
/// 编码器、容器格式、RTSP 网络和 AVPacket 所有权的影响。
class ScriptedPusher final : public IPusher {
public:
    PusherResult Open(const PusherConfig& config) override {
        ++open_calls;
        if (!config.is_valid()) {
            return PusherResult::Failed(MakeError(
                PusherErrorCategory::InvalidConfiguration,
                "scripted pusher received invalid config"));
        }
        opened = true;
        return PusherResult::Success();
    }

    PusherResult Push(const MediaPacket& packet) override {
        ++push_calls;
        last_packet_was_keyframe = packet.keyframe;
        if (!opened) {
            return PusherResult::Failed(MakeError(
                PusherErrorCategory::InvalidState,
                "scripted pusher is not open"));
        }
        if (fail_next_push) {
            fail_next_push = false;
            return PusherResult::Failed(MakeError(
                PusherErrorCategory::WriteFailed,
                "scripted write failed"));
        }
        if (next_error) {
            auto error = std::move(*next_error);
            next_error.reset();
            return PusherResult::Failed(std::move(error));
        }
        return PusherResult::Success();
    }

    PusherResult Close() override {
        ++close_calls;
        opened = false;
        return PusherResult::Success();
    }

    bool IsOpen() const override { return opened; }

    int open_calls{0};
    int push_calls{0};
    int close_calls{0};
    bool opened{false};
    bool fail_next_push{false};
    bool last_packet_was_keyframe{false};
    std::optional<PusherError> next_error;
};

bool IsFailedWith(const PusherPublishResult& result,
                  PusherErrorCategory category) {
    return !result.Succeed() && result.error.has_value() &&
           result.error->category == category;
}

}  // namespace

int main() {
    auto scripted_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* const scripted = scripted_pusher.get();
    PusherSession session(std::move(scripted_pusher));

    // 会话未打开时，Publish 不能把包发送给底层 Pusher。
    if (!IsFailedWith(session.Publish(MakeVideoPacket(true)),
                      PusherErrorCategory::InvalidState) ||
        scripted->push_calls != 0) {
        std::cerr << "Publish before Open was not rejected" << std::endl;
        return 1;
    }

    PusherSessionConfig config;
    config.pusher = MakeValidConfig();
    if (!session.Open(config).Succeed() ||
        session.State() != PusherSessionState::WaitingForKeyframe ||
        scripted->open_calls != 1) {
        std::cerr << "Session did not enter WaitingForKeyframe" << std::endl;
        return 1;
    }

    // 非关键帧被正常丢弃：结果是成功的策略处理，但没有真实写入。
    const PusherPublishResult dropped = session.Publish(MakeVideoPacket(false));
    if (!dropped.Succeed() || dropped.WasPublished() ||
        dropped.status != PusherPublishStatus::DroppedAwaitingKeyframe ||
        session.State() != PusherSessionState::WaitingForKeyframe ||
        scripted->push_calls != 0) {
        std::cerr << "Non-keyframe was not dropped while waiting" << std::endl;
        return 1;
    }

    // 第一个关键帧必须真正交给 Pusher；只有成功后状态才进入 Running。
    const PusherPublishResult first_keyframe = session.Publish(MakeVideoPacket(true));
    if (!first_keyframe.Succeed() || !first_keyframe.WasPublished() ||
        session.State() != PusherSessionState::Running ||
        scripted->push_calls != 1 || !scripted->last_packet_was_keyframe) {
        std::cerr << "First keyframe did not start the session" << std::endl;
        return 1;
    }

    // Running 阶段不再按关键帧过滤，普通 P 帧应正常转发。
    const PusherPublishResult running_packet = session.Publish(MakeVideoPacket(false));
    if (!running_packet.Succeed() || !running_packet.WasPublished() ||
        scripted->push_calls != 2) {
        std::cerr << "Running session did not forward a non-keyframe" << std::endl;
        return 1;
    }

    if (!session.Close().Succeed() || !session.Close().Succeed() ||
        session.State() != PusherSessionState::Closed ||
        scripted->close_calls != 1) {
        std::cerr << "Session Close is not idempotent" << std::endl;
        return 1;
    }

    // 写入失败后 Session 进入 Failed，不能再继续把包交给已失效输出。
    auto failing_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* const failing = failing_pusher.get();
    PusherSession failing_session(std::move(failing_pusher));
    if (!failing_session.Open(config).Succeed()) {
        std::cerr << "Failed to open scripted failure session" << std::endl;
        return 1;
    }
    failing->fail_next_push = true;
    if (!IsFailedWith(failing_session.Publish(MakeVideoPacket(true)),
                      PusherErrorCategory::WriteFailed) ||
        failing_session.State() != PusherSessionState::Failed) {
        std::cerr << "Write failure did not move session to Failed" << std::endl;
        return 1;
    }
    if (!IsFailedWith(failing_session.Publish(MakeVideoPacket(true)),
                      PusherErrorCategory::InvalidState) ||
        failing->push_calls != 1) {
        std::cerr << "Failed session accepted another packet" << std::endl;
        return 1;
    }
    if (!failing_session.Close().Succeed() ||
        failing_session.State() != PusherSessionState::Closed) {
        std::cerr << "Failed session did not close cleanly" << std::endl;
        return 1;
    }

    // 错误分类测试
    std::vector<std::string> retryable_errors;
    std::vector<std::string> non_retryable_errors;

    for (const auto category : {PusherErrorCategory::Timeout, PusherErrorCategory::Network,
                               PusherErrorCategory::Cancelled, PusherErrorCategory::Internal,
                               PusherErrorCategory::InvalidPacket}) {
        if (!failing_session.Open(config).Succeed()) return 1;
        const bool retryable = category == PusherErrorCategory::Timeout ||
                               category == PusherErrorCategory::Network;
        failing->next_error = PusherError{category, "scripted error", retryable};
        const auto result = failing_session.Publish(MakeVideoPacket(true));
        const auto expected_state = category == PusherErrorCategory::InvalidPacket
            ? PusherSessionState::WaitingForKeyframe : PusherSessionState::Failed;
        
        // 打印错误结果
        std::string category_str;
        switch (category) {
            case PusherErrorCategory::Timeout: category_str = "Timeout"; break;
            case PusherErrorCategory::Network: category_str = "Network"; break;
            case PusherErrorCategory::Cancelled: category_str = "Cancelled"; break;
            case PusherErrorCategory::Internal: category_str = "Internal"; break;
            case PusherErrorCategory::InvalidPacket: category_str = "InvalidPacket"; break;
            default: category_str = "Unknown"; break;
        }
        
        std::string state_str;
        switch (failing_session.State()) {
            case PusherSessionState::WaitingForKeyframe: state_str = "WaitingForKeyframe"; break;
            case PusherSessionState::Failed: state_str = "Failed"; break;
            case PusherSessionState::Closed: state_str = "Closed"; break;
            case PusherSessionState::Running: state_str = "Running"; break;
            default: state_str = "Unknown"; break;
        }

        std::string error_detail = category_str + " -> " + state_str + " (retryable: " + (retryable ? "yes" : "no") + ")";
        
        if (!IsFailedWith(result, category) || result.error->retryable != retryable ||
            failing_session.State() != expected_state) {
            std::cerr << "FAILED: " << error_detail << " - Session lost error policy or state" << std::endl;
            return 1;
        }
        
        std::cout << "PASSED: " << error_detail << std::endl;
        
        if (retryable) {
            retryable_errors.push_back(error_detail);
        } else {
            non_retryable_errors.push_back(error_detail);
        }
        
        if (!failing_session.Close().Succeed()) return 1;
    }

    // 打印汇总
    std::cout << "\n=== Error Policy Summary ===" << std::endl;
    std::cout << "Retryable Errors (" << retryable_errors.size() << "):" << std::endl;
    for (const auto& err : retryable_errors) {
        std::cout << "  - " << err << std::endl;
    }
    std::cout << "Non-Retryable Errors (" << non_retryable_errors.size() << "):" << std::endl;
    for (const auto& err : non_retryable_errors) {
        std::cout << "  - " << err << std::endl;
    }

    std::cout << "PusherSession test passed" << std::endl;
    return 0;
}