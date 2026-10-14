#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <optional>

#include "inference_context.h"
#include "inference_status.h"
#if 0
/// @brief  推理结果类型枚举
enum class ResultType {
    UNKNOWN,     ///< 未知结果类型
    DETECTION,   ///< 检测结果类型
    CLASSIFICATION,  ///< 分类结果类型
    SEGMENTATION,  ///< 分割结果类型
    POSE,          ///< 关键点检测结果类型
    OCR,           ///< 文本识别结果类型
    TRACK,         ///< 跟踪结果类型
    EMBEDDING,     ///< 嵌入结果类型
    LLM            ///< 大语言模型结果类型
};

/// @brief 检测框结构体
struct DetectionBox {
    int class_id;   ///< 检测框的类别ID
    float score;   ///< 检测框的置信度

    float x1;       ///< 检测框左上角x坐标
    float y1;       ///< 检测框左上角y坐标
    float x2;       ///< 检测框右下角x坐标
    float y2;       ///< 检测框右下角y坐标
};

/// @brief 检测结果结构体
struct DetectionResult {
    std::vector<DetectionBox> boxes; ///< 检测框列表
};

/// @brief 分类结果结构体
struct ClassificationResult {
    int class_id;   ///< 分类结果的类别ID
    float score;   ///< 分类结果的置信度
};

/// @brief 分割掩码结构体
struct SegmentationMask {
    int width;   ///< 分割掩码的宽度
    int height;   ///< 分割掩码的高度

    std::vector<uint8_t> mask; ///< 分割掩码数据
};

/// @brief 分割结果结构体
struct SegmentationResult {
    std::vector<SegmentationMask> masks; ///< 分割掩码列表
};


/// @brief 关键点结构体
struct KeyPoint {
    float x;       ///< 关键点x坐标
    float y;       ///< 关键点y坐标
    float score;    ///< 关键点的置信度
};

/// @brief 姿态检测结果结构体
struct PoseResult {
    std::vector<KeyPoint> keypoints; ///< 关键点列表
};
#endif
///////////////////////////////////////////////////////////////////
struct Rectangle {
    float x;       ///< 矩形左上角x坐标
    float y;       ///< 矩形左上角y坐标
    float width;   ///< 矩形的宽度
    float height;  ///< 矩形的高度
};

/// @brief 关键点结构体
struct KeyPoint {
    float x;       ///< 关键点x坐标
    float y;       ///< 关键点y坐标
    float score;    ///< 关键点的置信度
};

/// @brief 对象元数据结构体（第一阶段：目标检测）
struct ObjectMeta {
    int id{-1}; ///< 对象ID
    int class_id{-1}; ///< 对象的类别ID
    float score{0.0f}; ///< 对象的置信度
    Rectangle rect; ///< 对象的矩形区域
};

/// @brief 姿态结果结构体
struct PoseMeta {    
    std::vector<KeyPoint> keypoints; ///< 关键点名称列表
};

/// @brief 分割掩码元数据结构体
struct MaskMeta {
    int width;   ///< 分割掩码的宽度
    int height;   ///< 分割掩码的高度

    std::vector<uint8_t> mask; ///< 分割掩码数据
};

/// @brief 分类元数据结构体（第二阶段：分类）
struct ClassificationMeta {
    int class_id{-1};   ///< 分类结果的类别ID；-1 表示尚未赋值。
    float score{0.0f};  ///< 分类结果的置信度或归一化概率。
};

/// @brief 对象结果结构体（第二阶段：分类）
struct ObjectResult {
    ObjectMeta object;  ///< 对象元数据
    std::optional<PoseMeta> pose; ///< 姿态结果
    std::optional<MaskMeta> mask;   ///< 分割掩码结果
    std::optional<ClassificationMeta> cls; ///< 分类结果
};

/// @brief 一次推理请求的阶段耗时快照，单位为微秒。
///
/// queue_us 由 scheduler 填写，其余字段由 Session 在同步或 provider 异步完成路径
/// 中填写，便于基准和上层诊断区分预处理、provider 推理、后处理与端到端总耗时。
/// 未执行到的阶段保持 0；这比使用一个“是否成功”的布尔值更适合失败请求的部分
/// 耗时诊断。
struct InferenceTiming {
    uint64_t queue_us{0};       ///< scheduler 入队到 worker 派发的等待耗时。
    uint64_t preprocess_us{0};  ///< MediaFrame 到输入 TensorFrame 的耗时。
    uint64_t infer_us{0};       ///< provider 绑定、执行和输出收集耗时。
    uint64_t postprocess_us{0}; ///< 输出 TensorFrame 到业务结果的耗时。
    uint64_t total_us{0};       ///< Session 端到端耗时，包括已执行的全部阶段。
};

/// @brief 帧结果结构体（汇总）
struct FrameResult {
    /// @brief 帧级结构化状态；空检测仍应使用 kOk，而不是依赖 objects 是否为空。
    InferenceStatus status;
    uint64_t stream_id{0};   ///< 上游流标识；0 表示调用方未提供。
    uint64_t frame_id{0};    ///< 上游帧标识；0 表示调用方未提供。
    uint64_t request_id{0};  ///< 本次推理请求标识。
    std::string model_name;  ///< 实际执行模型名称。
    uint64_t model_generation{0}; ///< 实际执行模型的 Session 装配代次。
    uint64_t pts{0};         ///< 时间戳，单位为微秒。
    InferenceTiming timing;  ///< 同步/异步执行阶段耗时；失败请求也保留已完成阶段。
    std::vector<ObjectResult> objects;  ///< 对象结果列表
    /// @brief 帧级分类 Top-K 结果，按 score 从高到低排列。
    ///
    /// 图像分类没有源图中的检测框，因此不能借用 `objects` 或填充一个空 Rectangle。
    /// `ObjectResult::cls` 只保留给“先检测、再对某个检测框分类”的组合模型；纯分类
    /// adapter 将结果写入本字段，使上层可以明确区分帧语义和对象语义。
    std::vector<ClassificationMeta> classifications;

    /// @brief 用提交上下文覆盖结果身份字段。
    ///
    /// 后处理器只关心业务对象，可能返回一个新的 FrameResult 并丢失请求元数据；
    /// Session/scheduler 应在所有成功、失败和取消终态调用本方法，确保上层无需
    /// 根据 callback 所在的线程或错误阶段额外拼接 stream/frame/request 信息。
    void ApplyContext(const InferContext& context) {
        stream_id = context.stream_id;
        frame_id = context.frame_id;
        request_id = context.request_id;
        model_name = context.model_name;
        model_generation = context.model_generation;
        pts = context.pts;
    }
};

/// @brief 检测结果结构体
struct DetectionBox {
    int class_id; ///< 检测框的类别ID
    float confidence; ///< 检测框的置信度
    float x1; ///< 检测框左上角x坐标
    float y1; ///< 检测框左上角y坐标
    float x2; ///< 检测框右下角x坐标
    float y2; ///< 检测框右下角y坐标
};
