#include "model/yolo_model.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "engine/i_engine.h"
#include "inference/inference_logger.h"
#include "info/result.h"
#include "postprocessor/openvino_postprocessor.h"
#include "preprocessor/openvino_preprocessor.h"

namespace {

int OptionInt(const ModelConfig& config, const std::string& key, int fallback) {
    const auto it = config.options.find(key);
    if (it == config.options.end()) {
        return fallback;
    }

    try {
        const auto value = std::stoi(it->second);
        return value > 0 ? value : fallback;
    } catch (const std::exception&) {
        return fallback;
    }
}

bool OptionBool(const ModelConfig& config, const std::string& key, bool fallback, bool& value) {
    const auto it = config.options.find(key);
    if (it == config.options.end()) {
        value = fallback;
        return true;
    }

    auto normalized = it->second;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (normalized == "1" || normalized == "true" || normalized == "yes") {
        value = true;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no") {
        value = false;
        return true;
    }
    return false;
}

/// @brief 规范化配置中的模型类型，避免 profile 判断被大小写差异意外绕过。
std::string NormalizeToken(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

/// @brief 解析 YOLO 模型输出 schema 配置。
/// @param config 模型配置
/// @param schema 输出 schema
/// @return 是否成功解析
bool ParseOutputSchema(const ModelConfig& config, YoloOutputSchema& schema) {
    schema = {};
    schema.class_count = config.class_count;

    // OpenVINO 导出的标准 YOLOv8 Detect 输出是 [1, 4 + classes, N]：没有
    // 独立 objectness，且 attributes 位于第二维。把这一稳定约定固化为 profile，
    // 让用户只需声明 model_type=yolov8；下方的显式 options 仍拥有最高优先级，
    // 用于兼容经过 NMS、转置或自定义导出的模型。
    if (const auto model_type = config.options.find("model_type");
        model_type != config.options.end()) {
        const auto normalized_type = NormalizeToken(model_type->second);
        if (normalized_type == "yolov8") {
            schema.layout = YoloOutputLayout::kAttributesCandidates;
            schema.has_objectness = false;
        } else if (normalized_type == "yolo_nms_fused") {
            // 已融合 NMS 的常见输出是 [N, 6]：xyxy、score、class_id。它不再
            // 包含逐类 score，也不应进入普通 YOLO 的第二次 NMS。
            schema.layout = YoloOutputLayout::kCandidatesAttributes;
            schema.has_objectness = false;
            schema.encoding = YoloOutputEncoding::kNmsFused;
        } else if (normalized_type == "yolo_nms_split") {
            // 部分检测图把 NMS 后的 boxes、scores、classes 分成独立端口。该 profile
            // 不从输出顺序猜测三者关系，调用方必须在下方显式配置每个 tensor 名称。
            schema.has_objectness = false;
            schema.encoding = YoloOutputEncoding::kNmsSplit;
        }
    }

    // 解析张量名称配置
    if (const auto it = config.options.find("output_tensor_name");
        it != config.options.end()) {
        schema.tensor_name = it->second;
    }
    if (const auto it = config.options.find("valid_detections_tensor_name");
        it != config.options.end()) {
        schema.valid_detections_tensor_name = it->second;
    }
    if (const auto it = config.options.find("boxes_tensor_name"); it != config.options.end()) {
        schema.boxes_tensor_name = it->second;
    }
    if (const auto it = config.options.find("scores_tensor_name"); it != config.options.end()) {
        schema.scores_tensor_name = it->second;
    }
    if (const auto it = config.options.find("classes_tensor_name"); it != config.options.end()) {
        schema.classes_tensor_name = it->second;
    }

    // 解析输出布局配置
    if (const auto it = config.options.find("output_layout"); it != config.options.end()) {
        if (it->second == "candidates_attributes") {
            schema.layout = YoloOutputLayout::kCandidatesAttributes;
        } else if (it->second == "attributes_candidates") {
            schema.layout = YoloOutputLayout::kAttributesCandidates;
        } else if (it->second == "auto") {
            schema.layout = YoloOutputLayout::kAuto;
        } else {
            return false;
        }
    }

    // 解析输出编码配置
    if (const auto it = config.options.find("output_encoding"); it != config.options.end()) {
        if (it->second == "class_scores") {
            schema.encoding = YoloOutputEncoding::kClassScores;
        } else if (it->second == "nms_fused") {
            schema.encoding = YoloOutputEncoding::kNmsFused;
        } else if (it->second == "nms_split") {
            schema.encoding = YoloOutputEncoding::kNmsSplit;
        } else {
            return false;
        }
    }

    // 这里以 profile 已经设置的值作为 fallback，防止 YOLOv8 profile 在未传该项时又被旧的 YOLOv5 默认 true 覆盖。    
    if (!OptionBool(config, "has_objectness", schema.has_objectness, schema.has_objectness)) {
        return false;
    }

    const auto has_split_tensor_name = !schema.boxes_tensor_name.empty() ||
                                       !schema.scores_tensor_name.empty() ||
                                       !schema.classes_tensor_name.empty();
    if (schema.encoding == YoloOutputEncoding::kNmsSplit) {
        // 分离 NMS 的三个端口具有不同语义，不能复用 `output_tensor_name` 或根据
        // provider 的输出顺序推断。名称必须完整且互异，避免同一 buffer 被同时当作
        // boxes、score 或 class_id 解读。
        if (!schema.tensor_name.empty() || schema.boxes_tensor_name.empty() ||
            schema.scores_tensor_name.empty() || schema.classes_tensor_name.empty() ||
            schema.boxes_tensor_name == schema.scores_tensor_name ||
            schema.boxes_tensor_name == schema.classes_tensor_name ||
            schema.scores_tensor_name == schema.classes_tensor_name ||
            (!schema.valid_detections_tensor_name.empty() &&
             (schema.valid_detections_tensor_name == schema.boxes_tensor_name ||
              schema.valid_detections_tensor_name == schema.scores_tensor_name ||
              schema.valid_detections_tensor_name == schema.classes_tensor_name))) {
            return false;
        }
        return true;
    }

    // `num_detections` 只定义了 NMS 输出的候选有效范围。原始 class-score 输出
    // 没有相同的记录语义；允许它复用该字段会让错误配置在运行期静默截断。普通
    // profile 也不能残留分离 NMS 的端口名称，防止配置拼写被默认为无效字段。
    if (has_split_tensor_name ||
        (!schema.valid_detections_tensor_name.empty() &&
         (schema.encoding != YoloOutputEncoding::kNmsFused || schema.tensor_name.empty() ||
          schema.valid_detections_tensor_name == schema.tensor_name))) {
        return false;
    }
    return true;
}

bool ParseResizeMode(const ModelConfig& config, ResizeMode& mode) {
    const auto it = config.options.find("resize_mode");
    if (it == config.options.end() || it->second == "stretch") {
        mode = ResizeMode::kStretch;
        return true;
    }
    if (it->second == "letterbox") {
        mode = ResizeMode::kLetterbox;
        return true;
    }
    if (it->second == "center_crop") {
        mode = ResizeMode::kCenterCrop;
        return true;
    }
    return false;
}

}  // namespace

bool YoloModel::Initialize(const ModelConfig& config) {
    model_meta_ = {};
    model_meta_.name = config.name;
    model_meta_.task = TaskType::DETECT;
    model_meta_.input_width = OptionInt(config, "input_width", 640);
    model_meta_.input_height = OptionInt(config, "input_height", 640);
    model_meta_.input_format = PixelFormat::kUnknown;
    model_meta_.dynamic_shape = config.dynamic_shape;
    model_meta_.class_count = config.class_count;
    // 解析输出 schema 配置
    if (!ParseOutputSchema(config, output_schema_)) {
        LOG_MAIN_ERROR_AT("YOLO output schema options are invalid");
        initialized_ = false;
        return false;
    }
    // 解析缩放模式配置
    if (!ParseResizeMode(config, resize_mode_)) {
        LOG_MAIN_ERROR_AT("YOLO resize_mode option is invalid");
        initialized_ = false;
        return false;
    }

    if (!resetPreprocessor() || !resetPostprocessor()) {
        initialized_ = false;
        return false;
    }

    initialized_ = true;
    return true;
}

InferenceStatus YoloModel::ValidateModelDesc(const TensorModelDesc& model_desc) const {
    if (!initialized_) {
        return InferenceStatus::Failure(InferenceStatusCode::kNotInitialized, InferenceStage::kLoad, "YoloModel is not initialized");
    }

    // 根据张量名称在模型输出列表中查找对应的张量描述符
    const auto find_output = [&model_desc](const std::string& name) -> const TensorDesc* {
        for (const auto& candidate : model_desc.outputs) {
            if (candidate.name == name) {
                return &candidate;
            }
        }
        return nullptr;
    };

    const auto validate_valid_count = [&find_output, this]() -> InferenceStatus {
        if (output_schema_.valid_detections_tensor_name.empty()) {
            return InferenceStatus::Success();
        }

        // `num_detections` 是 NMS 的独立计数端口。加载期确认它是静态单元素且 dtype 可读取，避免首帧才把 padding 记录当成真实检测。
        const auto* valid_count = find_output(output_schema_.valid_detections_tensor_name);
        if (!valid_count) {
            return InferenceStatus::Failure(InferenceStatusCode::kModelIncompatible, InferenceStage::kLoad,
                "YOLO valid detections tensor is missing", output_schema_.valid_detections_tensor_name);
        }
        size_t valid_count_elements = 0;
        if (valid_count->dynamic_shape || !valid_count->shape.TryElementCount(valid_count_elements) || valid_count_elements != 1) {
            return InferenceStatus::Failure(InferenceStatusCode::kModelIncompatible, InferenceStage::kLoad,
                "YOLO valid detections tensor must be a static scalar", valid_count->name);
        }
        if (valid_count->type != TensorType::FP32 && valid_count->type != TensorType::INT32 && valid_count->type != TensorType::INT64) {
            return InferenceStatus::Failure(InferenceStatusCode::kModelIncompatible, InferenceStage::kLoad,
                "YOLO valid detections tensor dtype must be FP32, INT32 or INT64", valid_count->name);
        }
        return InferenceStatus::Success();
    };

    // 验证 NMS 分离模式的张量描述符
    if (output_schema_.encoding == YoloOutputEncoding::kNmsSplit) {
        const auto* boxes = find_output(output_schema_.boxes_tensor_name);
        const auto* scores = find_output(output_schema_.scores_tensor_name);
        const auto* classes = find_output(output_schema_.classes_tensor_name);
        if (!boxes || !scores || !classes) {
            return InferenceStatus::Failure(InferenceStatusCode::kModelIncompatible, InferenceStage::kLoad,
                "YOLO split NMS requires configured boxes, scores and classes tensors");
        }

        // boxes 严格采用 xyxy 行记录：`[N, 4]` 或 batch=1 的 `[1, N, 4]`。
        // 其它布局必须通过新的 profile 明确支持，不能在此处自动转置。
        size_t boxes_elements = 0;
        size_t candidate_count = 0;
        size_t box_attrs = 0;
        const auto boxes_rank = boxes->shape.dims.size();
        if (boxes->type != TensorType::FP32 || boxes->dynamic_shape ||
            !boxes->shape.TryElementCount(boxes_elements) ||
            (boxes_rank != 2 && boxes_rank != 3) ||
            (boxes_rank == 3 && boxes->shape.dims[0] != 1)) {
            return InferenceStatus::Failure(
                InferenceStatusCode::kModelIncompatible,
                InferenceStage::kLoad,
                "YOLO split NMS boxes tensor must be static FP32 [N,4] or [1,N,4]",
                boxes->name);
        }
        const auto boxes_count_dim = boxes_rank == 3 ? boxes->shape.dims[1] : boxes->shape.dims[0];
        const auto boxes_attrs_dim = boxes_rank == 3 ? boxes->shape.dims[2] : boxes->shape.dims[1];
        const auto to_size = [](int64_t dimension, size_t& value) {
            if (dimension <= 0 ||
                static_cast<std::uintmax_t>(dimension) > std::numeric_limits<size_t>::max()) {
                return false;
            }
            value = static_cast<size_t>(dimension);
            return true;
        };
        if (!to_size(boxes_count_dim, candidate_count) || !to_size(boxes_attrs_dim, box_attrs) ||
            box_attrs != 4) {
            return InferenceStatus::Failure(
                InferenceStatusCode::kModelIncompatible,
                InferenceStage::kLoad,
                "YOLO split NMS boxes tensor must contain xyxy records",
                boxes->name);
        }

        // scores/classes 都是一条候选一个值的向量：允许省略 batch 的 `[N]`，
        // 或明确 batch=1 的 `[1,N]`；拒绝 `[N,1]`，避免不知情地支持转置约定。
        const auto is_candidate_vector = [candidate_count](const TensorDesc& tensor) {
            size_t element_count = 0;
            if (tensor.dynamic_shape || !tensor.shape.TryElementCount(element_count) ||
                element_count != candidate_count) {
                return false;
            }
            if (tensor.shape.dims.size() == 1) {
                return tensor.shape.dims[0] > 0 &&
                       static_cast<size_t>(tensor.shape.dims[0]) == candidate_count;
            }
            return tensor.shape.dims.size() == 2 && tensor.shape.dims[0] == 1 &&
                   tensor.shape.dims[1] > 0 &&
                   static_cast<size_t>(tensor.shape.dims[1]) == candidate_count;
        };
        if (scores->type != TensorType::FP32 || !is_candidate_vector(*scores)) {
            return InferenceStatus::Failure(
                InferenceStatusCode::kModelIncompatible,
                InferenceStage::kLoad,
                "YOLO split NMS scores tensor must be static FP32 [N] or [1,N]",
                scores->name);
        }
        if ((classes->type != TensorType::FP32 && classes->type != TensorType::INT32 &&
             classes->type != TensorType::INT64) ||
            !is_candidate_vector(*classes)) {
            return InferenceStatus::Failure(
                InferenceStatusCode::kModelIncompatible,
                InferenceStage::kLoad,
                "YOLO split NMS classes tensor must be FP32, INT32 or INT64 [N] or [1,N]",
                classes->name);
        }
        return validate_valid_count();
    }

    const TensorDesc* output = nullptr;
    if (!output_schema_.tensor_name.empty()) {
        output = find_output(output_schema_.tensor_name);
    } else if (model_desc.outputs.size() == 1) {
        output = &model_desc.outputs.front();
    }

    if (!output) {
        return InferenceStatus::Failure(
            InferenceStatusCode::kModelIncompatible,
            InferenceStage::kLoad,
            "YOLO output tensor does not match configured name or output count",
            output_schema_.tensor_name);
    }
    if (output->type != TensorType::FP32) {
        return InferenceStatus::Failure(InferenceStatusCode::kModelIncompatible,
                                        InferenceStage::kLoad,
                                        "YOLO output tensor dtype must be FP32",
                                        output->name);
    }
    size_t shape_element_count = 0;
    if (output->dynamic_shape || !output->shape.TryElementCount(shape_element_count)) {
        return InferenceStatus::Failure(InferenceStatusCode::kModelIncompatible,
                                        InferenceStage::kLoad,
                                        "YOLO output tensor shape must be static and valid",
                                        output->name);
    }

    if (output->shape.dims.size() != 2 && output->shape.dims.size() != 3) {
        return InferenceStatus::Failure(InferenceStatusCode::kModelIncompatible,
                                        InferenceStage::kLoad,
                                        "YOLO output tensor rank must be 2 or 3",
                                        output->name);
    }
    if (output->shape.dims.size() == 3 && output->shape.dims[0] != 1) {
        return InferenceStatus::Failure(InferenceStatusCode::kModelIncompatible,
                                        InferenceStage::kLoad,
                                        "YOLO output tensor batch must be one",
                                        output->name);
    }

    const auto to_size = [](int64_t dimension, size_t& value) {
        if (dimension <= 0 ||
            static_cast<std::uintmax_t>(dimension) > std::numeric_limits<size_t>::max()) {
            return false;
        }
        value = static_cast<size_t>(dimension);
        return true;
    };
    const auto first = output->shape.dims.size() == 3 ? output->shape.dims[1]
                                                       : output->shape.dims[0];
    const auto second = output->shape.dims.size() == 3 ? output->shape.dims[2]
                                                        : output->shape.dims[1];
    size_t first_size = 0;
    size_t second_size = 0;
    if (!to_size(first, first_size) || !to_size(second, second_size)) {
        return InferenceStatus::Failure(InferenceStatusCode::kModelIncompatible,
                                        InferenceStage::kLoad,
                                        "YOLO output tensor dimensions are invalid",
                                        output->name);
    }

    size_t attrs = 0;
    switch (output_schema_.layout) {
    case YoloOutputLayout::kCandidatesAttributes:
        attrs = second_size;
        break;
    case YoloOutputLayout::kAttributesCandidates:
        attrs = first_size;
        break;
    case YoloOutputLayout::kAuto:
        attrs = first_size <= 256 && second_size > first_size ? first_size : second_size;
        break;
    }

    const auto expected_attrs = output_schema_.encoding == YoloOutputEncoding::kNmsFused
                                    ? size_t{6}
                                    : static_cast<size_t>(output_schema_.class_count) +
                                          (output_schema_.has_objectness ? size_t{5} : size_t{4});
    if (attrs != expected_attrs) {
        return InferenceStatus::Failure(
            InferenceStatusCode::kModelIncompatible,
            InferenceStage::kLoad,
            "YOLO output attrs do not match configured schema",
            "expected_attrs=" + std::to_string(expected_attrs) +
                ", actual_attrs=" + std::to_string(attrs));
    }

    return validate_valid_count();
}

bool YoloModel::resetPreprocessor() {
    if (model_meta_.input_width <= 0 || model_meta_.input_height <= 0) {
        LOG_MAIN_ERROR_AT("YOLO input size is invalid: {}x{}",
                          model_meta_.input_width,
                          model_meta_.input_height);
        return false;
    }

    auto preprocessor = std::make_unique<OpenVinoYoloPreprocessor>();
    OpenVinoYoloPreprocessorConfig preprocessor_config;
    preprocessor_config.input_width = static_cast<uint32_t>(model_meta_.input_width);
    preprocessor_config.input_height = static_cast<uint32_t>(model_meta_.input_height);
    preprocessor_config.pixel_format = model_meta_.input_format;
    preprocessor_config.resize_mode = resize_mode_;

    if (!preprocessor->Initialize(preprocessor_config)) {
        return false;
    }

    preprocessor_ = std::move(preprocessor);
    return true;
}

bool YoloModel::resetPostprocessor() {
    auto postprocessor = std::make_unique<YoloPostprocessor>();
    OpenVinoYoloPostprocessorConfig postprocessor_config;
    postprocessor_config.input_width = model_meta_.input_width;
    postprocessor_config.input_height = model_meta_.input_height;
    // schema 已在 Initialize 阶段按 YOLOv5/YOLOv8 profile 和显式 options 解析；
    // 这里仅将同一份契约传给 decoder，避免加载期校验与首帧 decode 使用不同语义。
    postprocessor_config.output_schema = output_schema_;

    if (!postprocessor->Initialize(postprocessor_config)) {
        return false;
    }

    postprocessor_ = std::move(postprocessor);
    return true;
}

const ModelMeta& YoloModel::GetModelMeta() const {
    return model_meta_;
}

bool YoloModel::UpdateModelMeta(const ModelMeta& meta) {
    auto next = model_meta_;
    if (!meta.name.empty()) {
        next.name = meta.name;
    }
    next.task = meta.task;
    if (meta.input_width > 0) {
        next.input_width = meta.input_width;
    }
    if (meta.input_height > 0) {
        next.input_height = meta.input_height;
    }
    if (meta.input_format != PixelFormat::kUnknown) {
        next.input_format = meta.input_format;
    }
    next.dynamic_shape = meta.dynamic_shape;
    if (meta.class_count > 0) {
        next.class_count = meta.class_count;
        output_schema_.class_count = meta.class_count;
    }

    const auto size_changed = next.input_width != model_meta_.input_width ||
                              next.input_height != model_meta_.input_height ||
                              next.input_format != model_meta_.input_format;
    model_meta_ = std::move(next);

    if (initialized_ && size_changed && (!resetPreprocessor() || !resetPostprocessor())) {
        initialized_ = false;
        return false;
    }

    LOG_MAIN_INFO_AT("YOLO model meta updated: input={}x{}",
                     model_meta_.input_width,
                     model_meta_.input_height);
    return true;
}

TensorFrame YoloModel::Preprocess(const MediaFrame& frame) {
    TensorFrame output;
    PreprocessWithStatus(frame, output);
    return output;
}

InferenceStatus YoloModel::PreprocessWithStatus(const MediaFrame& frame, TensorFrame& output) {
    if (!initialized_ || !preprocessor_) {
        LOG_MAIN_ERROR_AT("YoloModel is not initialized");
        output = {};
        return InferenceStatus::Failure(InferenceStatusCode::kNotInitialized,
                                        InferenceStage::kPreprocess,
                                        "YoloModel is not initialized");
    }

    try {
        output = preprocessor_->Process(frame);
    } catch (const std::exception& e) {
        LOG_MAIN_ERROR_AT("YoloModel preprocessing failed: {}", e.what());
        output = {};
        return InferenceStatus::Failure(InferenceStatusCode::kInvalidInput,
                                        InferenceStage::kPreprocess,
                                        "YoloModel preprocessing failed",
                                        e.what());
    } catch (...) {
        LOG_MAIN_ERROR_AT("YoloModel preprocessing failed with an unknown exception");
        output = {};
        return InferenceStatus::Failure(InferenceStatusCode::kInvalidInput,
                                        InferenceStage::kPreprocess,
                                        "YoloModel preprocessing failed",
                                        "unknown preprocessing exception");
    }

    if (output.Empty()) {
        return InferenceStatus::Failure(InferenceStatusCode::kInvalidInput,
                                        InferenceStage::kPreprocess,
                                        "YoloModel preprocessing returned an empty tensor frame");
    }
    return InferenceStatus::Success();
}

FrameResult YoloModel::Postprocess(const TensorFrame& output) {
    FrameResult result;
    PostprocessWithStatus(output, result);
    return result;
}

InferenceStatus YoloModel::PostprocessWithStatus(const TensorFrame& output, FrameResult& result) {
    if (!initialized_ || !postprocessor_) {
        LOG_MAIN_ERROR_AT("YoloModel postprocessor is not initialized");
        result.status = InferenceStatus::Failure(InferenceStatusCode::kNotInitialized,
                                                 InferenceStage::kPostprocess,
                                                 "YoloModel postprocessor is not initialized");
        return result.status;
    }

    try {
        result = postprocessor_->Process(output);
    } catch (const std::exception& e) {
        LOG_MAIN_ERROR_AT("YoloModel postprocessing failed: {}", e.what());
        result = {};
        result.status = InferenceStatus::Failure(InferenceStatusCode::kPostprocessFailed,
                                                 InferenceStage::kPostprocess,
                                                 "YoloModel postprocessing failed",
                                                 e.what());
        return result.status;
    } catch (...) {
        LOG_MAIN_ERROR_AT("YoloModel postprocessing failed with an unknown exception");
        result = {};
        result.status = InferenceStatus::Failure(InferenceStatusCode::kPostprocessFailed,
                                                 InferenceStage::kPostprocess,
                                                 "YoloModel postprocessing failed",
                                                 "unknown postprocessing exception");
        return result.status;
    }

    if (result.status.code == InferenceStatusCode::kUnknown) {
        result.status = InferenceStatus::Success();
    }
    return result.status;
}
