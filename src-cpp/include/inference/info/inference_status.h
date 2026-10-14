#pragma once

#include <string>
#include <utility>

/// @brief 推理结果或请求失败的稳定分类。
///
/// 该枚举只描述跨 provider 都有意义的公共语义；OpenVINO、ONNX Runtime
/// 等后端的原始错误文本应放在 InferenceStatus::provider_detail 中，而不是
/// 继续让上层通过日志内容猜测失败原因。
enum class InferenceStatusCode {
    kOk,
    kUnknown,
    kNotInitialized,
    kShuttingDown,
    kInvalidInput,
    kModelIncompatible,
    kQueueFull,
    kCancelled,
    kDeadlineExceeded,
    kRequestUnavailable,
    kProviderExecutionFailed,
    kOutputCollectionFailed,
    kPostprocessFailed,
    kCallbackFailed,
};

/// @brief 发生状态的推理阶段。
enum class InferenceStage {
    kNone,
    kLoad,
    kPreprocess,
    kSubmit,
    kExecute,
    kOutputCollection,
    kPostprocess,
    kCallback,
    kRelease,
};

/// @brief 可编程检查的推理状态。
struct InferenceStatus {
    /// @brief 公共错误分类；默认值不是成功，避免空构造结果被误判为成功。
    InferenceStatusCode code{InferenceStatusCode::kUnknown};
    /// @brief 发生状态的阶段。
    InferenceStage stage{InferenceStage::kNone};
    /// @brief 面向调用方的稳定、简短描述。
    std::string message;
    /// @brief provider 原始错误或补充诊断信息，不要求上层解析其格式。
    std::string provider_detail;

    /// @brief 判断状态是否代表推理成功。
    bool Ok() const noexcept {
        return code == InferenceStatusCode::kOk;
    }

    /// @brief 创建成功状态。
    static InferenceStatus Success() {
        return {InferenceStatusCode::kOk, InferenceStage::kNone, {}, {}};
    }

    /// @brief 创建结构化失败状态。
    static InferenceStatus Failure(InferenceStatusCode status_code,
                                   InferenceStage status_stage,
                                   std::string status_message,
                                   std::string detail = {}) {
        return {status_code,
                status_stage,
                std::move(status_message),
                std::move(detail)};
    }
};
