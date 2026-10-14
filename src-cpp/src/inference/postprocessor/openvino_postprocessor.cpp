#include "postprocessor/openvino_postprocessor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "inference/inference_logger.h"

namespace {

constexpr float kEpsilon = 1e-6f;

const TensorPlane* FindFirstFloatTensor(const TensorFrame& output,
                                       const std::string& tensor_name) {
    if (!tensor_name.empty()) {
        const auto* tensor = output.FindTensor(tensor_name);
        if (tensor && tensor->type == TensorType::FP32 && tensor->data && tensor->bytes > 0) {
            return tensor;
        }
        return nullptr;
    }

    for (const auto& [_, tensor] : output.tensors_) {
        if (tensor && tensor->type == TensorType::FP32 && tensor->data && tensor->bytes > 0) {
            return tensor.get();
        }
    }
    return nullptr;
}

bool ResolveYoloShape(const TensorShape& shape,
                      const YoloOutputSchema& schema,
                      size_t& count,
                      size_t& attrs,
                      bool& transposed) {
    const auto read_dimension = [](int64_t dimension, size_t& value) {
        if (dimension <= 0) {
            return false;
        }

        // shape 使用 int64_t 表达，转换到 size_t 前必须处理跨平台宽度差异。
        const auto unsigned_dimension = static_cast<std::uintmax_t>(dimension);
        if (unsigned_dimension > std::numeric_limits<size_t>::max()) {
            return false;
        }
        value = static_cast<size_t>(unsigned_dimension);
        return true;
    };

    const auto assign_layout = [&](size_t first, size_t second) {
        switch (schema.layout) {
        case YoloOutputLayout::kCandidatesAttributes:
            count = first;
            attrs = second;
            transposed = false;
            return;
        case YoloOutputLayout::kAttributesCandidates:
            attrs = first;
            count = second;
            transposed = true;
            return;
        case YoloOutputLayout::kAuto:
            // 兼容旧模型：较小且不超过 256 的维度视为 attrs。
            count = first;
            attrs = second;
            if (count <= 256 && attrs > count) {
                std::swap(count, attrs);
                transposed = true;
            } else {
                transposed = false;
            }
            return;
        }
    };

    if (shape.dims.size() == 3) {
        size_t batch = 0;
        size_t first = 0;
        size_t second = 0;
        if (!read_dimension(shape.dims[0], batch) ||
            !read_dimension(shape.dims[1], first) ||
            !read_dimension(shape.dims[2], second)) {
            return false;
        }
        // 显式 schema 目前只承诺单 batch，避免把多 batch 输出静默当成一帧。
        if (schema.layout != YoloOutputLayout::kAuto && batch != 1) {
            return false;
        }
        assign_layout(first, second);
        return true;
    }

    if (shape.dims.size() == 2) {
        size_t first = 0;
        size_t second = 0;
        if (!read_dimension(shape.dims[0], first) || !read_dimension(shape.dims[1], second)) {
            return false;
        }
        assign_layout(first, second);
        return true;
    }

    return false;
}

/// @brief 以 checked 乘法计算字节数，避免 shape 乘法回绕成小数值。
bool CheckedMultiply(size_t lhs, size_t rhs, size_t& result) {
    if (rhs != 0 && lhs > std::numeric_limits<size_t>::max() / rhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

float YoloValue(const float* data, size_t index, size_t attr, size_t count, size_t attrs, bool transposed) {
    return transposed ? data[attr * count + index] : data[index * attrs + attr];
}

float Area(const DetectionBox& box) {
    return std::max(0.0f, box.x2 - box.x1) * std::max(0.0f, box.y2 - box.y1);
}

float IoU(const DetectionBox& a, const DetectionBox& b) {
    const auto ix1 = std::max(a.x1, b.x1);
    const auto iy1 = std::max(a.y1, b.y1);
    const auto ix2 = std::min(a.x2, b.x2);
    const auto iy2 = std::min(a.y2, b.y2);
    const auto iw = std::max(0.0f, ix2 - ix1);
    const auto ih = std::max(0.0f, iy2 - iy1);
    const auto inter = iw * ih;
    const auto union_area = Area(a) + Area(b) - inter;

    if (union_area <= std::numeric_limits<float>::epsilon()) {
        return 0.0f;
    }
    return inter / union_area;
}

/// @brief 根据 schema 计算单个候选所需属性数，避免把 fused NMS 的 class_id
/// 当作“第二个类别分数”而发生越界或错误解码。
size_t ExpectedAttributeCount(const YoloOutputSchema& schema) {
    if (schema.encoding == YoloOutputEncoding::kNmsFused) {
        return 6;
    }
    return static_cast<size_t>(schema.class_count) +
           (schema.has_objectness ? size_t{5} : size_t{4});
}

/// @brief 将融合 NMS 的可选 `num_detections` 输出转换为安全的候选数量。
///
/// OpenVINO 导出会把该端口表示为 FP32、INT32 或 INT64，且常见形状包括标量、`[1]`
/// 和 `[1, 1]`。这里按“静态 shape 恰好一个元素”处理，并拒绝 NaN、小数、负值和
/// buffer 不足；主检测 tensor 的 shape 仍是容量上限，不能因计数端口而绕过边界检查。
bool TryReadValidDetectionCount(const TensorPlane& tensor,
                                size_t& count,
                                std::string& error_message) {
    count = 0;
    size_t element_count = 0;
    size_t element_bytes = 0;
    if (!tensor.shape.TryElementCount(element_count) || element_count != 1 ||
        !TryGetTensorTypeByteSize(tensor.type, element_bytes) || tensor.data == nullptr ||
        tensor.bytes < element_bytes) {
        error_message = "YOLO valid detections tensor must contain one complete scalar";
        return false;
    }

    const auto assign_signed_count = [&count, &error_message](std::int64_t value) {
        if (value < 0 || static_cast<std::uintmax_t>(value) >
                             std::numeric_limits<size_t>::max()) {
            error_message = "YOLO valid detections value is out of range";
            return false;
        }
        count = static_cast<size_t>(value);
        return true;
    };

    switch (tensor.type) {
    case TensorType::FP32: {
        const auto value = *tensor.Data<float>();
        const auto rounded_value = std::round(value);
        // 所有实际候选数都远小于 int64_t 上限。严格限制到该范围可避免将巨大
        // 浮点值直接转换为整数时发生未定义或实现相关的截断。
        if (!std::isfinite(value) || value < 0.0f ||
            std::abs(value - rounded_value) > kEpsilon ||
            rounded_value >= static_cast<float>(std::numeric_limits<std::int64_t>::max())) {
            error_message = "YOLO valid detections value must be a non-negative integer";
            return false;
        }
        return assign_signed_count(static_cast<std::int64_t>(rounded_value));
    }
    case TensorType::INT32:
        return assign_signed_count(static_cast<std::int64_t>(*tensor.Data<std::int32_t>()));
    case TensorType::INT64:
        return assign_signed_count(*tensor.Data<std::int64_t>());
    default:
        error_message = "YOLO valid detections tensor dtype is unsupported";
        return false;
    }
}

/// @brief 将正的 shape 维度安全转换为 size_t，供分离 NMS 的端口校验复用。
bool TryReadPositiveSize(int64_t dimension, size_t& value) {
    if (dimension <= 0 ||
        static_cast<std::uintmax_t>(dimension) > std::numeric_limits<size_t>::max()) {
        return false;
    }
    value = static_cast<size_t>(dimension);
    return true;
}

/// @brief 校验一个 tensor 的完整 shape 所需字节均已提供，且不会在计算中溢出。
///
/// 分离 NMS 的三个端口分别来自 provider。即使候选数已经由另一个端口校验，也不能
/// 假设当前 tensor 的 buffer 完整；每个端口都必须独立验证，才能保证后续逐元素读取
/// 不依赖 provider 的隐式内存正确性。
bool HasCompleteTensorBuffer(const TensorPlane& tensor,
                             size_t expected_elements,
                             std::string& error_message) {
    size_t shape_elements = 0;
    size_t element_bytes = 0;
    size_t required_bytes = 0;
    if (!tensor.shape.TryElementCount(shape_elements) || shape_elements != expected_elements ||
        !TryGetTensorTypeByteSize(tensor.type, element_bytes) ||
        !CheckedMultiply(expected_elements, element_bytes, required_bytes)) {
        error_message = "YOLO split NMS tensor shape or byte size is invalid";
        return false;
    }
    if (tensor.data == nullptr || tensor.bytes < required_bytes) {
        error_message = "YOLO split NMS tensor buffer is smaller than its shape requires";
        return false;
    }
    return true;
}

/// @brief 解析分离 NMS boxes 端口的候选容量。
///
/// 该 profile 只接受行连续的 xyxy 记录 `[N,4]` 或 `[1,N,4]`。不接受转置布局，
/// 因为 boxes/scores/classes 分离导出本身已经需要显式配置，继续猜测会让坐标列
/// 与候选索引错位。
bool TryResolveSplitBoxes(const TensorPlane& tensor,
                           size_t& candidate_count,
                           std::string& error_message) {
    candidate_count = 0;
    if (tensor.type != TensorType::FP32) {
        error_message = "YOLO split NMS boxes tensor dtype must be FP32";
        return false;
    }

    const auto rank = tensor.shape.dims.size();
    if (rank != 2 && rank != 3) {
        error_message = "YOLO split NMS boxes tensor rank must be 2 or 3";
        return false;
    }
    if (rank == 3 && tensor.shape.dims[0] != 1) {
        error_message = "YOLO split NMS boxes tensor batch must be one";
        return false;
    }

    const auto count_dimension = rank == 3 ? tensor.shape.dims[1] : tensor.shape.dims[0];
    const auto attrs_dimension = rank == 3 ? tensor.shape.dims[2] : tensor.shape.dims[1];
    size_t attrs = 0;
    if (!TryReadPositiveSize(count_dimension, candidate_count) ||
        !TryReadPositiveSize(attrs_dimension, attrs) || attrs != 4) {
        error_message = "YOLO split NMS boxes tensor must use [N,4] or [1,N,4]";
        return false;
    }

    size_t expected_elements = 0;
    if (!CheckedMultiply(candidate_count, attrs, expected_elements) ||
        !HasCompleteTensorBuffer(tensor, expected_elements, error_message)) {
        return false;
    }
    return true;
}

/// @brief 校验 scores/classes 端口与 boxes 端口逐候选一一对应。
bool HasSplitCandidateVector(const TensorPlane& tensor,
                             size_t candidate_count,
                             std::string& error_message) {
    const auto rank = tensor.shape.dims.size();
    if (rank == 1) {
        size_t count = 0;
        if (!TryReadPositiveSize(tensor.shape.dims[0], count) || count != candidate_count) {
            error_message = "YOLO split NMS vector count does not match boxes";
            return false;
        }
    } else if (rank == 2) {
        size_t count = 0;
        if (tensor.shape.dims[0] != 1 ||
            !TryReadPositiveSize(tensor.shape.dims[1], count) || count != candidate_count) {
            error_message = "YOLO split NMS vector must use [N] or [1,N]";
            return false;
        }
    } else {
        error_message = "YOLO split NMS vector rank must be 1 or 2";
        return false;
    }

    return HasCompleteTensorBuffer(tensor, candidate_count, error_message);
}

/// @brief 从 classes 端口读取并验证一个离散 class_id。
///
/// 与融合 `[N,6]` profile 一样，FP32 class_id 必须是精确整数；INT32/INT64 可避免
/// 浮点转换但仍需验证其在模型 class_count 范围内。单条记录无效时返回 false，由
/// decoder 跳过该记录而不是让其它正常候选全部失败。
bool TryReadSplitClassId(const TensorPlane& tensor,
                         size_t index,
                         int class_count,
                         int& class_id) {
    const auto assign = [class_count, &class_id](std::int64_t value) {
        if (value < 0 || value >= class_count) {
            return false;
        }
        class_id = static_cast<int>(value);
        return true;
    };

    switch (tensor.type) {
    case TensorType::FP32: {
        const auto value = tensor.Data<float>()[index];
        const auto rounded_value = std::round(value);
        if (!std::isfinite(value) || std::abs(value - rounded_value) > kEpsilon ||
            rounded_value <= -1.0f ||
            rounded_value >= static_cast<float>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        return assign(static_cast<std::int64_t>(rounded_value));
    }
    case TensorType::INT32:
        return assign(static_cast<std::int64_t>(tensor.Data<std::int32_t>()[index]));
    case TensorType::INT64:
        return assign(tensor.Data<std::int64_t>()[index]);
    default:
        return false;
    }
}

}  // namespace

bool YoloPostprocessor::Initialize(const OpenVinoYoloPostprocessorConfig& config) {
    if (config.input_width <= 0 || config.input_height <= 0) {
        LOG_MAIN_ERROR_AT("YOLO postprocessor input size is invalid: {}x{}",
                          config.input_width,
                          config.input_height);
        return false;
    }
    if (config.output_schema.class_count <= 0) {
        LOG_MAIN_ERROR_AT("YOLO output schema class count is invalid: {}",
                          config.output_schema.class_count);
        return false;
    }

    conf_threshold_ = config.conf_threshold;
    nms_threshold_ = config.nms_threshold;
    output_schema_ = config.output_schema;
    tensor_meta_.input_width = config.input_width;
    tensor_meta_.input_height = config.input_height;
    tensor_meta_.src_width = config.input_width;
    tensor_meta_.src_height = config.input_height;
    tensor_meta_.letterbox = {};
    return true;
}

FrameResult YoloPostprocessor::Process(const TensorFrame& output) {
    FrameResult result;
    result.frame_id = 0;
    result.pts = 0;

    const auto& meta = output.tensor_meta_;

    // 从 TensorFrame 中复制完整的元信息。
    tensor_meta_ = meta;
    // 如果原始尺寸未设置，默认回退到模型输入尺寸。
    if (tensor_meta_.src_width <= 0) {
        tensor_meta_.src_width = tensor_meta_.input_width;
    }
    if (tensor_meta_.src_height <= 0) {
        tensor_meta_.src_height = tensor_meta_.input_height;
    }

    InferenceStatus decode_status;
    std::vector<DetectionBox> detections;
    if (output_schema_.encoding == YoloOutputEncoding::kNmsSplit) {
        // 分离 NMS 必须同时读取三个具名端口，不能退化为“取第一个 FP32 tensor”。
        detections = decodeSplitNms(output, decode_status);
    } else {
        const auto* tensor = FindFirstFloatTensor(output, output_schema_.tensor_name);
        if (!tensor) {
            const auto message = output_schema_.tensor_name.empty()
                                     ? "YOLO postprocess did not find FP32 output tensor"
                                     : "Configured YOLO output tensor is missing or not FP32";
            LOG_MAIN_WARN_AT("{}", message);
            result.status = InferenceStatus::Failure(
                InferenceStatusCode::kPostprocessFailed,
                InferenceStage::kPostprocess,
                message,
                output_schema_.tensor_name);
            return result;
        }

        std::optional<size_t> candidate_limit;
        if (!output_schema_.valid_detections_tensor_name.empty()) {
            const auto* valid_count_tensor =
                output.FindTensor(output_schema_.valid_detections_tensor_name);
            std::string count_error;
            size_t valid_count = 0;
            if (!valid_count_tensor ||
                !TryReadValidDetectionCount(*valid_count_tensor, valid_count, count_error)) {
                if (count_error.empty()) {
                    count_error = "YOLO valid detections tensor is missing";
                }
                LOG_MAIN_WARN_AT("{}: {}",
                                 count_error,
                                 output_schema_.valid_detections_tensor_name);
                result.status = InferenceStatus::Failure(
                    InferenceStatusCode::kPostprocessFailed,
                    InferenceStage::kPostprocess,
                    count_error,
                    output_schema_.valid_detections_tensor_name);
                return result;
            }
            candidate_limit = valid_count;
        }
        detections = decode(*tensor, decode_status, candidate_limit);
    }
    if (!decode_status.Ok()) {
        result.status = std::move(decode_status);
        return result;
    }
    // fused NMS 输出已经由模型图完成候选筛选；再次按本地阈值抑制会改变模型
    // 导出时的语义。原始 class-score 输出才需要走既有 NMS。
    if (output_schema_.encoding == YoloOutputEncoding::kClassScores) {
        detections = nms(detections);
    }

    result.objects.reserve(detections.size());
    for (const auto& detection : detections) {
        Rectangle model_box;
        model_box.x = detection.x1;
        model_box.y = detection.y1;
        model_box.width = detection.x2 - detection.x1;
        model_box.height = detection.y2 - detection.y1;

        ObjectResult object;
        object.object.class_id = detection.class_id;
        object.object.score = detection.confidence;
        object.object.rect = restoreBox(model_box);
        result.objects.push_back(std::move(object));
    }

    result.status = InferenceStatus::Success();
    return result;
}

std::vector<DetectionBox> YoloPostprocessor::decode(const TensorPlane& tensor,
                                                    InferenceStatus& status,
                                                    std::optional<size_t> candidate_limit) {
    std::vector<DetectionBox> detections;
    status = InferenceStatus::Success();

    size_t count = 0;
    size_t attrs = 0;
    bool transposed = false;
    if (!ResolveYoloShape(tensor.shape, output_schema_, count, attrs, transposed)) {
        LOG_MAIN_WARN_AT("Unsupported YOLO output shape");
        status = InferenceStatus::Failure(InferenceStatusCode::kPostprocessFailed,
                                          InferenceStage::kPostprocess,
                                          "YOLO output shape is unsupported");
        return detections;
    }

    const auto expected_attrs = ExpectedAttributeCount(output_schema_);
    if (attrs != expected_attrs) {
        status = InferenceStatus::Failure(
            InferenceStatusCode::kPostprocessFailed,
            InferenceStage::kPostprocess,
            "YOLO output attrs do not match configured schema",
            "expected_attrs=" + std::to_string(expected_attrs) +
                ", actual_attrs=" + std::to_string(attrs));
        return detections;
    }

    // 多输出融合 NMS 的有效计数只能缩小主输出 shape 声明的候选范围。大于容量
    // 说明两个端口来自不同模型版本、不同 batch 或被错误绑定，必须失败而不能截断。
    if (candidate_limit && *candidate_limit > count) {
        status = InferenceStatus::Failure(
            InferenceStatusCode::kPostprocessFailed,
            InferenceStage::kPostprocess,
            "YOLO valid detections value exceeds output candidate capacity",
            "valid_detections=" + std::to_string(*candidate_limit) +
                ", candidate_capacity=" + std::to_string(count));
        return detections;
    }
    const auto decoded_count = candidate_limit.value_or(count);

    // 即使 shape 的维度都为正，count * attrs 仍可能在 size_t 中回绕；
    // 在任何指针寻址前先计算完整的元素和字节需求，确保 decode 不会越界读取。
    size_t shape_elements = 0;
    size_t required_elements = 0;
    size_t element_bytes = 0;
    size_t required_bytes = 0;
    if (!tensor.shape.TryElementCount(shape_elements) ||
        !CheckedMultiply(count, attrs, required_elements) ||
        !TryGetTensorTypeByteSize(tensor.type, element_bytes) ||
        !CheckedMultiply(required_elements, element_bytes, required_bytes)) {
        status = InferenceStatus::Failure(
            InferenceStatusCode::kPostprocessFailed,
            InferenceStage::kPostprocess,
            "YOLO output tensor size calculation overflowed");
        return detections;
    }

    // 当前 decoder 只消费单 batch 的 count * attrs 数据；shape_elements 仍需
    // 成功计算，以便拒绝带有溢出维度的伪造 shape，而不是只检查局部两个维度。
    (void)shape_elements;
    if (tensor.data == nullptr || tensor.bytes < required_bytes) {
        status = InferenceStatus::Failure(
            InferenceStatusCode::kPostprocessFailed,
            InferenceStage::kPostprocess,
            "YOLO output tensor buffer is smaller than its shape requires",
            "required_bytes=" + std::to_string(required_bytes) +
                ", actual_bytes=" + std::to_string(tensor.bytes));
        return detections;
    }

    const auto* data = tensor.Data<float>();
    detections.reserve(decoded_count);
    if (output_schema_.encoding == YoloOutputEncoding::kNmsFused) {
        for (size_t i = 0; i < decoded_count; ++i) {
            const auto x1 = YoloValue(data, i, 0, count, attrs, transposed);
            const auto y1 = YoloValue(data, i, 1, count, attrs, transposed);
            const auto x2 = YoloValue(data, i, 2, count, attrs, transposed);
            const auto y2 = YoloValue(data, i, 3, count, attrs, transposed);
            const auto score = YoloValue(data, i, 4, count, attrs, transposed);
            const auto raw_class_id = YoloValue(data, i, 5, count, attrs, transposed);
            const auto rounded_class_id = std::round(raw_class_id);

            // class_id 是离散字段；拒绝 NaN、非整数和超出 schema 的值，避免模型
            // 错误把 score/坐标列错位时被静默转换成一个看似有效的类别。
            if (!std::isfinite(score) || !std::isfinite(raw_class_id) ||
                std::abs(raw_class_id - rounded_class_id) > kEpsilon ||
                rounded_class_id < 0.0f ||
                rounded_class_id >= static_cast<float>(output_schema_.class_count) ||
                score < conf_threshold_) {
                continue;
            }

            DetectionBox box;
            box.class_id = static_cast<int>(rounded_class_id);
            box.confidence = score;
            box.x1 = std::clamp(x1, 0.0f, static_cast<float>(tensor_meta_.input_width));
            box.y1 = std::clamp(y1, 0.0f, static_cast<float>(tensor_meta_.input_height));
            box.x2 = std::clamp(x2, 0.0f, static_cast<float>(tensor_meta_.input_width));
            box.y2 = std::clamp(y2, 0.0f, static_cast<float>(tensor_meta_.input_height));
            if (Area(box) > 0.0f) {
                detections.push_back(box);
            }
        }
        return detections;
    }

    const auto has_objectness = output_schema_.has_objectness;
    const auto class_begin = has_objectness ? size_t{5} : size_t{4};
    if (class_begin >= attrs) {
        return detections;
    }

    for (size_t i = 0; i < decoded_count; ++i) {
        const auto cx = YoloValue(data, i, 0, count, attrs, transposed);
        const auto cy = YoloValue(data, i, 1, count, attrs, transposed);
        const auto w = std::abs(YoloValue(data, i, 2, count, attrs, transposed));
        const auto h = std::abs(YoloValue(data, i, 3, count, attrs, transposed));
        const auto objectness = has_objectness ? YoloValue(data, i, 4, count, attrs, transposed) : 1.0f;

        int best_class = -1;
        float best_score = 0.0f;
        for (size_t c = class_begin; c < attrs; ++c) {
            const auto class_score = YoloValue(data, i, c, count, attrs, transposed);
            if (class_score > best_score) {
                best_score = class_score;
                best_class = static_cast<int>(c - class_begin);
            }
        }

        const auto score = objectness * best_score;
        if (best_class < 0 || score < conf_threshold_) {
            continue;
        }

        DetectionBox box;
        box.class_id = best_class;
        box.confidence = score;
        box.x1 = std::clamp(cx - w * 0.5f, 0.0f, static_cast<float>(tensor_meta_.input_width));
        box.y1 = std::clamp(cy - h * 0.5f, 0.0f, static_cast<float>(tensor_meta_.input_height));
        box.x2 = std::clamp(cx + w * 0.5f, 0.0f, static_cast<float>(tensor_meta_.input_width));
        box.y2 = std::clamp(cy + h * 0.5f, 0.0f, static_cast<float>(tensor_meta_.input_height));
        if (Area(box) > 0.0f) {
            detections.push_back(box);
        }
    }

    return detections;
}

std::vector<DetectionBox> YoloPostprocessor::decodeSplitNms(const TensorFrame& output,
                                                              InferenceStatus& status) {
    std::vector<DetectionBox> detections;
    status = InferenceStatus::Success();

    const auto* boxes = output.FindTensor(output_schema_.boxes_tensor_name);
    const auto* scores = output.FindTensor(output_schema_.scores_tensor_name);
    const auto* classes = output.FindTensor(output_schema_.classes_tensor_name);
    if (!boxes || !scores || !classes) {
        status = InferenceStatus::Failure(
            InferenceStatusCode::kPostprocessFailed,
            InferenceStage::kPostprocess,
            "YOLO split NMS did not find configured boxes, scores or classes tensor");
        return detections;
    }

    std::string validation_error;
    size_t candidate_count = 0;
    if (!TryResolveSplitBoxes(*boxes, candidate_count, validation_error) ||
        scores->type != TensorType::FP32 ||
        !HasSplitCandidateVector(*scores, candidate_count, validation_error) ||
        (classes->type != TensorType::FP32 && classes->type != TensorType::INT32 &&
         classes->type != TensorType::INT64) ||
        !HasSplitCandidateVector(*classes, candidate_count, validation_error)) {
        if (validation_error.empty()) {
            validation_error = "YOLO split NMS tensor dtype is unsupported";
        }
        status = InferenceStatus::Failure(InferenceStatusCode::kPostprocessFailed,
                                          InferenceStage::kPostprocess,
                                          validation_error);
        return detections;
    }

    size_t decoded_count = candidate_count;
    if (!output_schema_.valid_detections_tensor_name.empty()) {
        const auto* valid_count_tensor =
            output.FindTensor(output_schema_.valid_detections_tensor_name);
        std::string count_error;
        if (!valid_count_tensor ||
            !TryReadValidDetectionCount(*valid_count_tensor, decoded_count, count_error)) {
            if (count_error.empty()) {
                count_error = "YOLO valid detections tensor is missing";
            }
            status = InferenceStatus::Failure(InferenceStatusCode::kPostprocessFailed,
                                              InferenceStage::kPostprocess,
                                              count_error,
                                              output_schema_.valid_detections_tensor_name);
            return detections;
        }
        if (decoded_count > candidate_count) {
            status = InferenceStatus::Failure(
                InferenceStatusCode::kPostprocessFailed,
                InferenceStage::kPostprocess,
                "YOLO valid detections value exceeds split output candidate capacity",
                "valid_detections=" + std::to_string(decoded_count) +
                    ", candidate_capacity=" + std::to_string(candidate_count));
            return detections;
        }
    }

    const auto* boxes_data = boxes->Data<float>();
    const auto* scores_data = scores->Data<float>();
    detections.reserve(decoded_count);
    for (size_t i = 0; i < decoded_count; ++i) {
        const auto x1 = boxes_data[i * 4];
        const auto y1 = boxes_data[i * 4 + 1];
        const auto x2 = boxes_data[i * 4 + 2];
        const auto y2 = boxes_data[i * 4 + 3];
        const auto score = scores_data[i];
        int class_id = -1;

        // 无效坐标、低分和非法 class_id 都只影响当前候选；这与 `[N,6]` 融合
        // profile 的逐记录过滤一致，不会因固定容量区域的一条脏数据丢弃整帧。
        if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) ||
            !std::isfinite(y2) || !std::isfinite(score) || score < conf_threshold_ ||
            !TryReadSplitClassId(*classes, i, output_schema_.class_count, class_id)) {
            continue;
        }

        DetectionBox box;
        box.class_id = class_id;
        box.confidence = score;
        box.x1 = std::clamp(x1, 0.0f, static_cast<float>(tensor_meta_.input_width));
        box.y1 = std::clamp(y1, 0.0f, static_cast<float>(tensor_meta_.input_height));
        box.x2 = std::clamp(x2, 0.0f, static_cast<float>(tensor_meta_.input_width));
        box.y2 = std::clamp(y2, 0.0f, static_cast<float>(tensor_meta_.input_height));
        if (Area(box) > 0.0f) {
            detections.push_back(box);
        }
    }
    return detections;
}

std::vector<DetectionBox> YoloPostprocessor::nms(std::vector<DetectionBox>& detections) {
    std::sort(detections.begin(),
              detections.end(),
              [](const DetectionBox& lhs, const DetectionBox& rhs) {
                  return lhs.confidence > rhs.confidence;
              });

    std::vector<DetectionBox> kept;
    std::vector<bool> removed(detections.size(), false);
    for (size_t i = 0; i < detections.size(); ++i) {
        if (removed[i]) {
            continue;
        }

        kept.push_back(detections[i]);
        for (size_t j = i + 1; j < detections.size(); ++j) {
            if (removed[j] || detections[i].class_id != detections[j].class_id) {
                continue;
            }
            if (IoU(detections[i], detections[j]) > nms_threshold_) {
                removed[j] = true;
            }
        }
    }

    return kept;
}

Rectangle YoloPostprocessor::restoreBox(const Rectangle& box) {
    const auto sx = tensor_meta_.letterbox.scale_x > kEpsilon ? tensor_meta_.letterbox.scale_x : 1.0f;
    const auto sy = tensor_meta_.letterbox.scale_y > kEpsilon ? tensor_meta_.letterbox.scale_y : 1.0f;

    const auto x1 = std::clamp((box.x - tensor_meta_.letterbox.pad_x) / sx,
                               0.0f,
                               static_cast<float>(tensor_meta_.src_width));
    const auto y1 = std::clamp((box.y - tensor_meta_.letterbox.pad_y) / sy,
                               0.0f,
                               static_cast<float>(tensor_meta_.src_height));
    const auto x2 = std::clamp((box.x + box.width - tensor_meta_.letterbox.pad_x) / sx,
                               0.0f,
                               static_cast<float>(tensor_meta_.src_width));
    const auto y2 = std::clamp((box.y + box.height - tensor_meta_.letterbox.pad_y) / sy,
                               0.0f,
                               static_cast<float>(tensor_meta_.src_height));

    Rectangle restored;
    restored.x = x1;
    restored.y = y1;
    restored.width = std::max(0.0f, x2 - x1);
    restored.height = std::max(0.0f, y2 - y1);
    return restored;
}
