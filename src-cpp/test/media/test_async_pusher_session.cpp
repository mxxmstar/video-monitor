#include "media/ffmpeg_packet_buffer.h"
#include "media/pusher/async_pusher_session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

extern "C" {
#include <libavcodec/packet.h>
}

namespace {

PusherError MakeError(PusherErrorCategory category, const char* message,
                      bool retryable = false) {
    return PusherError{category, message, retryable};
}

PusherConfig MakeValidConfig() {
    PusherConfig config;
    config.output_url = "async-scripted-output.mp4";
    config.video_track.media_type = MediaType::VIDEO;
    config.video_track.codec_type = CodecType::H264;
    config.video_track.time_base_num = 1;
    config.video_track.time_base_den = 25;
    config.video_track.video().width = 1280;
    config.video_track.video().height = 720;
    config.video_track.video().fps = 25.0f;
    return config;
}

MediaPacket MakeVideoPacket(bool keyframe, int value) {
    AVPacket* raw = av_packet_alloc();
    if (!raw || av_new_packet(raw, 4) < 0) {
        if (raw) av_packet_free(&raw);
        throw std::runtime_error("failed to allocate test packet");
    }
    std::memset(raw->data, value, static_cast<std::size_t>(raw->size));

    MediaPacket packet;
    packet.type = MediaType::VIDEO;
    packet.codec = CodecType::H264;
    packet.keyframe = keyframe;
    packet.pts = value;
    packet.dts = value;
    packet.duration = 1;
    packet.time_base = {1, 25};
    packet.buffer = std::make_shared<FFmpegPacketBuffer>(raw);
    packet.backend.type = BackendHandle::FFMPEG;
    packet.backend.ptr = raw;
    return packet;
}

class ScriptedPusher final : public IPusher {
public:
    PusherResult Open(const PusherConfig& config) override {
        if (!config.is_valid()) {
            return PusherResult::Failed(MakeError(
                PusherErrorCategory::InvalidConfiguration, "invalid config"));
        }
        opened = true;
        ++open_calls;
        return PusherResult::Success();
    }

    PusherResult Push(const MediaPacket& packet) override {
        ++push_calls;
        if (block_push) {
            std::unique_lock<std::mutex> lock(mutex);
            push_entered = true;
            condition.notify_all();
            condition.wait(lock, [this] { return stop_calls > 0; });
            block_push = false;
            return PusherResult::Failed(MakeError(
                PusherErrorCategory::Cancelled, "push interrupted"));
        }
        if (!opened) {
            return PusherResult::Failed(MakeError(
                PusherErrorCategory::InvalidState, "not open"));
        }
        last_packet_value = packet.buffer ? packet.buffer->Data()[0] : -1;
        return PusherResult::Success();
    }

    PusherResult Close() override {
        opened = false;
        ++close_calls;
        return PusherResult::Success();
    }

    bool IsOpen() const override { return opened; }

    PusherResult RequestStop() override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++stop_calls;
        }
        condition.notify_all();
        return PusherResult::Success();
    }

    bool opened{false};
    bool block_push{false};
    bool push_entered{false};
    int last_packet_value{-1};
    std::atomic<int> open_calls{0};
    std::atomic<int> push_calls{0};
    std::atomic<int> close_calls{0};
    std::atomic<int> stop_calls{0};
    std::mutex mutex;
    std::condition_variable condition;
};

bool WaitUntil(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

}  // namespace

int main() {
    auto scripted_pusher = std::make_unique<ScriptedPusher>();
    ScriptedPusher* const scripted = scripted_pusher.get();
    AsyncPusherSession session(std::make_unique<PusherSession>(
        std::move(scripted_pusher)));

    PusherSessionConfig config;
    config.mode = PusherSessionMode::Asynchronous;
    config.async.queue_capacity = 1;
    config.async.close_mode = PusherAsyncCloseMode::Discard;
    config.pusher = MakeValidConfig();
    config.reconnect_policy.enabled = false;

    if (!session.Open(config).Succeed()) {
        std::cerr << "Async session did not open" << std::endl;
        return 1;
    }

    MediaPacket non_keyframe = MakeVideoPacket(false, 1);
    const PusherEnqueueResult dropped = session.Enqueue(non_keyframe);
    if (dropped.status != PusherEnqueueStatus::DroppedAwaitingKeyframe ||
        session.Stats().dropped_awaiting_keyframe != 1) {
        std::cerr << "Async session did not gate non-keyframes" << std::endl;
        return 1;
    }

    MediaPacket first_keyframe = MakeVideoPacket(true, 7);
    const PusherEnqueueResult accepted = session.Enqueue(first_keyframe);
    if (!accepted.Accepted() ||
        !WaitUntil([&] { return session.Stats().written == 1; }) ||
        session.State() != PusherSessionState::Running ||
        scripted->last_packet_value != 7) {
        std::cerr << "Async session did not write the accepted keyframe" << std::endl;
        return 1;
    }

    // The queued packet owns an AVPacket reference. Releasing the source packet
    // immediately after Enqueue must not invalidate the packet seen by Pusher.
    MediaPacket owned_source = MakeVideoPacket(false, 9);
    if (!session.Enqueue(owned_source).Accepted()) {
        std::cerr << "Async session rejected a running packet" << std::endl;
        return 1;
    }
    owned_source.buffer.reset();
    owned_source.backend.ptr = nullptr;
    if (!WaitUntil([&] { return session.Stats().written == 2; }) ||
        scripted->last_packet_value != 9) {
        std::cerr << "Async packet ownership was not independent" << std::endl;
        return 1;
    }

    // Block the consumer so the packet-count limit can be observed deterministically.
    scripted->block_push = true;
    MediaPacket blocked = MakeVideoPacket(false, 11);
    if (!session.Enqueue(blocked).Accepted() ||
        !WaitUntil([&] { return scripted->push_entered; })) {
        std::cerr << "Async session did not enter the blocking write" << std::endl;
        return 1;
    }
    MediaPacket queued = MakeVideoPacket(false, 12);
    MediaPacket full = MakeVideoPacket(false, 13);
    const PusherEnqueueResult queued_result = session.Enqueue(queued);
    const PusherEnqueueResult full_result = session.Enqueue(full);
    if (!queued_result.Accepted() ||
        full_result.status != PusherEnqueueStatus::QueueFull) {
        std::cerr << "Async queue capacity was not enforced" << std::endl;
        return 1;
    }

    if (!session.RequestStop().Succeed() || !session.Close().Succeed() ||
        scripted->close_calls.load() != 1 ||
        session.Stats().dropped_on_close == 0) {
        std::cerr << "Async stop/close did not discard pending packets" << std::endl;
        return 1;
    }

    std::cout << "AsyncPusherSession test passed" << std::endl;
    return 0;
}
