#pragma once

#include <optional>
#include <string>
#include <vector>

#include "info/result.h"
#include "postprocessor/i_postprocessor.h"
#include "tensordata/tensor_frame.h"

/// @brief YOLO 输出的两个二维布局方向。
///
/// `kCandidatesAttributes` 对应 `[N, attrs]`，`kAttributesCandidates` 对应
/// `[attrs, N]`。`kAuto` 只用于兼容尚未迁移的旧配置，新模型应明确声明方向。
enum class YoloOutputLayout {
    kCandidatesAttributes,  ///< 每行是一个候选框，列是属性，对应 `[N, attrs]`
    kAttributesCandidates,  ///< 每列是一个属性，行是候选框，对应 `[attrs, N]`
    kAuto,                  ///< 自动根据模型配置推断布局
};

/// @brief YOLO 输出属性的编码方式。
///
/// `kClassScores` 是 YOLOv5/YOLOv8 原始 Detect 输出，属性中保存每个类别分数；
/// `kNmsFused` 表示导出图已经执行 NMS，单个候选固定为 xyxy、score、class_id；
/// `kNmsSplit` 则把 boxes、scores、classes 放在三个具名输出端口中。
enum class YoloOutputEncoding {
    kClassScores,   ///< 输出包含每类的置信度
    kNmsFused,      ///< 输出已融合 NMS，每个候选框包含 xyxy、score、class_id
    kNmsSplit,      ///< 输出 boxes、scores、classes 分离
};

/// @brief YOLO 输出 tensor 的显式语义 schema。
struct YoloOutputSchema {
    /// @brief 可选的输出 tensor 名称；为空时保留单输出兼容查找。
    std::string tensor_name;
    /// @brief NMS 输出可选的有效检测数量 tensor 名称。
    ///
    /// 部分导出图会将固定容量的 `[N, 6]` 检测记录与 `num_detections` 分成两个
    /// 输出端口。该字段非空时，decoder 只读取计数范围内的记录；它只适用于
    /// `kNmsFused` 或 `kNmsSplit`。融合记录必须通过 `tensor_name` 指定主输出；
    /// 分离端口则由下方三个字段指定，避免多输出模型依赖端口枚举顺序。
    std::string valid_detections_tensor_name;
    /// @brief `kNmsSplit` 的 xyxy boxes 输出名称，shape 仅接受 `[N, 4]` 或 `[1, N, 4]`。
    std::string boxes_tensor_name;
    /// @brief `kNmsSplit` 的每候选 score 输出名称，shape 仅接受 `[N]` 或 `[1, N]`。
    std::string scores_tensor_name;
    /// @brief `kNmsSplit` 的每候选 class_id 输出名称，shape 与 scores 必须一致。
    std::string classes_tensor_name;
    /// @brief 候选框和属性维度的排列方向。
    YoloOutputLayout layout{YoloOutputLayout::kCandidatesAttributes};
    /// @brief 类别数，用于校验 attrs，而不是从 attrs 值反推类别数。
    int class_count{80};
    /// @brief 输出是否包含 objectness 属性；YOLOv5 为 true，YOLOv8 常为 false。
    bool has_objectness{true};
    /// @brief 属性字段的编码约定，决定 decoder 的 attrs 校验和是否还需执行 NMS。
    YoloOutputEncoding encoding{YoloOutputEncoding::kClassScores};
};

/// @brief OpenVINO YOLO postprocessor config.
struct OpenVinoYoloPostprocessorConfig {
    float conf_threshold{0.25f};
    float nms_threshold{0.45f};
    int input_width{640};
    int input_height{640};
    YoloOutputSchema output_schema;
};

class YoloPostprocessor : public IPostprocessor {
public:
    bool Initialize(const OpenVinoYoloPostprocessorConfig& config);

    FrameResult Process(const TensorFrame& output) override;

private:
    /// @brief 将一个 YOLO 输出端口解码为检测框。
    /// @param tensor 待解析的 YOLO 输出 tensor
    /// @param status 输出结构化解析状态；失败时不会返回可用检测结果
    /// @param candidate_limit 多输出融合 NMS 的有效记录上限；为空时读取 shape 中全部候选。
    ///        该上限已由独立的 `num_detections` 端口校验，不能大于主输出候选数。
    std::vector<DetectionBox> decode(const TensorPlane& tensor,
                                     InferenceStatus& status,
                                     std::optional<size_t> candidate_limit = std::nullopt);

    /// @brief 解码 boxes、scores、classes 分离的融合 NMS 输出。
    ///
    /// 三个端口的名称、dtype 和候选数量已在 Session 初始化时由模型描述校验；此处仍
    /// 对实际 TensorFrame 的 shape、buffer 和有效检测计数复核，防止 provider 返回
    /// 与已加载模型不一致的输出时越界读取或产生伪检测。
    std::vector<DetectionBox> decodeSplitNms(const TensorFrame& output,
                                             InferenceStatus& status);

    std::vector<DetectionBox> nms(std::vector<DetectionBox>& detections);

    Rectangle restoreBox(const Rectangle& box);

private:
    float conf_threshold_{0.25f};

    float nms_threshold_{0.45f};

    YoloOutputSchema output_schema_;

    TensorMeta tensor_meta_;
};
