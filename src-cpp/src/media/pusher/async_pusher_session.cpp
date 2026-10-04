#include "media/pusher/async_pusher_session.h"

#include <memory>
#include <utility>

#include "media/ffmpeg_packet_buffer.h"

extern "C" {
#include <libavcodec/packet.h>
}

namespace {

struct AvPacketDeleter {
    void operator()(AVPacket* packet) const noexcept {
        if (packet) {
            av_packet_free(&packet);
        }
    }
};

}  // namespace

AsyncPusherSession::AsyncPusherSession()
    : AsyncPusherSession(std::make_unique<PusherSession>()) {}

AsyncPusherSession::AsyncPusherSession(std::unique_ptr<PusherSession> session)
    : session_(std::move(session)) {}

AsyncPusherSession::~AsyncPusherSession() {
    (void)Close();
}

PusherError AsyncPusherSession::makeError(PusherErrorCategory category, const char* message) {
    return PusherError{category, message, false};
}

std::optional<MediaPacket> AsyncPusherSession::shallowClonePacket(const MediaPacket& packet, PusherError& error) {
    if (!packet.buffer || packet.buffer->Size() == 0 || !IsValidTimeBase(packet.time_base) ||
        packet.backend.type != BackendHandle::FFMPEG || packet.backend.ptr == nullptr) {
        error = makeError(PusherErrorCategory::InvalidPacket, "AsyncPusherSession received an invalid FFmpeg packet");
        return std::nullopt;
    }

    const auto* source = static_cast<const AVPacket*>(packet.backend.ptr);
    if (!source->data || source->size <= 0 || (IsValidTimestamp(packet.duration) && packet.duration < 0)) {
        error = makeError(PusherErrorCategory::InvalidPacket, "AsyncPusherSession received an invalid packet payload");
        return std::nullopt;
    }

    std::unique_ptr<AVPacket, AvPacketDeleter> copy(av_packet_alloc());
    if (!copy || av_packet_ref(copy.get(), source) < 0) {
        error = makeError(PusherErrorCategory::Internal, "AsyncPusherSession could not clone the FFmpeg packet");
        return std::nullopt;
    }

    MediaPacket owned = packet;
    owned.buffer = std::make_shared<FFmpegPacketBuffer>(copy.release());
    owned.backend.type = BackendHandle::FFMPEG;
    owned.backend.ptr = static_cast<FFmpegPacketBuffer*>(owned.buffer.get())->GetPacket();
    return owned;
}

PusherResult AsyncPusherSession::Open(const PusherSessionConfig& config) {
    const PusherResult close_result = Close();
    if (!close_result.Succeed()) {
        return close_result;
    }
    if (!session_) {
        return PusherResult::Failed(makeError(PusherErrorCategory::Internal,
            "AsyncPusherSession has no synchronous session"));
    }
    if (config.mode != PusherSessionMode::Asynchronous) {
        return PusherResult::Failed(makeError(PusherErrorCategory::InvalidConfiguration, "AsyncPusherSession requires asynchronous session mode"));
    }
    if (!config.is_valid() || config.async.queue_capacity == 0) {
        return PusherResult::Failed(makeError(PusherErrorCategory::InvalidConfiguration, "AsyncPusherSession received an invalid configuration"));
    }

    const PusherResult open_result = session_->Open(config);
    if (!open_result.Succeed()) {
        return open_result;
    }

    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        queue_ = std::make_unique<BoundedSpscQueue<QueuedPacket>>(config.async.queue_capacity);
        queued_count_ = 0;
        pending_keyframes_ = 0;
        close_mode_ = config.async.close_mode;
        ++generation_;
        accepting_.store(true);
        stop_requested_.store(false);
        close_requested_.store(false);
        discard_requested_.store(false);
        waiting_for_keyframe_.store(true);
    }
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_ = {};
    }

    try {
        worker_thread_ = std::thread(&AsyncPusherSession::workerLoop, this, generation_);
    } catch (...) {
        accepting_.store(false);
        close_requested_.store(true);
        discard_requested_.store(true);
        (void)session_->RequestStop();
        (void)session_->Close();
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        queue_.reset();
        queued_count_ = 0;
        pending_keyframes_ = 0;
        return PusherResult::Failed(makeError(PusherErrorCategory::Internal, "AsyncPusherSession could not start its worker thread"));
    }
    return PusherResult::Success();
}

PusherEnqueueResult AsyncPusherSession::Enqueue(const MediaPacket& packet) {
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!accepting_.load() || !queue_ || stop_requested_.load()) {
            return PusherEnqueueResult::Rejected(makeError(PusherErrorCategory::InvalidState, "AsyncPusherSession is not accepting packets"));
        }
        if (waiting_for_keyframe_.load() && !packet.keyframe) {
            // 如果关键帧已经入队但尚未写出，允许其后的包跟随入队；真正的
            // 状态迁移仍由写入线程在关键帧写入结果返回后提交。
            if (pending_keyframes_ == 0) {
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                ++stats_.dropped_awaiting_keyframe;
                return PusherEnqueueResult::DroppedAwaitingKeyframe();
            }
        }

        // BoundedSpscQueue 的容量参数用于预分配，try_enqueue 只保证无分配，
        // 因此在 Session 层显式执行容量上限检查。
        if (queued_count_ >= queue_->capacity()) {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            ++stats_.dropped_queue_full;
            return PusherEnqueueResult::QueueFull();
        }
    }
    

    PusherError clone_error{};
    std::optional<MediaPacket> owned = shallowClonePacket(packet, clone_error);
    if (!owned.has_value()) {
        return PusherEnqueueResult::Rejected(std::move(clone_error));
    }

    QueuedPacket queued{std::move(*owned), generation_};
    const bool queued_keyframe = queued.packet.keyframe;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!queue_->push(std::move(queued))) {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            ++stats_.dropped_queue_full;
            return PusherEnqueueResult::QueueFull();
        }
        ++queued_count_;
        if (queued_keyframe) {
            ++pending_keyframes_;
        }
        recordQueueSize(queued_count_);
    }

    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        ++stats_.accepted;
    }
    notifyWorker();
    return PusherEnqueueResult::AcceptedResult();
}

