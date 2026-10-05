#include "media/publisher/publisher.h"
#include "media/ffmpeg_packet_buffer.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

extern "C" {
#include <libavcodec/packet.h>
}

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
    config.kind = PublisherKind::Client;
    config.targets.push_back({"primary", {}});
    config.targets[0].session.pusher = MakeValidPusherConfig();
    config.targets[0].session.reconnect_policy.enabled = false;
    return config;
}

MediaPacket MakeVideoPacket(bool keyframe) {
    MediaPacket packet;
    packet.type = MediaType::VIDEO;
    packet.codec = CodecType::H264;
    packet.keyframe = keyframe;
    return packet;
}

MediaPacket MakeEncodedVideoPacket(bool keyframe) {
    AVPacket* raw = av_packet_alloc();
    if (!raw || av_new_packet(raw, 4) < 0) {
        if (raw) {
            av_packet_free(&raw);
        }
        throw std::runtime_error("failed to allocate encoded test packet");
    }
    std::memset(raw->data, 0, static_cast<std::size_t>(raw->size));

    MediaPacket packet = MakeVideoPacket(keyframe);
    packet.pts = 0;
    packet.dts = 0;
    packet.duration = 1;
    packet.time_base = {1, 25};
    packet.buffer = std::make_shared<FFmpegPacketBuffer>(raw);
    packet.backend.type = BackendHandle::FFMPEG;
    packet.backend.ptr = raw;
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
        if (fail_next_open) {
            fail_next_open = false;
            return PusherResult::Failed(MakeError(PusherErrorCategory::OpenFailed, "scripted open failed"));
        }
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
        if (fail_next_close) {
            fail_next_close = false;
            return PusherResult::Failed(MakeError(PusherErrorCategory::CloseFailed, "scripted close failed"));
        }
        return PusherResult::Success();
    }

    bool IsOpen() const override { return opened; }

    PusherResult RequestStop() override { ++stop_calls; return PusherResult::Success(); }

    std::atomic<int> open_calls{0};
    std::atomic<int> push_calls{0};
    std::atomic<int> close_calls{0};
    std::atomic<int> stop_calls{0};
    bool opened{false};
    bool fail_next_push{false};
    bool fail_next_open{false};
    bool fail_next_close{false};
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

    // 同步 Publisher 只接受 Publish；异步模式由配置选择，并只接受 Enqueue。
    auto async_scripted_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* const async_scripted = async_scripted_pusher.get();
    auto async_session = std::make_unique<AsyncPusherSession>(
        std::make_unique<PusherSession>(std::move(async_scripted_pusher)));
    Publisher async_publisher(std::move(async_session));
    PublisherConfig async_config = MakeValidPublisherConfig();
    async_config.targets[0].session.mode = PusherSessionMode::Asynchronous;
    async_config.targets[0].session.async.queue_capacity = 2;

    if (!async_publisher.Open(async_config).Succeed() ||
        async_publisher.State() != PublisherState::WaitingForKeyframe ||
        !IsPublishFailure(async_publisher.Publish(MakeVideoPacket(true)),
                          PusherErrorCategory::InvalidState)) {
        std::cerr << "Publisher did not enforce asynchronous mode" << std::endl;
        return 1;
    }

    if (!async_publisher.Enqueue(MakeEncodedVideoPacket(true)).Accepted()) {
        std::cerr << "Publisher did not enqueue an asynchronous packet" << std::endl;
        return 1;
    }
    for (int i = 0; i < 100 && async_scripted->push_calls == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (async_scripted->push_calls != 1 ||
        async_publisher.State() != PublisherState::Running ||
        !async_publisher.Close().Succeed()) {
        std::cerr << "Publisher did not complete asynchronous output" << std::endl;
        return 1;
    }

    PublisherConfig invalid = MakeValidPublisherConfig();
    if (invalid.targets.empty() || !invalid.is_valid()) return 1;
    invalid.targets.push_back(invalid.targets.front());
    if (invalid.is_valid()) {
        std::cerr << "Publisher accepted duplicate target IDs" << std::endl;
        return 1;
    }
    invalid.targets[1].target_id = "secondary";
    invalid.targets[1].session.mode = PusherSessionMode::Asynchronous;
    if (invalid.is_valid()) {
        std::cerr << "Publisher accepted mixed session modes" << std::endl;
        return 1;
    }

    PublisherConfig multi_config = MakeValidPublisherConfig();
    multi_config.targets.push_back(multi_config.targets.front());
    multi_config.targets[1].target_id = "secondary";
    multi_config.targets[1].session.pusher.output_url = "secondary-output.mp4";

    auto first_pusher = std::make_unique<ScriptedPusher>();
    auto second_pusher = std::make_unique<ScriptedPusher>();
    auto* first = first_pusher.get();
    auto* second = second_pusher.get();
    std::vector<std::unique_ptr<PusherSession>> sync_sessions;
    sync_sessions.push_back(std::make_unique<PusherSession>(std::move(first_pusher)));
    sync_sessions.push_back(std::make_unique<PusherSession>(std::move(second_pusher)));
    Publisher multi(std::move(sync_sessions));
    if (!multi.Open(multi_config).Succeed() ||
        !multi.Publish(MakeVideoPacket(true)).WasPublished() ||
        first->push_calls != 1 || second->push_calls != 1 ||
        multi.TargetStates().size() != 2 || multi.State() != PublisherState::Running) {
        std::cerr << "Publisher did not broadcast to both synchronous sessions" << std::endl;
        return 1;
    }
    first->fail_next_push = true;
    const auto partial = multi.Publish(MakeVideoPacket(false));
    if (!IsPublishFailure(partial, PusherErrorCategory::WriteFailed) ||
        partial.error->message.find("primary") == std::string::npos ||
        second->push_calls != 2 || multi.State() != PublisherState::Failed ||
        multi.TargetStates()[1].state != PusherSessionState::Running ||
        multi.LastPublishResults().size() != 2 ||
        !multi.LastPublishResults()[1].result.WasPublished()) {
        std::cerr << "Publisher did not isolate a synchronous target failure" << std::endl;
        return 1;
    }
    if (!multi.RequestStop().Succeed() || first->stop_calls == 0 || second->stop_calls == 0 ||
        !multi.Close().Succeed()) {
        std::cerr << "Publisher did not stop and close all targets" << std::endl;
        return 1;
    }

    second->fail_next_open = true;
    const auto failed_open = multi.Open(multi_config);
    if (failed_open.Succeed() || failed_open.error->message.find("secondary") == std::string::npos ||
        first->open_calls != 2 || first->close_calls != 2 ||
        multi.State() != PublisherState::Closed) {
        std::cerr << "Publisher did not roll back a partial Open" << std::endl;
        return 1;
    }
    if (!multi.Open(multi_config).Succeed()) return 1;
    first->fail_next_close = true;
    const auto failed_close = multi.Close();
    if (failed_close.Succeed() || failed_close.error->message.find("primary") == std::string::npos ||
        second->close_calls != 3 || multi.State() != PublisherState::Failed) {
        std::cerr << "Publisher did not close remaining targets after a close failure" << std::endl;
        return 1;
    }

    auto async_first_pusher = std::make_unique<ScriptedPusher>();
    auto async_second_pusher = std::make_unique<ScriptedPusher>();
    auto* async_first = async_first_pusher.get();
    auto* async_second = async_second_pusher.get();
    std::vector<std::unique_ptr<AsyncPusherSession>> async_sessions;
    async_sessions.push_back(std::make_unique<AsyncPusherSession>(
        std::make_unique<PusherSession>(std::move(async_first_pusher))));
    async_sessions.push_back(std::make_unique<AsyncPusherSession>(
        std::make_unique<PusherSession>(std::move(async_second_pusher))));
    Publisher multi_async(std::move(async_sessions));
    for (auto& target : multi_config.targets) {
        target.session.mode = PusherSessionMode::Asynchronous;
        target.session.async.queue_capacity = 2;
    }
    if (!multi_async.Open(multi_config).Succeed() ||
        !multi_async.Enqueue(MakeEncodedVideoPacket(true)).Accepted() ||
        multi_async.LastEnqueueResults().size() != 2) {
        std::cerr << "Publisher did not enqueue to both asynchronous sessions" << std::endl;
        return 1;
    }
    for (int i = 0; i < 200 && (async_first->push_calls == 0 || async_second->push_calls == 0); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (async_first->push_calls != 1 || async_second->push_calls != 1 ||
        multi_async.State() != PublisherState::Running || !multi_async.Close().Succeed()) {
        std::cerr << "Publisher did not write both asynchronous targets" << std::endl;
        return 1;
    }

    std::cout << "Publisher test passed" << std::endl;
    return 0;
}
