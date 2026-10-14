#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>

#include <openvino/openvino.hpp>

#include "engine/i_engine.h"
#include "engine/openvino_request_pool.h"

/// @brief OpenVINO 通用推理引擎。
///
/// 引擎允许调用方请求 CPU 或 AUTO。AUTO 的设备选择由 OpenVINO runtime 完成，
/// 加载成功后通过 EngineConfig::actual_device 暴露实际执行设备；上层不需要
/// 依赖 provider 私有类型即可读取设备和 capability。
class OpenVinoEngine : public IInferenceEngine {
public:
    OpenVinoEngine();
    ~OpenVinoEngine() override;

    /// @brief 加载模型
    bool LoadModel(const EngineLoadConfig& config) override;
    /// @brief 运行同步推理
    bool Infer(const TensorFrame& input, TensorFrame& output) override;
    /// @brief 同步执行推理并返回 OpenVINO 结构化状态
    InferenceStatus InferWithStatus(const TensorFrame& input, TensorFrame& output) override;
    /// @brief 运行异步推理
    bool InferAsync(const InferContext& ctx, const TensorFrame& input, InferCallback cb) override;
    /// @brief 提交异步推理并返回 OpenVINO 结构化提交状态
    InferenceStatus InferAsyncWithStatus(const InferContext& ctx,
                                         const TensorFrame& input,
                                         InferCallback cb) override;
    /// @brief 释放推理引擎资源
    void Release() override;
    /// @brief 获取引擎能力
    EngineCapability Supports() const override;
    /// @brief 获取模型输入/输出张量描述
    TensorModelDesc GetModelDesc() const override;
    /// @brief 获取推理引擎配置和信息
    EngineConfig GetEngineConfig() const override;
    /// @brief 获取 OpenVINO 引擎生命周期状态
    EngineState GetState() const override;
    /// @brief 获取最近一次成功编译的 OpenVINO runtime 属性诊断快照。
    OpenVinoRuntimeSnapshot GetRuntimeSnapshot() const;
    /// @brief 获取单 engine 内存 compiled-model LRU 的当前统计。
    OpenVinoCompiledModelCacheStats GetCompiledModelCacheStats() const;

private:
    /// @brief OpenVINO Core
    ov::Core core_;
    /// @brief 编译模型
    ov::CompiledModel compiled_model_;
    /// @brief 推理请求池
    std::shared_ptr<OpenVinoInferRequestPool> request_pool_;
    /// @brief 引擎配置
    EngineConfig config_;
    /// @brief 模型张量描述
    TensorModelDesc tensor_model_desc_;
    /// @brief OpenVINO 预处理配置
    OpenVinoPreprocessConfig preprocess_config_;
    /// @brief 最近一次成功编译的 runtime 请求/回读属性。
    OpenVinoRuntimeSnapshot runtime_snapshot_;
    /// @brief 一条可被后续 LoadModel 复用的已编译图及其 provider 描述。
    ///
    /// 该结构故意不保存 OpenVinoInferRequestPool。request 属于一次 engine 激活期，
    /// 可能仍有异步 callback 正在 drain；把它跨 profile 缓存会破坏 Release 的关闭
    /// 顺序。CompiledModel 可以独立复用，加载命中时仍创建全新的 request pool。
    struct CompiledModelCacheEntry {
        std::string key;
        ov::CompiledModel compiled_model;
        bool support_dynamic_shape{false};
        std::string actual_device;
        TensorModelDesc tensor_model_desc;
        OpenVinoPreprocessConfig preprocess_config;
        OpenVinoRuntimeSnapshot runtime_snapshot;
    };
    /// @brief 最近使用的条目位于 front，最久未使用的条目位于 back。
    std::list<CompiledModelCacheEntry> compiled_model_cache_;
    /// @brief cache 命中、未命中和 LRU 逐出的累计诊断，受 state_mutex_ 保护。
    OpenVinoCompiledModelCacheStats compiled_model_cache_stats_;
    /// @brief 保护模型对象、配置和生命周期状态的互斥锁。
    ///
    /// 推理执行本身不持有该锁，而是先复制 shared_ptr/CompiledModel；这样释放
    /// 线程可以及时切断新提交，同时让已经取得 request 的调用自然完成。
    mutable std::mutex state_mutex_;
    /// @brief Load/Release 等状态切换的等待条件。
    std::condition_variable state_cv_;
    /// @brief 当前生命周期状态，所有访问必须位于 state_mutex_ 临界区内。
    EngineState state_{EngineState::kCreated};
};

/// @brief 兼容旧调用方的 OpenVINO CPU 引擎名称。
///
/// 这里保留一个没有额外状态的 facade，而不是只写 type alias：MSVC 增量链接或
/// 已经编译的旧目标可能仍然引用 `OpenVinoCpuEngine` 的构造函数符号。facade 让
/// 这些目标平滑过渡到 `OpenVinoEngine` 的实现，同时不再把运行时能力限制为 CPU。
class OpenVinoCpuEngine final : public OpenVinoEngine {
public:
    OpenVinoCpuEngine();
    ~OpenVinoCpuEngine() override;
};
