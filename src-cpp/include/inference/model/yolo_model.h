#pragma once

#include <memory>

#include "model/i_model.h"
#include "postprocessor/i_postprocessor.h"
#include "postprocessor/openvino_postprocessor.h"
#include "preprocessor/i_preprocessor.h"
#include "preprocessor/openvino_preprocessor.h"

class YoloModel : public IModel {
public:
    /// @brief 初始化模型资源
    /// @param config 模型配置
    /// @return 是否初始化成功
    bool Initialize(const ModelConfig& config) override;

    /// @brief 校验 OpenVINO 编译模型的 YOLO 输出契约。
    ///
    /// 验证模型描述是否与 YOLO 配置兼容，在加载阶段检查模型输出的张量形状、类型、布局是否符合预期。
    InferenceStatus ValidateModelDesc(const TensorModelDesc& model_desc) const override;

    /// @brief 获取模型元信息
    const ModelMeta& GetModelMeta() const override;

    /// @brief 更新模型元信息
    bool UpdateModelMeta(const ModelMeta& meta) override;

    /// @brief 预处理媒体帧
    /// 将原始 YUV plane 封装成 TensorFrame，让 OpenVINO 内部预处理处理颜色转换和 resize。
    /// @param frame 媒体帧数据
    /// @return 处理后的张量帧
    TensorFrame Preprocess(const MediaFrame& frame) override;
    /// @brief YOLO 预处理并返回结构化状态
    InferenceStatus PreprocessWithStatus(const MediaFrame& frame, TensorFrame& output) override;

    /// @brief 后处理模型输出张量
    /// Decode + NMS + Result
    /// @param output 模型输出张量
    /// @return 视频帧结果
    FrameResult Postprocess(const TensorFrame& output) override;
    /// @brief YOLO 后处理并返回结构化状态
    InferenceStatus PostprocessWithStatus(const TensorFrame& output, FrameResult& result) override;

private:
    /// @brief 重置预处理器
    bool resetPreprocessor();

    /// @brief 重置后处理器
    bool resetPostprocessor();

    std::unique_ptr<IPreprocessor> preprocessor_;   ///< 预处理器
    std::unique_ptr<IPostprocessor> postprocessor_; ///< 后处理器
    YoloOutputSchema output_schema_; ///< YOLO 输出契约，初始化时由 ModelConfig.options 解析
    ResizeMode resize_mode_{ResizeMode::kStretch}; ///< 当前已验证的输入几何策略
    bool initialized_{false}; ///< 初始化状态
};
