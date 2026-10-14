#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "media/media_frame.h"
#include "info/result.h"
#include "tensordata/tensor_frame.h"

struct TensorModelDesc;

/// @brief 模型配置结构体，仅描述模型语义层通用配置
struct ModelConfig {
    /// @brief 模型名称
    std::string name{""};
    /// @brief 是否支持动态形状
    bool dynamic_shape{false};
    /// @brief 类别数量
    int class_count{80};
    /// @brief 业务参数
    std::map<std::string, std::string> options;
};

/// @brief 任务类型枚举
enum class TaskType {
    DETECT,
    SEGMENT,
    POSE,
    OCR,
    CLASSIFY
};

/// @brief 模型元信息
struct ModelMeta {
    /// @brief 模型名称
    std::string name;
    /// @brief 任务类型
    TaskType task{TaskType::DETECT};
    /// @brief 输入宽度
    int input_width{0};
    /// @brief 输入高度
    int input_height{0};
    /// @brief 输入像素格式
    PixelFormat input_format{PixelFormat::kUnknown};
    /// @brief 是否动态输入
    bool dynamic_shape{false};
    /// @brief 类别数量
    int class_count{80};
};

/// @brief 模型语义层接口
class IModel {
public:
    virtual ~IModel() = default;

    /// @brief 初始化模型资源
    virtual bool Initialize(const ModelConfig& config) = 0;
    /// @brief 在 Session 创建阶段校验 provider 返回的模型 I/O 描述。
    ///
    /// 旧模型默认成功以保持兼容；需要显式输出契约的模型应覆盖该方法，
    /// 在首帧推理前拒绝名称、dtype、rank 或 shape 不匹配的模型。
    virtual InferenceStatus ValidateModelDesc(const TensorModelDesc& model_desc) const {
        (void)model_desc;
        return InferenceStatus::Success();
    }
    /// @brief 获取模型元信息
    virtual const ModelMeta& GetModelMeta() const = 0;
    /// @brief 更新模型元信息，用于推理引擎解析后回写输入形状
    virtual bool UpdateModelMeta(const ModelMeta& meta) {
        model_meta_ = meta;
        return true;
    }
    /// @brief 预处理媒体帧
    virtual TensorFrame Preprocess(const MediaFrame& frame) = 0;
    /// @brief 预处理并返回结构化状态
    ///
    /// 默认实现包装旧 TensorFrame 接口，空 TensorFrame 会转换为明确的输入错误；
    /// provider/model 可覆盖本方法以报告更精确的未初始化或格式错误。
    virtual InferenceStatus PreprocessWithStatus(const MediaFrame& frame, TensorFrame& output) {
        output = Preprocess(frame);
        if (output.Empty()) {
            return InferenceStatus::Failure(InferenceStatusCode::kInvalidInput,
                                            InferenceStage::kPreprocess,
                                            "Model preprocessing returned an empty tensor frame");
        }
        return InferenceStatus::Success();
    }
    /// @brief 后处理模型输出张量
    virtual FrameResult Postprocess(const TensorFrame& output) = 0;
    /// @brief 后处理并返回结构化状态
    ///
    /// 旧模型实现尚未填写 FrameResult::status 时，兼容包装把正常返回视为成功；
    /// 已迁移实现返回的明确失败状态会原样透传。
    virtual InferenceStatus PostprocessWithStatus(const TensorFrame& output,
                                                  FrameResult& result) {
        result = Postprocess(output);
        if (result.status.code == InferenceStatusCode::kUnknown) {
            result.status = InferenceStatus::Success();
        }
        return result.status;
    }

protected:
    ModelMeta model_meta_;  ///< 模型参数
};
