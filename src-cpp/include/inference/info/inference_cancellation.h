#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

/// @brief 单次推理请求的协作式取消令牌。
///
/// 令牌可由上游在 Submit 前或 Submit 后调用 Cancel()。scheduler 会在任务派发前
/// 检查该状态；支持原生取消的 provider 可以通过 Register() 订阅取消并中断正在
/// 运行的 request。取消是幂等的，已经完成的请求不因晚到 Cancel 而获得第二终态。
class InferenceCancellationToken {
public:
    using Callback = std::function<void()>;

private:
    struct State {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        uint64_t next_registration_id{1};
        std::unordered_map<uint64_t, Callback> callbacks;
    };

public:
    /// @brief 取消回调的 RAII 注册句柄。
    ///
    /// provider 必须把句柄持有到 request 完成；析构或 Reset() 会移除回调，避免
    /// 调用方在 request 已归还池后取消同一个 token 时错误地取消被复用的 request。
    class Registration {
    public:
        Registration() = default;
        ~Registration() {
            Reset();
        }

        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;

        Registration(Registration&& other) noexcept
            : state_(std::move(other.state_)), registration_id_(other.registration_id_) {
            other.registration_id_ = 0;
        }

        Registration& operator=(Registration&& other) noexcept {
            if (this != &other) {
                Reset();
                state_ = std::move(other.state_);
                registration_id_ = other.registration_id_;
                other.registration_id_ = 0;
            }
            return *this;
        }

        /// @brief 解除订阅；可重复调用。
        void Reset() noexcept {
            if (registration_id_ == 0) {
                return;
            }
            if (auto state = state_.lock()) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->callbacks.erase(registration_id_);
            }
            state_.reset();
            registration_id_ = 0;
        }

    private:
        friend class InferenceCancellationToken;

        Registration(std::weak_ptr<State> state, uint64_t registration_id)
            : state_(std::move(state)), registration_id_(registration_id) {
        }

        std::weak_ptr<State> state_;
        uint64_t registration_id_{0};
    };

    InferenceCancellationToken() : state_(std::make_shared<State>()) {
    }

    /// @brief 请求取消；首次调用返回 true，重复调用返回 false。
    ///
    /// 回调在 token 内部锁外执行，避免 provider 的 cancel 实现重入注册或销毁
    /// registration 时发生锁顺序死锁。一个订阅者异常不会阻止其余 request 收到
    /// 取消信号，provider 负责记录自身的具体失败原因。
    bool Cancel() const noexcept {
        bool expected = false;
        if (!state_->cancelled.compare_exchange_strong(expected, true,
                                                        std::memory_order_acq_rel)) {
            return false;
        }

        std::vector<Callback> callbacks;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            callbacks.reserve(state_->callbacks.size());
            for (auto& [_, callback] : state_->callbacks) {
                callbacks.push_back(std::move(callback));
            }
            state_->callbacks.clear();
        }
        for (auto& callback : callbacks) {
            try {
                if (callback) {
                    callback();
                }
            } catch (...) {
                // 尽力通知其他订阅者；取消请求本身仍然保持已触发状态。
            }
        }
        return true;
    }

    /// @brief 查询是否已经请求取消。
    bool IsCancellationRequested() const noexcept {
        return state_->cancelled.load(std::memory_order_acquire);
    }

    /// @brief 返回当前仍订阅原生取消通知的 request 数量。
    ///
    /// 此值是诊断快照，不能据此推导某个 request 已经真正进入设备执行，也不能
    /// 作为业务流控条件。它用于长稳/集成测试确认 provider 已持有 cancellation
    /// registration，避免在 provider 注册前过早 Cancel 而把“设备取消延迟”误测为
    /// scheduler 或预处理阶段的本地取消。完成、Reset 或 Cancel 后应自然回到 0。
    std::size_t GetActiveRegistrationCount() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->callbacks.size();
    }

    /// @brief 注册运行中 request 的原生取消回调。
    ///
    /// 若 token 已取消，callback 会在本方法返回前同步执行，返回空 registration；
    /// 调用方应在启动 provider request 前后再次检查 IsCancellationRequested()，
    /// 以覆盖注册和 start_async 之间的竞争窗口。
    Registration Register(Callback callback) const {
        if (!callback) {
            return {};
        }

        uint64_t registration_id = 0;
        bool run_immediately = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->cancelled.load(std::memory_order_acquire)) {
                run_immediately = true;
            } else {
                registration_id = state_->next_registration_id++;
                if (registration_id == 0) {
                    registration_id = state_->next_registration_id++;
                }
                state_->callbacks.emplace(registration_id, std::move(callback));
            }
        }
        if (run_immediately) {
            callback();
            return {};
        }
        return Registration(state_, registration_id);
    }

private:
    std::shared_ptr<State> state_;
};
