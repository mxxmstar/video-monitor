#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/i_model.h"
#include "info/inference_context.h"
#include "info/inference_status.h"
#include "tensordata/tensor_frame.h"

enum class EngineCapability : uint32_t {
    NONE = 0,
    ASYNC = 1u << 0, ///< 是否支持异步推理
    DYNAMIC = 1u << 1, ///< 是否支持动态形状
    BATCH = 1u << 2, ///< 是否支持批量推理
    CPU = 1u << 3, ///< 是否支持 CPU 推理
    GPU = 1u << 4, ///< 是否支持 GPU 推理
    CANCEL = 1u << 5, ///< 是否支持取消已派发的异步推理请求
};

/// @brief 推理引擎的生命周期状态。
///
/// 状态只描述引擎对象本身，不代表某一条 request 的执行状态。所有 provider
/// 都必须遵守同一套转换约定，这样 Session 和上层调度器无需依赖 provider 日志
/// 就能判断当前操作是否允许执行。
enum class EngineState {
    kCreated,   ///< 对象已创建，但尚未成功加载模型。
    kLoading,   ///< 正在加载或替换模型；暂不接受新的推理提交。
    kReady,     ///< 模型和 request 资源均已准备好，可以推理。
    kReleasing, ///< 正在停止新提交并等待已开始的 request 完成。
    kReleased,  ///< 资源已释放；可以再次调用 LoadModel。
    kFailed,    ///< 最近一次加载失败；可以再次调用 LoadModel。
};

inline EngineCapability operator|(EngineCapability lhs, EngineCapability rhs) {
    return static_cast<EngineCapability>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

inline EngineCapability& operator|=(EngineCapability& lhs, EngineCapability rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline bool HasEngineCapability(EngineCapability value, EngineCapability capability) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(capability)) != 0;
}

/// @brief 推理引擎配置和信息
struct EngineConfig {
    /// @brief 模型文件路径
    std::string model_path;
    /// @brief 后端类型
    std::string backend{"OPENVINO"};
    /// @brief 设备类型
    std::string device{"CPU"};
    /// @brief OpenVINO 编译后实际执行推理的设备。
    ///
    /// `device` 保存调用方请求的设备（例如 `AUTO`），而该字段保存 runtime
    /// 解析后的物理设备（例如 `CPU` 或 `GPU`）。在模型尚未成功加载时为空；
    /// 非 OpenVINO provider 可以按自身能力填充或保留为空。
    std::string actual_device;
    /// @brief 请求的模型 batch 大小；当前 OpenVINO 实现只接受 1。
    int batch_size{1};
    /// @brief 推理请求池大小
    uint32_t request_count{1};
    /// @brief 是否支持异步推理
    bool support_async{false};
    /// @brief 是否支持动态形状
    bool support_dynamic_shape{false};
    /// @brief 已验证的最大 batch 大小；当前固定为 1，不能据此声明 BATCH capability。
    uint32_t max_batch_size{1};
};

/// @brief OpenVINO 预处理配置
struct OpenVinoPreprocessConfig {
    /// @brief 是否启用 OpenVINO 内部预处理
    bool enabled{false};
    /// @brief OpenVINO 预处理输入像素格式
    PixelFormat input_pixel_format{PixelFormat::kNV12};
    /// @brief 模型期望的像素格式
    PixelFormat model_pixel_format{PixelFormat::kRGB24};
    /// @brief 模型输入布局，空字符串表示从模型输入形状推断
    std::string model_input_layout;
    /// @brief OpenVINO 缩放因子，0 表示不进行缩放
    float scale{255.0f};
};

/// @brief OpenVINO 加载期静态图像输入 reshape 配置。
///
/// 本配置只描述一次 `LoadModel()` 内完成的静态图重编译，不能用于按帧修改
/// 输入尺寸。当前 Session 只处理单帧图像和静态输出，因此限制完整 rank-4
/// shape、batch=1 和单模型输入；多输入映射、动态 batch、compiled-model cache
/// 必须在后续独立 profile 中定义资源所有权和淘汰语义后再开放。
struct OpenVinoStaticReshapeConfig {
    /// @brief 目标模型输入形状，必须是 `[1,C,H,W]` 或 `[1,H,W,C]` 的正整数维度。
    TensorShape input_shape;
};

