#include "media/publisher/publisher.h"

#include <iostream>
#include <memory>
#include <utility>

namespace {

PusherError MakeError(PusherErrorCategory category, const char* message) {
    return PusherError{category, message, false};
}

PusherConfig MakeValidPusherConfig() {
    PusherConfig config;
    config.output_url = "scripted-publisher-output.mp4";
    config.video_track.media_type = MediaType::VIDEO;
    config.video_track.codec_type = CodecType::H264;
    config.video_track.time_base_num = 1;
    config.video_track.time_base_den = 25;
    config.video_track.video().width = 1280;
    config.video_track.video().height = 720;
    config.video_track.video().fps = 25.0f;
    return config;
}

PublisherConfig MakeValidPublisherConfig() {
    PublisherConfig config;
    config.kind = PublisherKind::FFmpegFile;
    config.session.pusher = MakeValidPusherConfig();
    return config;
}

MediaPacket MakeVideoPacket(bool keyframe) {
    MediaPacket packet;
    packet.type = MediaType::VIDEO;
    packet.codec = CodecType::H264;
    packet.keyframe = keyframe;
    return packet;
}

/// @brief 记录门面是否正确转发调用的测试替身。
///
/// 这里不创建真实 AVPacket 或文件，目的是只验证 Publisher 的职责：配置
/// 校验、状态映射，以及将发布策略委托给内部 PusherSession。
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
};

bool IsPublishFailure(const PusherPublishResult& result,
                      PusherErrorCategory category) {
    return !result.Succeed() && result.error.has_value() &&
           result.error->category == category;
}

}  // namespace

int main() {
    auto scripted_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* const scripted = scripted_pusher.get();
    auto session = std::make_unique<PusherSession>(std::move(scripted_pusher));
    Publisher publisher(std::move(session));

    // 门面在 Open 前不允许发布，且不应让包越过 Session 进入 Pusher。
    if (!IsPublishFailure(publisher.Publish(MakeVideoPacket(true)),
                          PusherErrorCategory::InvalidState) ||
        scripted->push_calls != 0 ||
        publisher.State() != PublisherState::Closed) {
        std::cerr << "Publisher accepted a packet before Open" << std::endl;
        return 1;
    }

    // 未注册的 PublisherKind 必须在门面层被拒绝，不能把不明确的输出类型
    // 误交给默认 FFmpegSession。
    PublisherConfig unsupported = MakeValidPublisherConfig();
    unsupported.kind = static_cast<PublisherKind>(999);
    if (publisher.Open(unsupported).Succeed() || scripted->open_calls != 0) {
        std::cerr << "Unsupported PublisherKind was accepted" << std::endl;
        return 1;
    }

    if (!publisher.Open(MakeValidPublisherConfig()).Succeed() ||
        publisher.State() != PublisherState::WaitingForKeyframe ||
        scripted->open_calls != 1) {
        std::cerr << "Publisher did not open its session" << std::endl;
        return 1;
    }

    // 门面不自行处理关键帧策略；它应原样返回 Session 的策略性丢弃结果。
    const PusherPublishResult dropped = publisher.Publish(MakeVideoPacket(false));
    if (!dropped.Succeed() || dropped.WasPublished() ||
        dropped.status != PusherPublishStatus::DroppedAwaitingKeyframe ||
        publisher.State() != PublisherState::WaitingForKeyframe ||
        scripted->push_calls != 0) {
        std::cerr << "Publisher did not preserve keyframe waiting behavior" << std::endl;
        return 1;
    }

    const PusherPublishResult keyframe = publisher.Publish(MakeVideoPacket(true));
    if (!keyframe.Succeed() || !keyframe.WasPublished() ||
        !scripted->last_packet_was_keyframe || scripted->push_calls != 1 ||
        publisher.State() != PublisherState::Running) {
        std::cerr << "Publisher did not forward the first keyframe" << std::endl;
        return 1;
    }

    // 底层写失败应透传给调用方，并由 Session 的状态映射表现为 Failed。
    scripted->fail_next_push = true;
    if (!IsPublishFailure(publisher.Publish(MakeVideoPacket(false)),
                          PusherErrorCategory::WriteFailed) ||
        publisher.State() != PublisherState::Failed) {
        std::cerr << "Publisher did not expose a session write failure" << std::endl;
        return 1;
    }

    if (!publisher.Close().Succeed() || !publisher.Close().Succeed() ||
        publisher.State() != PublisherState::Closed ||
        scripted->close_calls != 1) {
        std::cerr << "Publisher Close is not idempotent" << std::endl;
        return 1;
    }

    std::cout << "Publisher test passed" << std::endl;
    return 0;
}