PusherResult AsyncPusherSession::Close() {
    if (!session_) {
        accepting_.store(false);
        close_requested_.store(false);
        discard_requested_.store(false);
        stop_requested_.store(false);
        return PusherResult::Success();
    }
    bool has_async_work = false;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        accepting_.store(false);
        close_requested_.store(true);
        has_async_work = queue_ != nullptr || worker_thread_.joinable();
        if (close_mode_ == PusherAsyncCloseMode::Discard) {
            discard_requested_.store(true);
        }
    }

    if (has_async_work &&
        (discard_requested_.load() || stop_requested_.load())) {
        (void)session_->RequestStop();
    }
    notifyWorker();
    if (worker_thread_.joinable()) {
        if (worker_thread_.get_id() == std::this_thread::get_id()) {
            return PusherResult::Failed(makeError(PusherErrorCategory::Internal, "AsyncPusherSession cannot close from its worker thread"));
        }
        worker_thread_.join();
    }

    const PusherResult result = session_->Close();
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        queue_.reset();
        queued_count_ = 0;
        pending_keyframes_ = 0;
        close_requested_.store(false);
        discard_requested_.store(false);
        stop_requested_.store(false);
    }
    return result;
}

PusherSessionState AsyncPusherSession::State() const {
    return session_ ? session_->State() : PusherSessionState::Failed;
}

std::optional<PusherEvent> AsyncPusherSession::LastEvent() const {
    return session_ ? session_->LastEvent() : std::nullopt;
}

PusherResult AsyncPusherSession::RequestStop() {
    accepting_.store(false);
    stop_requested_.store(true);
    discard_requested_.store(true);
    notifyWorker();
    if (!session_) {
        return PusherResult::Success();
    }
    return session_->RequestStop();
}

AsyncPusherStats AsyncPusherSession::Stats() const {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(stats_mutex_);
    AsyncPusherStats result = stats_;
    result.queued_packets = queued_count_;
    return result;
}

void AsyncPusherSession::workerLoop(std::uint64_t generation) {
    for (;;) {
        std::size_t queued_count = 0;
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            queued_count = queued_count_;
        }
        if ((close_requested_.load() || stop_requested_.load()) && (discard_requested_.load() || queued_count == 0)) {
            if (discard_requested_.load()) {
                clearQueue(true);
            }
            return;
        }

        // 取出一个包，若队列为空则等待条件变量
        QueuedPacket queued;
        bool popped = false;
        std::size_t remaining = 0;
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            if (queue_ && queue_->pop(queued)) {
                if (queued_count_ > 0) {
                    --queued_count_;
                }
                remaining = queued_count_;
                popped = true;
            }
        }
        if (popped) {
            recordQueueSize(remaining);
            const auto finish_keyframe = [this, &queued] {
                if (!queued.packet.keyframe) {
                    return; // 不是关键帧无需处理
                }
                std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                if (pending_keyframes_ > 0) {
                    --pending_keyframes_;
                }
            };
            // 如果代际号不匹配，说明当前包属于旧会话，直接丢弃；如果请求丢弃或停止，也直接丢弃。
            if (queued.generation != generation || discard_requested_.load() || stop_requested_.load()) {
                finish_keyframe();
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                ++stats_.dropped_on_close;
                continue;
            }

            const PusherPublishResult result = session_->Publish(queued.packet);
            finish_keyframe();
            if (result.WasPublished()) {
                waiting_for_keyframe_.store(false);
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                ++stats_.written;
                continue;
            }
            if (result.status == PusherPublishStatus::DroppedAwaitingKeyframe) {
                waiting_for_keyframe_.store(true);
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                ++stats_.dropped_awaiting_keyframe;
                continue;
            }

            {
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                ++stats_.write_failures;
            }
            clearQueue(stop_requested_.load() || close_requested_.load());
            if (session_->State() == PusherSessionState::Failed) {
                accepting_.store(false);
                waiting_for_keyframe_.store(false);
                return;
            }
            waiting_for_keyframe_.store(true);
            continue;
        }

        std::unique_lock<std::mutex> wait_lock(worker_wait_mutex_);
        worker_cv_.wait(wait_lock, [this] {
            return close_requested_.load() || stop_requested_.load() || (queue_ && !queue_->empty());
        });
    }
}

void AsyncPusherSession::clearQueue(bool count_as_close) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!queue_) {
        return;
    }

    QueuedPacket queued;
    std::size_t dropped = 0;
    while (queue_->pop(queued)) {
        ++dropped;
        if (queued_count_ > 0) {
            --queued_count_;
        }
        if (queued.packet.keyframe && pending_keyframes_ > 0) {
            --pending_keyframes_;
        }
    }
    if (dropped == 0) {
        return;
    }
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    if (count_as_close) {
        stats_.dropped_on_close += dropped;
    } else {
        stats_.dropped_on_failure += dropped;
    }
    stats_.queued_packets = queued_count_;
}

void AsyncPusherSession::recordQueueSize(std::size_t size) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.queued_packets = size;
    if (size > stats_.high_water_mark) {
        stats_.high_water_mark = size;
    }
}

void AsyncPusherSession::notifyWorker() {
    worker_cv_.notify_one();
}