/// @brief OpenVINO engine 内存 compiled-model cache 的显式配置。
///
/// 该 cache 只在一个 `OpenVinoEngine` 对象的生命周期内生效，条目包含已编译的
/// OpenVINO 图及其 I/O 描述，不包含可并发使用的 InferRequest。每次命中仍会按本次
/// `request_count` 新建 request pool，因此 request pool 的关闭/回调生命周期不与
/// 图缓存耦合。省略本配置时 cache 关闭；容量必须为正数，LRU 逐出只释放 cache
/// 自身的引用，已被当前 engine 使用的 compiled model 仍由活动成员持有。
struct OpenVinoCompiledModelCacheConfig {
    /// @brief 最多保留的编译图条目数，必须大于 0。
    uint32_t capacity{0};
};

/// @brief OpenVINO engine 内存 compiled-model cache 的只读运行统计。
///
/// hit/miss/eviction 是 engine 对象创建后的累积值；entry_count/capacity 描述当前
/// 载入配置下的 cache 状态。编译或 request pool 初始化失败不会写入 cache，也不会
/// 记录为可复用失败条目，避免临时设备/文件错误被永久放大为缓存故障。
struct OpenVinoCompiledModelCacheStats {
    uint32_t capacity{0};      ///< 当前启用的 LRU 容量；0 表示 cache 已关闭。
    uint32_t entry_count{0};   ///< 当前驻留的已成功编译图数量。
    uint64_t hit_count{0};     ///< 使用已有 compiled model 的次数。
    uint64_t miss_count{0};    ///< 启用 cache 后需要重新编译的次数。
    uint64_t eviction_count{0}; ///< 因容量限制移除最久未使用条目的次数。
};

/// @brief OpenVINO 编译期性能属性。
///
/// 字符串和值故意保持 provider 配置层的简单标量形态，避免在公共 Session API
/// 中暴露 `ov::hint::PerformanceMode` 等第三方枚举。空 performance_hint 和 0
/// num_streams 表示不覆盖 OpenVINO 默认值；num_streams 的 -1/-2 分别表示 AUTO
/// 和 NUMA，正数表示显式 stream 数量。
struct OpenVinoRuntimeConfig {
    /// @brief 性能模式：LATENCY、THROUGHPUT 或 CUMULATIVE_THROUGHPUT。
    std::string performance_hint;
    /// @brief stream 数量：0 使用默认，-1 使用 OpenVINO AUTO，-2 使用 NUMA，正数为显式数量。
    int32_t num_streams{0};
    /// @brief 推理线程数：0 使用插件默认，正数限制 inference worker 数量。
    int32_t inference_num_threads{0};
    /// @brief 是否收集 profiling 计数；未设置表示沿用插件默认。
    std::optional<bool> enable_profiling;
    /// @brief 模型缓存目录；空字符串表示不覆盖 OpenVINO 默认缓存策略。
    std::string cache_dir;
};

/// @brief OpenVINO 编译后运行时属性诊断快照。
///
/// `requested` 是调用方提交给 EngineLoadConfig 的配置，actual_* 是从
/// ov::CompiledModel 回读的最终值。不同设备插件支持的只读属性可能不完全相同，
/// 因此单项回读失败只会写入 readback_errors，不会让已成功编译的模型失效。
struct OpenVinoRuntimeSnapshot {
    /// @brief 编译时请求的性能配置。
    OpenVinoRuntimeConfig requested;
    /// @brief runtime 实际采用的性能模式；空字符串表示无法回读。
    std::string actual_performance_hint;
    /// @brief runtime 实际 stream 数，-1/-2 保留 AUTO/NUMA 语义，0 表示无法回读。
    int32_t actual_num_streams{0};
    /// @brief runtime 实际推理线程数，0 表示无法回读。
    int32_t actual_inference_num_threads{0};
    /// @brief runtime 实际 profiling 开关；无值表示无法回读。
    std::optional<bool> actual_enable_profiling;
    /// @brief runtime 实际 cache 目录；空字符串表示无法回读或未启用。
    std::string actual_cache_dir;
    /// @brief 是否所有已查询属性均成功回读。
    bool readback_complete{false};
    /// @brief 各属性独立回读失败的诊断文本。
    std::vector<std::string> readback_errors;
};

