#include "engine/openvino_request_pool.h"
#include "inference/inference_logger.h"

bool OpenVinoInferRequestPool::Initialize(ov::CompiledModel& model, uint32_t request_count) {
    if (request_count == 0) {
        LOG_MAIN_ERROR_AT("request_count must be greater than 0");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = false;
    total_count_ = 0;
    completed_async_count_ = 0;

    if (!idle_requests_.empty()) {
        LOG_MAIN_WARN_AT("OpenVINO request pool already initialized, clearing...");
        while (!idle_requests_.empty()) {
            idle_requests_.pop();
        }
    }

    for (uint32_t i = 0; i < request_count; ++i) {
        try {
            auto request = std::make_shared<ov::InferRequest>(model.create_infer_request());
            idle_requests_.push(std::move(request));
        } catch (const std::exception& e) {
            LOG_MAIN_ERROR_AT("Failed to create infer request {}: {}", i, e.what());
            while (!idle_requests_.empty()) {
                idle_requests_.pop();
            }
            total_count_ = 0;
            completed_async_count_ = 0;
            return false;
        }
    }

    total_count_ = idle_requests_.size();
    LOG_MAIN_INFO_AT("OpenVINO request pool initialized with {} requests", total_count_);
    return true;
}

std::shared_ptr<ov::InferRequest> OpenVinoInferRequestPool::Acquire() {
    std::unique_lock<std::mutex> lock(mutex_);

    cv_.wait(lock, [this]() { return shutdown_ || !idle_requests_.empty(); });
    if (shutdown_ || idle_requests_.empty()) {
        return nullptr;
    }

    auto request = std::move(idle_requests_.front());
    idle_requests_.pop();

    return request;
}

void OpenVinoInferRequestPool::Release(std::shared_ptr<ov::InferRequest> request) {
    if (!request) {
        LOG_MAIN_WARN_AT("Release nullptr request, ignored");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        idle_requests_.push(std::move(request));
    }

    cv_.notify_one();
}

void OpenVinoInferRequestPool::MarkAsyncCompleted() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 推理已经由 OpenVINO 完成，但 callback 仍可能正在执行用户代码。
        // 此时只更新关闭所需的 bookkeeping，不把 request 放回 idle 队列，
        // 以免另一个提交在当前 callback 返回前复用同一个 request。
        ++completed_async_count_;
    }

    // Release() 线程可能正在 WaitAll() 中等待“全部 request 已完成”。
    cv_.notify_all();
}

void OpenVinoInferRequestPool::ReleaseAsync(std::shared_ptr<ov::InferRequest> request) {
    if (!request) {
        LOG_MAIN_WARN_AT("ReleaseAsync nullptr request, ignored");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 正常路径必定先调用 MarkAsyncCompleted()。保留防御性检查，避免
        // 错误的重复归还导致 size_t 下溢并破坏 WaitAll() 的关闭判断。
        if (completed_async_count_ == 0) {
            LOG_MAIN_WARN_AT("ReleaseAsync called without a completed async request");
        } else {
            --completed_async_count_;
        }
        idle_requests_.push(std::move(request));
    }

    cv_.notify_all();
}

void OpenVinoInferRequestPool::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }

    cv_.notify_all();
}

bool OpenVinoInferRequestPool::WaitAll() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() {
        // 已完成但尚未离开用户 callback 的 request 不在 idle 队列中，必须
        // 纳入完成数量，否则 callback 内调用 engine->Release() 会自我等待。
        return idle_requests_.size() + completed_async_count_ >= total_count_;
    });

    return true;
}

size_t OpenVinoInferRequestPool::IdleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return idle_requests_.size();
}

size_t OpenVinoInferRequestPool::TotalCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_count_;
}
