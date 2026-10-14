#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

#include <openvino/openvino.hpp>

/// @brief OpenVINO 推理请求池
class OpenVinoInferRequestPool {
public:
    /// @brief 初始化推理池
    /// @param model 编译后的模型
    /// @param request_count 推理请求数量
    /// @return 是否初始化成功
    bool Initialize(ov::CompiledModel& model, uint32_t request_count);
    /// @brief 获取推理请求
    std::shared_ptr<ov::InferRequest> Acquire();
    /// @brief 释放推理请求
    void Release(std::shared_ptr<ov::InferRequest> request);
    /// @brief 标记异步推理已经完成，但暂不把 request 放回空闲队列
    ///
    /// OpenVINO callback 进入用户 callback 前调用本方法。此时 request 的推理
    /// 已完成，关闭线程可以不再等待它；但 request 仍由 callback 局部变量持有，
    /// 因而不会在用户 callback 返回前被另一个推理复用。
    void MarkAsyncCompleted();
    /// @brief 在用户异步 callback 返回后归还 request
    ///
    /// 该方法与 MarkAsyncCompleted() 成对调用，先减少“已完成但仍在 callback
    /// 生命周期内”的计数，再把 request 放回队列并唤醒等待者。
    void ReleaseAsync(std::shared_ptr<ov::InferRequest> request);
    /// @brief 停止接收新的推理请求并唤醒等待线程
    void Shutdown();

    /// @brief 等待所有异步推理请求完成
    bool WaitAll();
    
    /// @brief 获取空闲推理请求数量
    size_t IdleCount() const;
    /// @brief 获取总推理请求数量
    size_t TotalCount() const;

private:
    /// @brief 空闲推理请求队列
    std::queue<std::shared_ptr<ov::InferRequest>> idle_requests_;
    /// @brief 互斥锁
    mutable std::mutex mutex_;
    /// @brief 条件变量
    std::condition_variable cv_;
    /// @brief 总推理请求数量
    size_t total_count_{0};
    /// @brief 已完成 OpenVINO 推理、但仍在用户 callback 生命周期内的 request 数
    ///
    /// WaitAll() 将 idle_requests_ 与本计数相加判断是否所有请求都已完成。这样
    /// 用户 callback 内重入 engine->Release() 时不会等待自己归还 request，同时
    /// request 仍不会在 callback 返回前进入 idle_requests_。
    size_t completed_async_count_{0};
    /// @brief 是否已经关闭请求池
    bool shutdown_{false};
};

/// @brief request_pool 的 RAII 包装
class RequestLease {
public:
    RequestLease(std::shared_ptr<OpenVinoInferRequestPool> pool, std::shared_ptr<ov::InferRequest> request)
        : pool_(std::move(pool)),
          request_(std::move(request)) {
    }

    ~RequestLease() {
        if (pool_ && request_) {
            pool_->Release(std::move(request_));
        }
    }

    bool Valid() const {
        return request_ != nullptr;
    }

    /// @brief 将 request 转移给异步完成回调，并放弃 lease 对 pool 的持有
    ///
    /// 异步回调由 OpenVINO 保存在 request 内部。如果回调同时强引用 lease，
    /// 而 lease 又继续持有 request/pool，就会形成 request -> callback -> lease
    /// -> pool -> request 的循环。异步路径必须在回调开始时调用本方法：request
    /// 由回调局部变量保持到回调结束，lease 则不再持有任何运行时资源。
    std::shared_ptr<ov::InferRequest> TakeRequestForAsync() {
        pool_.reset();
        return std::move(request_);
    }

    ov::InferRequest& operator*() {
        return *request_;
    }

    ov::InferRequest* operator->() {
        return request_.get();
    }

private:
    std::shared_ptr<OpenVinoInferRequestPool> pool_;
    std::shared_ptr<ov::InferRequest> request_;
};