/// @brief 推理引擎加载配置
struct EngineLoadConfig {
    EngineConfig engine;    ///< 推理引擎配置
    std::optional<OpenVinoPreprocessConfig> preprocess; ///< OpenVINO 自带预处理
    /// @brief 可选的 OpenVINO 加载期静态 reshape；未设置时保留 IR 原始输入形状。
    std::optional<OpenVinoStaticReshapeConfig> reshape;
    std::optional<OpenVinoRuntimeConfig> runtime; // OpenVINO 性能 hint/streams 配置
    /// @brief 可选的单 engine 内存 compiled-model LRU；未设置时不保留旧编译图。
    std::optional<OpenVinoCompiledModelCacheConfig> compiled_model_cache;
};

/// @brief 输入/输出张量描述
struct TensorDesc {
    /// @brief 张量名称
    std::string name;
    /// @brief 张量元素类型
    TensorType type{TensorType::UNKNOWN};
    /// @brief 张量形状
    TensorShape shape;
    /// @brief 是否动态形状
    bool dynamic_shape{false};
    /// @brief 是否输入张量
    bool is_input{true};
    /// @brief 张量元素数量
    size_t element_count{0};
    /// @brief 张量字节数
    size_t bytes{0};
};

/// @brief 模型张量描述
struct TensorModelDesc {
    std::vector<TensorDesc> inputs;
    std::vector<TensorDesc> outputs;
};

/// @brief 异步推理输出
struct InferOutput {
    /// @brief 结构化执行状态；新代码应优先检查 status.Ok()。
    InferenceStatus status;
    /// @brief 兼容旧调用方的成功镜像字段，后续迁移完成后删除。
    bool success{false};
    TensorFrame output;
};

using InferCallback = std::function<void(const InferContext&, InferOutput&&)>;

/// @brief 推理引擎基类
/// 它仅加载模型、运行推理并返回输出张量
class IInferenceEngine {
public:
    virtual ~IInferenceEngine() = default;

    /// @brief 加载模型
    virtual bool LoadModel(const EngineLoadConfig& config) = 0;
    /// @brief 运行同步推理
    virtual bool Infer(const TensorFrame& input, TensorFrame& output) = 0;
    /// @brief 同步执行推理并返回结构化状态
    ///
    /// 默认实现包装旧 bool 接口，保证其他尚未迁移的 provider 仍可编译；已迁移
    /// 的 provider 应覆盖本方法并返回具体的 code/stage/message/detail。
    virtual InferenceStatus InferWithStatus(const TensorFrame& input, TensorFrame& output) {
        if (Infer(input, output)) {
            return InferenceStatus::Success();
        }
        return InferenceStatus::Failure(InferenceStatusCode::kProviderExecutionFailed,
                                        InferenceStage::kExecute,
                                        "Inference engine execution failed");
    }
    /// @brief 运行异步推理
    virtual bool InferAsync(const InferContext& ctx, const TensorFrame& input, InferCallback cb) = 0;
    /// @brief 提交异步推理并返回结构化提交状态
    ///
    /// 默认实现包装旧 bool 接口，provider 可覆盖它以区分关闭、输入错误和
    /// request 获取失败；异步执行完成后的 provider 状态仍通过 InferCallback 返回。
    virtual InferenceStatus InferAsyncWithStatus(const InferContext& ctx,
                                                  const TensorFrame& input,
                                                  InferCallback cb) {
        if (InferAsync(ctx, input, std::move(cb))) {
            return InferenceStatus::Success();
        }
        return InferenceStatus::Failure(InferenceStatusCode::kProviderExecutionFailed,
                                        InferenceStage::kSubmit,
                                        "Inference async submission failed");
    }
    /// @brief 释放推理引擎资源
    virtual void Release() = 0;
    /// @brief 获取引擎能力
    virtual EngineCapability Supports() const = 0;
    /// @brief 获取模型输入/输出张量描述
    virtual TensorModelDesc GetModelDesc() const = 0;
    /// @brief 获取推理引擎配置和信息
    virtual EngineConfig GetEngineConfig() const = 0;

    /// @brief 查询引擎生命周期状态。
    ///
    /// 默认返回 Created 以兼容尚未迁移状态机的 provider。新 provider 应覆盖
    /// 此方法并在内部用 mutex 保护状态读写，不能把普通 bool 当成并发协议。
    virtual EngineState GetState() const {
        return EngineState::kCreated;
    }
};
