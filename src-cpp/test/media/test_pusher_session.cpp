#include "media/pusher/pusher_session.h"

#include <iostream>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
    packet.time_base = {1, 25};
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
        if (fail_next_open_count > 0) {
            --fail_next_open_count;
            opened = false;
            return PusherResult::Failed(PusherError{
                PusherErrorCategory::OpenFailed, "scripted open failed", true});
        }
        if (next_open_error.has_value()) {
            opened = false;
            auto error = std::move(*next_open_error);
            next_open_error.reset();
            return PusherResult::Failed(std::move(error));
        }
        opened = true;
        return PusherResult::Success();
    }

    PusherResult Push(const MediaPacket& packet) override {
        ++push_calls;
        last_packet_was_keyframe = packet.keyframe;
        last_packet = packet;
        if (block_next_push) {
            std::unique_lock<std::mutex> lock(block_mutex);
            push_entered = true;
            block_condition.notify_all();
            block_condition.wait(lock, [this] { return stop_calls.load() > 0; });
            block_next_push = false;
            return PusherResult::Failed(MakeError(PusherErrorCategory::Cancelled,
                "scripted push interrupted"));
        }
        if (!opened) {
            return PusherResult::Failed(MakeError(
                PusherErrorCategory::InvalidState,
                "scripted pusher is not open"));
        }
        if (fail_next_push) {
            fail_next_push = false;
            return PusherResult::Failed(PusherError{
                PusherErrorCategory::WriteFailed, "scripted write failed", true});
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

    PusherResult RequestStop() override {
        stop_calls.fetch_add(1);
        block_condition.notify_all();
        return PusherResult::Success();
    }

    void SetEventCallback(EventCallback cb) override { event_callback = std::move(cb); }

    void EmitEvent(const PusherEvent& event) { event_callback(event); }

    std::atomic<int> open_calls{0};
    int push_calls{0};
    std::atomic<int> close_calls{0};
    bool opened{false};
    bool fail_next_push{false};
    int fail_next_open_count{0};
    bool last_packet_was_keyframe{false};
    std::optional<MediaPacket> last_packet;
    std::optional<PusherError> next_error;
    std::optional<PusherError> next_open_error;
    std::atomic<int> stop_calls{0};
    std::mutex block_mutex;
    std::condition_variable block_condition;
    bool block_next_push{false};
    bool push_entered{false};
    EventCallback event_callback;
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
    config.reconnect_policy.enabled = false;
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
    MediaPacket first_keyframe_packet = MakeVideoPacket(true);
    first_keyframe_packet.pts = 120;
    first_keyframe_packet.dts = 118;
    first_keyframe_packet.duration = 2;
    const PusherPublishResult first_keyframe = session.Publish(first_keyframe_packet);
    if (!first_keyframe.Succeed() || !first_keyframe.WasPublished() ||
        session.State() != PusherSessionState::Running ||
        scripted->push_calls != 1 || !scripted->last_packet_was_keyframe ||
        !scripted->last_packet.has_value() ||
        scripted->last_packet->pts != first_keyframe_packet.pts ||
        scripted->last_packet->dts != first_keyframe_packet.dts) {
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

    // StartAtZero 由 Session 处理而不是 Muxer 处理。首个被接纳的关键帧
    // 使用最早的 DTS/PTS 建立 epoch；上游 packet 本身不能被改写。
    PusherSessionConfig zero_based_config = config;
    zero_based_config.timestamp_policy.mode = PusherTimestampMode::StartAtZero;
    if (!session.Open(zero_based_config).Succeed()) {
        std::cerr << "Failed to open zero-based session" << std::endl;
        return 1;
    }
    MediaPacket zero_based_first = MakeVideoPacket(true);
    zero_based_first.pts = 120;
    zero_based_first.dts = 118;
    zero_based_first.duration = 2;
    if (!session.Publish(zero_based_first).WasPublished() ||
        !scripted->last_packet.has_value() || scripted->last_packet->pts != 2 ||
        scripted->last_packet->dts != 0 || scripted->last_packet->duration != 2 ||
        zero_based_first.pts != 120 || zero_based_first.dts != 118) {
        std::cerr << "Session did not normalize the first packet without mutating input" << std::endl;
        return 1;
    }
    MediaPacket zero_based_next = MakeVideoPacket(false);
    zero_based_next.pts = 124;
    zero_based_next.dts = 122;
    zero_based_next.duration = 2;
    if (!session.Publish(zero_based_next).WasPublished() ||
        !scripted->last_packet.has_value() || scripted->last_packet->pts != 6 ||
        scripted->last_packet->dts != 4 || scripted->last_packet->duration != 2) {
        std::cerr << "Session did not preserve the zero-based timeline" << std::endl;
        return 1;
    }
    if (!session.Close().Succeed()) return 1;

    // 首个接纳包缺少 PTS/DTS 时不能猜测 epoch；失败不应将包交给 Pusher，
    // 后续具有时间戳的关键帧仍可建立新的 epoch。
    if (!session.Open(zero_based_config).Succeed()) return 1;
    const int pushes_before_missing_timestamp = scripted->push_calls;
    if (!IsFailedWith(session.Publish(MakeVideoPacket(true)),
                      PusherErrorCategory::InvalidPacket) ||
        scripted->push_calls != pushes_before_missing_timestamp ||
        session.State() != PusherSessionState::WaitingForKeyframe) {
        std::cerr << "Missing first timestamp was not rejected by StartAtZero" << std::endl;
        return 1;
    }
    MediaPacket valid_after_missing_timestamp = MakeVideoPacket(true);
    valid_after_missing_timestamp.pts = 502;
    valid_after_missing_timestamp.dts = 500;
    if (!session.Publish(valid_after_missing_timestamp).WasPublished() ||
        !scripted->last_packet.has_value() || scripted->last_packet->pts != 2 ||
        scripted->last_packet->dts != 0) {
        std::cerr << "Session did not establish epoch after a rejected packet" << std::endl;
        return 1;
    }
    if (!session.Close().Succeed()) return 1;

    // 若首个有效时间戳包被底层拒绝，epoch 不能被提交；下一次成功写入的
    // 关键帧必须以自身最早的 DTS/PTS 作为 0 点。
    if (!session.Open(zero_based_config).Succeed()) return 1;
    scripted->next_error = MakeError(PusherErrorCategory::InvalidPacket,
                                     "scripted packet rejection");
    MediaPacket rejected_first_packet = MakeVideoPacket(true);
    rejected_first_packet.pts = 302;
    rejected_first_packet.dts = 300;
    if (!IsFailedWith(session.Publish(rejected_first_packet),
                      PusherErrorCategory::InvalidPacket) ||
        session.State() != PusherSessionState::WaitingForKeyframe) {
        std::cerr << "Rejected first packet changed the session state" << std::endl;
        return 1;
    }
    MediaPacket accepted_after_rejection = MakeVideoPacket(true);
    accepted_after_rejection.pts = 402;
    accepted_after_rejection.dts = 400;
    if (!session.Publish(accepted_after_rejection).WasPublished() ||
        !scripted->last_packet.has_value() || scripted->last_packet->pts != 2 ||
        scripted->last_packet->dts != 0) {
        std::cerr << "Rejected first packet incorrectly established the epoch" << std::endl;
        return 1;
    }
    if (!session.Close().Succeed()) return 1;

    // StartAtZero 的 epoch 是当前轨道 time base 下的整数 tick。不能把不同
    // time base 的值静默相减，否则会产生错误的媒体时间轴。
    if (!session.Open(zero_based_config).Succeed()) return 1;
    MediaPacket mismatched_time_base = MakeVideoPacket(true);
    mismatched_time_base.pts = 100;
    mismatched_time_base.dts = 100;
    mismatched_time_base.time_base = {1, 50};
    if (!IsFailedWith(session.Publish(mismatched_time_base),
                      PusherErrorCategory::InvalidPacket) ||
        session.State() != PusherSessionState::WaitingForKeyframe) {
        std::cerr << "StartAtZero accepted a packet with a mismatched time base" << std::endl;
        return 1;
    }
    if (!session.Close().Succeed()) return 1;

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

    if (!IsRetryablePusherError(PusherErrorCategory::OpenFailed) ||
        !IsRetryablePusherError(PusherErrorCategory::WriteFailed) ||
        !IsRetryablePusherError(PusherErrorCategory::Timeout) ||
        !IsRetryablePusherError(PusherErrorCategory::Network) ||
        IsRetryablePusherError(PusherErrorCategory::InvalidPacket) ||
        IsRetryablePusherError(PusherErrorCategory::CloseFailed)) {
        std::cerr << "Pusher error retryability classification is incorrect" << std::endl;
        return 1;
    }

    // 可重试写失败会关闭旧连接并重新 Open；失败包不重发，重连后等待关键帧。
    auto reconnect_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* const reconnecting = reconnect_pusher.get();
    PusherSession reconnecting_session(std::move(reconnect_pusher));
    PusherSessionConfig reconnect_config = config;
    reconnect_config.reconnect_policy.enabled = true;
    reconnect_config.reconnect_policy.initial_delay = std::chrono::milliseconds(0);
    reconnect_config.reconnect_policy.max_attempts = 1;
    if (!reconnecting_session.Open(reconnect_config).Succeed()) {
        std::cerr << "Failed to open reconnecting session" << std::endl;
        return 1;
    }
    reconnecting->fail_next_push = true;
    if (!IsFailedWith(reconnecting_session.Publish(MakeVideoPacket(true)),
                      PusherErrorCategory::WriteFailed) ||
        reconnecting_session.State() != PusherSessionState::WaitingForKeyframe ||
        reconnecting->open_calls != 2 || reconnecting->push_calls != 1) {
        std::cerr << "PusherSession did not recover after a retryable write failure"
                  << std::endl;
        return 1;
    }
    if (!reconnecting_session.Publish(MakeVideoPacket(false)).Succeed() ||
        reconnecting->push_calls != 1) {
        std::cerr << "PusherSession wrote a non-keyframe during reconnect" << std::endl;
        return 1;
    }
    if (!reconnecting_session.Publish(MakeVideoPacket(true)).WasPublished() ||
        reconnecting_session.State() != PusherSessionState::Running ||
        reconnecting->push_calls != 2 ||
        !reconnecting_session.Close().Succeed()) {
        std::cerr << "PusherSession did not resume on the next keyframe" << std::endl;
        return 1;
    }

    auto waiting_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* const waiting = waiting_pusher.get();
    PusherSession waiting_session(std::move(waiting_pusher));
    PusherSessionConfig waiting_config = reconnect_config;
    waiting_config.reconnect_policy.initial_delay = std::chrono::milliseconds(750);
    if (!waiting_session.Open(waiting_config).Succeed()) return 1;
    waiting->fail_next_push = true;
    PusherPublishResult waiting_result;
    std::thread waiting_writer([&] {
        waiting_result = waiting_session.Publish(MakeVideoPacket(true));
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (waiting->close_calls.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto stopped_at = std::chrono::steady_clock::now();
    waiting_session.RequestStop();
    waiting_writer.join();
    if (waiting->close_calls.load() != 1 || waiting->open_calls.load() != 1 ||
        !IsFailedWith(waiting_result, PusherErrorCategory::Cancelled) ||
        std::chrono::steady_clock::now() - stopped_at > std::chrono::milliseconds(200) ||
        !waiting_session.Close().Succeed()) {
        std::cerr << "Stop did not interrupt reconnect backoff" << std::endl;
        return 1;
    }

    auto retry_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* const retrying = retry_pusher.get();
    PusherSession retry_session(std::move(retry_pusher));
    PusherSessionConfig retry_config = reconnect_config;
    retry_config.reconnect_policy.max_attempts = 2;
    retry_config.reconnect_policy.initial_delay = std::chrono::milliseconds(10);
    retry_config.reconnect_policy.multiplier = 2.0;
    if (!retry_session.Open(retry_config).Succeed()) return 1;
    retrying->fail_next_push = true;
    retrying->fail_next_open_count = 1;
    const auto retry_started = std::chrono::steady_clock::now();
    const auto retry_result = retry_session.Publish(MakeVideoPacket(true));
    if (!IsFailedWith(retry_result, PusherErrorCategory::WriteFailed) ||
        retry_session.State() != PusherSessionState::WaitingForKeyframe ||
        retrying->open_calls.load() != 3 || retrying->push_calls != 1 ||
        std::chrono::steady_clock::now() - retry_started < std::chrono::milliseconds(25) ||
        !retry_session.Publish(MakeVideoPacket(true)).WasPublished() ||
        !retry_session.Close().Succeed()) {
        std::cerr << "PusherSession did not retry Open with backoff" << std::endl;
        return 1;
    }

    auto rejected_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* const rejected = rejected_pusher.get();
    PusherSession rejected_session(std::move(rejected_pusher));
    if (!rejected_session.Open(reconnect_config).Succeed()) return 1;
    rejected->fail_next_push = true;
    rejected->next_open_error = PusherError{
        PusherErrorCategory::Authentication, "scripted authentication failed", false};
    if (!IsFailedWith(rejected_session.Publish(MakeVideoPacket(true)),
                      PusherErrorCategory::Authentication) ||
        rejected_session.State() != PusherSessionState::Failed ||
        rejected->open_calls.load() != 2 ||
        !rejected_session.Close().Succeed()) {
        std::cerr << "PusherSession lost the terminal reconnect error" << std::endl;
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

    auto event_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* event_source = event_pusher.get();
    PusherSession event_session(std::move(event_pusher));
    if (!event_session.Open(config).Succeed()) return 1;
    const PusherEvent network_event{
        PusherEventStage::Write,
        PusherError{PusherErrorCategory::Network, "scripted async failure", true},
    };
    std::thread event_thread([&] { event_source->EmitEvent(network_event); });
    event_thread.join();
    {
        std::unique_lock<std::mutex> lock(event_source->block_mutex);
        if (!event_source->block_condition.wait_for(lock, std::chrono::seconds(2),
                [&] { return event_source->stop_calls.load() > 0; })) {
            std::cerr << "Event thread did not react without a Session call" << std::endl;
            return 1;
        }
    }
    const auto event_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (event_session.State() != PusherSessionState::Failed &&
           std::chrono::steady_clock::now() < event_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto observed_event = event_session.LastEvent();
    if (event_session.State() != PusherSessionState::Failed ||
        !observed_event.has_value() ||
        observed_event->stage != PusherEventStage::Write ||
        observed_event->error.category != PusherErrorCategory::Network ||
        !observed_event->error.retryable ||
        !IsFailedWith(event_session.Publish(MakeVideoPacket(true)),
                      PusherErrorCategory::Network) ||
        event_source->push_calls != 0) {
        std::cerr << "Structured async event was not handled by Session" << std::endl;
        return 1;
    }
    if (!event_session.Close().Succeed()) return 1;

    auto async_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* const async_source = async_pusher.get();
    PusherSession async_session(std::move(async_pusher));
    if (!async_session.Open(reconnect_config).Succeed() ||
        !async_session.Publish(MakeVideoPacket(true)).WasPublished()) return 1;
    async_source->EmitEvent(network_event);
    const auto async_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (async_source->open_calls.load() < 2 &&
           std::chrono::steady_clock::now() < async_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (async_source->open_calls.load() != 2 ||
        async_session.State() != PusherSessionState::WaitingForKeyframe ||
        async_session.Publish(MakeVideoPacket(false)).status !=
            PusherPublishStatus::DroppedAwaitingKeyframe ||
        !async_session.Publish(MakeVideoPacket(true)).WasPublished() ||
        async_session.State() != PusherSessionState::Running ||
        !async_session.Close().Succeed()) {
        std::cerr << "Async event did not recover on the next keyframe" << std::endl;
        return 1;
    }

    const int stops_after_close = event_source->stop_calls.load();
    event_source->EmitEvent(network_event);
    if (!event_session.Open(config).Succeed() ||
        event_session.State() != PusherSessionState::WaitingForKeyframe ||
        event_session.LastEvent().has_value() ||
        event_source->stop_calls.load() != stops_after_close) {
        std::cerr << "Closed connection event leaked into reopened Session" << std::endl;
        return 1;
    }
    if (!event_session.Close().Succeed()) return 1;

    auto interrupted_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* interrupted = interrupted_pusher.get();
    PusherSession interrupted_session(std::move(interrupted_pusher));
    if (!interrupted_session.Open(config).Succeed()) return 1;
    interrupted->block_next_push = true;
    PusherPublishResult interrupted_result;
    std::thread interrupted_writer([&] {
        interrupted_result = interrupted_session.Publish(MakeVideoPacket(true));
    });
    {
        std::unique_lock<std::mutex> lock(interrupted->block_mutex);
        if (!interrupted->block_condition.wait_for(lock, std::chrono::seconds(2),
                [&] { return interrupted->push_entered; })) {
            interrupted_session.RequestStop();
            interrupted_writer.join();
            std::cerr << "Event interruption test did not reach Push" << std::endl;
            return 1;
        }
    }
    interrupted->EmitEvent(network_event);
    {
        std::unique_lock<std::mutex> lock(interrupted->block_mutex);
        if (!interrupted->block_condition.wait_for(lock, std::chrono::seconds(2),
                [&] { return interrupted->stop_calls.load() > 0; })) {
            interrupted_session.RequestStop();
            interrupted_writer.join();
            std::cerr << "Event thread did not interrupt blocked Push" << std::endl;
            return 1;
        }
    }
    interrupted_writer.join();
    const auto interrupted_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (interrupted_session.State() != PusherSessionState::Failed &&
           std::chrono::steady_clock::now() < interrupted_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (interrupted->stop_calls.load() != 1 ||
        !IsFailedWith(interrupted_result, PusherErrorCategory::Cancelled) ||
        interrupted_session.State() != PusherSessionState::Failed ||
        !interrupted_session.LastEvent().has_value()) {
        std::cerr << "Async event did not interrupt blocked Push" << std::endl;
        return 1;
    }
    if (!interrupted_session.Close().Succeed()) return 1;

    auto blocking_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* blocking = blocking_pusher.get();
    PusherSession blocking_session(std::move(blocking_pusher));
    if (!blocking_session.Open(config).Succeed()) return 1;
    blocking->block_next_push = true;
    PusherPublishResult blocked_result;
    std::thread writer([&] { blocked_result = blocking_session.Publish(MakeVideoPacket(true)); });
    bool push_entered = false;
    {
        std::unique_lock<std::mutex> lock(blocking->block_mutex);
        push_entered = blocking->block_condition.wait_for(lock, std::chrono::seconds(2),
            [&] { return blocking->push_entered; });
    }
    if (!push_entered) {
        blocking_session.RequestStop();
        writer.join();
        std::cerr << "Scripted Push did not enter the blocking call" << std::endl;
        return 1;
    }
    const auto stop_started = std::chrono::steady_clock::now();
    const bool stop_ok = blocking_session.RequestStop().Succeed() &&
                         blocking_session.RequestStop().Succeed();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;
    const auto close_result = blocking_session.Close();
    writer.join();
    if (!stop_ok || stop_elapsed > std::chrono::milliseconds(200) ||
        !close_result.Succeed() || blocking->stop_calls.load() != 2 ||
        !IsFailedWith(blocked_result, PusherErrorCategory::Cancelled) ||
        blocking_session.State() != PusherSessionState::Closed) {
        std::cerr << "RequestStop/Close concurrency contract failed" << std::endl;
        return 1;
    }

    std::cout << "PusherSession test passed" << std::endl;
    return 0;
}
