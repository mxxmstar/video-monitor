#include "engine/openvino_engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <openvino/core/layout.hpp>
#include <openvino/core/preprocess/pre_post_process.hpp>
#include <openvino/core/preprocess/resize_algorithm.hpp>
#include <openvino/runtime/exception.hpp>

#include "inference/inference_logger.h"

namespace {

constexpr auto kDefaultDevice = "CPU";

/// @brief 将设备名称规范化为 OpenVINO 配置使用的大小写。
///
/// 配置来自 JSON、命令行或上层结构体，调用方可能传入 `cpu`/`Auto` 等形式。
/// 在进入 OpenVINO Core 之前统一转成大写，避免同一设备在校验、日志和 capability
/// 判断中出现多个字符串表示。
std::string NormalizeDevice(std::string value) {
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    // AUTO 插件在部分 OpenVINO 版本中返回 "(CPU)" 这样的展示字符串。括号只
    // 是日志包装，不属于设备标识；统一移除后，配置回读和 capability 判断可保持
    // 与显式 "CPU" 相同的语义，同时不影响未来带点号的设备 ID。
    value.erase(std::remove_if(value.begin(),
                               value.end(),
                               [](unsigned char character) {
                                   return character == '(' || character == ')' ||
                                          character == '[' || character == ']' ||
                                          std::isspace(character);
                               }),
                value.end());
    return value;
}

/// @brief 规范化模型输入布局字符串。
///
/// layout 不使用 OpenVINO 的 device 语义，但同样需要消除调用方配置中的大小写和
/// 空白差异。这里不解析任意 Layout 语法；当前图像预处理只关心能由 rank/channel
/// 位置可靠识别的 NCHW/NHWC/CHW/HWC，其他情况仍交给 provider 的原始诊断。
std::string NormalizeImageLayout(std::string value) {
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    value.erase(std::remove_if(value.begin(),
                               value.end(),
                               [](unsigned char character) {
                                   return std::isspace(character);
                               }),
                value.end());
    return value;
}

/// @brief 向 cache key 追加带长度前缀的字符串字段，避免分隔符造成拼接歧义。
///
/// 模型路径、cache_dir 等配置允许包含逗号、冒号或空格，不能简单用一个字符连接
/// 后就假设不同配置必然生成不同 key。长度前缀使键的序列化保持一一对应，同时
/// 仍可在日志/调试器中直接查看，不需要额外引入哈希库或持久化格式。
void AppendCacheKeyField(std::string& key, const std::string& value) {
    key += std::to_string(value.size());
    key += ':';
    key += value;
    key += ';';
}

/// @brief 将浮点缩放系数按位写入 cache key，避免文本格式化丢失配置差异。
std::string FloatCacheKeyField(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return std::to_string(bits);
}

/// @brief 生成单 engine compiled-model LRU 的完整配置身份。
///
/// 缓存仅复用 `compile_model` 的结果，故 key 不能包含 request_count；每次命中都会
/// 新建独立 request pool。相反，任何能改变图、预处理图或插件编译选项的字段都必须
/// 包含在内。文件身份使用规范路径、字节数和修改时间，是本阶段避免“同路径热替换”
/// 重用旧图的轻量边界；内容 checksum 与版本化将由 M6 的模型热更新协议补齐。
std::optional<std::string> BuildCompiledModelCacheKey(const EngineLoadConfig& load_config,
                                                       const EngineConfig& config) {
    std::error_code error;
    auto canonical_path = std::filesystem::weakly_canonical(config.model_path, error);
    if (error) {
        LOG_MAIN_WARN_AT("Skip OpenVINO compiled-model cache because model path cannot be canonicalized: {}",
                         error.message());
        return std::nullopt;
    }
    const auto file_size = std::filesystem::file_size(canonical_path, error);
    if (error) {
        LOG_MAIN_WARN_AT("Skip OpenVINO compiled-model cache because model file size is unavailable: {}",
                         error.message());
        return std::nullopt;
    }
    const auto write_time = std::filesystem::last_write_time(canonical_path, error);
    if (error) {
        LOG_MAIN_WARN_AT("Skip OpenVINO compiled-model cache because model modification time is unavailable: {}",
                         error.message());
        return std::nullopt;
    }

    std::string key;
    AppendCacheKeyField(key, canonical_path.generic_string());
    AppendCacheKeyField(key, std::to_string(file_size));
    AppendCacheKeyField(key, std::to_string(write_time.time_since_epoch().count()));
    AppendCacheKeyField(key, config.device);

    if (load_config.preprocess.has_value()) {
        const auto& preprocess = *load_config.preprocess;
        AppendCacheKeyField(key, "preprocess");
        AppendCacheKeyField(key, preprocess.enabled ? "1" : "0");
        AppendCacheKeyField(key, std::to_string(static_cast<uint32_t>(preprocess.input_pixel_format)));
        AppendCacheKeyField(key, std::to_string(static_cast<uint32_t>(preprocess.model_pixel_format)));
        AppendCacheKeyField(key, NormalizeImageLayout(preprocess.model_input_layout));
        AppendCacheKeyField(key, FloatCacheKeyField(preprocess.scale));
    } else {
        AppendCacheKeyField(key, "no-preprocess");
    }

    if (load_config.reshape.has_value()) {
        AppendCacheKeyField(key, "reshape");
        for (const auto dimension : load_config.reshape->input_shape.dims) {
            AppendCacheKeyField(key, std::to_string(dimension));
        }
    } else {
        AppendCacheKeyField(key, "no-reshape");
    }

    const auto runtime = load_config.runtime.value_or(OpenVinoRuntimeConfig{});
    AppendCacheKeyField(key, NormalizeDevice(runtime.performance_hint));
    AppendCacheKeyField(key, std::to_string(runtime.num_streams));
    AppendCacheKeyField(key, std::to_string(runtime.inference_num_threads));
    AppendCacheKeyField(key, runtime.enable_profiling.has_value()
                                 ? (*runtime.enable_profiling ? "1" : "0")
                                 : "unset");
    AppendCacheKeyField(key, runtime.cache_dir);
    return key;
}

bool IsSupportedDevice(const std::string& device) {
    const auto normalized = NormalizeDevice(device);
    return normalized == "CPU" || normalized == "AUTO";
}

/// @brief 把公共字符串配置转换为 OpenVINO 性能模式枚举。
///
/// 转换集中在 engine 内，确保直接使用 OpenVinoEngine 的调用方与 factory 校验的
/// 语义一致；未知值抛出异常后由 LoadModel 的统一失败路径转为 kFailed 状态。
ov::hint::PerformanceMode ToPerformanceMode(const std::string& value) {
    const auto normalized = NormalizeDevice(value);
    if (normalized == "LATENCY") {
        return ov::hint::PerformanceMode::LATENCY;
    }
    if (normalized == "THROUGHPUT") {
        return ov::hint::PerformanceMode::THROUGHPUT;
    }
    if (normalized == "CUMULATIVE_THROUGHPUT") {
        return ov::hint::PerformanceMode::CUMULATIVE_THROUGHPUT;
    }
    throw std::runtime_error("Unsupported OpenVINO performance hint: " + value);
}

/// @brief 构造 OpenVINO compile_model 的性能属性集合。
///
/// 0/空字符串不写入 AnyMap，表示完全沿用插件默认值；这样升级 OpenVINO 时，
/// 默认策略仍由 runtime 负责，平台只对调用方明确指定的字段施加约束。
ov::AnyMap BuildRuntimeProperties(const OpenVinoRuntimeConfig& config) {
    ov::AnyMap properties;
    if (!config.performance_hint.empty()) {
        properties.insert(ov::hint::performance_mode(ToPerformanceMode(config.performance_hint)));
    }

    if (config.num_streams > 0) {
        properties.insert(ov::num_streams(ov::streams::Num(config.num_streams)));
    } else if (config.num_streams == -1) {
        properties.insert(ov::num_streams(ov::streams::AUTO));
    } else if (config.num_streams == -2) {
        properties.insert(ov::num_streams(ov::streams::NUMA));
    } else if (config.num_streams != 0) {
        throw std::runtime_error("OpenVINO num_streams must be 0, -1, -2 or positive");
    }
    if (config.inference_num_threads > 0) {
        properties.insert(ov::inference_num_threads(config.inference_num_threads));
    } else if (config.inference_num_threads < 0) {
        throw std::runtime_error("OpenVINO inference_num_threads must be 0 or positive");
    }
    if (config.enable_profiling.has_value()) {
        properties.insert(ov::enable_profiling(*config.enable_profiling));
    }
    if (!config.cache_dir.empty()) {
        // cache_dir 由 OpenVINO 负责创建/写入；engine 不在加载前创建目录，避免
        // 仅验证配置就产生不可回收的副作用。部署层可先保证目录存在和可写。
        properties.insert(ov::cache_dir(config.cache_dir));
    }
    return properties;
}

std::string PerformanceModeName(ov::hint::PerformanceMode mode) {
    switch (mode) {
        case ov::hint::PerformanceMode::LATENCY:
            return "LATENCY";
        case ov::hint::PerformanceMode::THROUGHPUT:
            return "THROUGHPUT";
        case ov::hint::PerformanceMode::CUMULATIVE_THROUGHPUT:
            return "CUMULATIVE_THROUGHPUT";
        default:
            return "UNKNOWN";
    }
}

/// @brief 从已编译模型逐项读取 OpenVINO runtime 属性。
///
/// 属性查询不是所有插件/设备都保证支持，故每一项都单独 try/catch。快照的作用
/// 是可观测性，不应改变“compile_model 成功即模型可用”的主生命周期语义。
OpenVinoRuntimeSnapshot ReadRuntimeSnapshot(const ov::CompiledModel& compiled_model,
                                            const OpenVinoRuntimeConfig& requested) {
    OpenVinoRuntimeSnapshot snapshot;
    snapshot.requested = requested;

    try {
        snapshot.actual_performance_hint = PerformanceModeName(
            compiled_model.get_property<ov::hint::PerformanceMode>(ov::hint::performance_mode));
    } catch (const std::exception& e) {
        snapshot.readback_errors.push_back("performance_hint: " + std::string(e.what()));
    }

    try {
        snapshot.actual_num_streams = static_cast<int32_t>(
            compiled_model.get_property<ov::streams::Num>(ov::num_streams));
    } catch (const std::exception& e) {
        snapshot.readback_errors.push_back("num_streams: " + std::string(e.what()));
    }

    try {
        snapshot.actual_inference_num_threads =
            compiled_model.get_property<int32_t>(ov::inference_num_threads);
    } catch (const std::exception& e) {
        snapshot.readback_errors.push_back("inference_num_threads: " + std::string(e.what()));
    }

    try {
        snapshot.actual_enable_profiling =
            compiled_model.get_property<bool>(ov::enable_profiling);
    } catch (const std::exception& e) {
        snapshot.readback_errors.push_back("enable_profiling: " + std::string(e.what()));
    }

    try {
        snapshot.actual_cache_dir = compiled_model.get_property<std::string>(ov::cache_dir);
    } catch (const std::exception& e) {
        snapshot.readback_errors.push_back("cache_dir: " + std::string(e.what()));
    }

    snapshot.readback_complete = snapshot.readback_errors.empty();
    return snapshot;
}

/// @brief 将 OpenVINO 返回的实际设备列表压缩成稳定的诊断字符串。
///
/// AUTO 可能返回多个执行设备；EngineConfig 仍保留单一 actual_device 字段以兼容
/// 现有接口，因此这里使用逗号连接完整列表，Supports() 再按分隔符逐项识别能力。
std::string JoinExecutionDevices(const std::vector<std::string>& devices) {
    std::string result;
    for (const auto& device : devices) {
        if (device.empty()) {
            continue;
        }
        if (!result.empty()) {
            result += ",";
        }
        result += NormalizeDevice(device);
    }
    return result;
}

bool HasExecutionDevice(const std::string& actual_devices, const std::string& wanted) {
    const auto normalized_wanted = NormalizeDevice(wanted);
    std::string token;
    for (std::size_t i = 0; i <= actual_devices.size(); ++i) {
        const bool at_end = i == actual_devices.size();
        const char character = at_end ? ',' : actual_devices[i];
        if (character == ',' || character == ';' || character == '+' ||
            std::isspace(static_cast<unsigned char>(character))) {
            if (!token.empty()) {
                if (NormalizeDevice(token) == normalized_wanted) {
                    return true;
                }
                token.clear();
            }
            continue;
        }
        token += character;
    }
    return false;
}

size_t TensorTypeSize(TensorType type) {
    switch (type) {
        case TensorType::FP32:
        case TensorType::INT32:
            return 4;
        case TensorType::FP16:
            return 2;
        case TensorType::INT8:
        case TensorType::UINT8:
        case TensorType::BOOL:
            return 1;
        case TensorType::INT64:
            return 8;
        case TensorType::UNKNOWN:
        default:
            return 0;
    }
}

TensorType FromOvElementType(const ov::element::Type& type) {
    if (type == ov::element::f32) {
        return TensorType::FP32;
    }
    if (type == ov::element::f16) {
        return TensorType::FP16;
    }
    if (type == ov::element::i8) {
        return TensorType::INT8;
    }
    if (type == ov::element::u8) {
        return TensorType::UINT8;
    }
    if (type == ov::element::i32) {
        return TensorType::INT32;
    }
    if (type == ov::element::i64) {
        return TensorType::INT64;
    }
    if (type == ov::element::boolean) {
        return TensorType::BOOL;
    }
    return TensorType::UNKNOWN;
}

ov::element::Type ToOvElementType(TensorType type) {
    switch (type) {
        case TensorType::FP32:
            return ov::element::f32;
        case TensorType::FP16:
            return ov::element::f16;
        case TensorType::INT8:
            return ov::element::i8;
        case TensorType::UINT8:
            return ov::element::u8;
        case TensorType::INT32:
            return ov::element::i32;
        case TensorType::INT64:
            return ov::element::i64;
        case TensorType::BOOL:
            return ov::element::boolean;
        case TensorType::UNKNOWN:
        default:
            throw std::runtime_error("Unsupported TensorType");
    }
}

bool IsYuvFormat(PixelFormat format) {
    return format == PixelFormat::kNV12 || format == PixelFormat::kI420;
}

ov::preprocess::ColorFormat ToInputColorFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::kNV12:
            return ov::preprocess::ColorFormat::NV12_TWO_PLANES;
        case PixelFormat::kI420:
            return ov::preprocess::ColorFormat::I420_THREE_PLANES;
        case PixelFormat::kBGR24:
            return ov::preprocess::ColorFormat::BGR;
        case PixelFormat::kRGB24:
            return ov::preprocess::ColorFormat::RGB;
        case PixelFormat::kGRAY8:
            return ov::preprocess::ColorFormat::GRAY;
        case PixelFormat::kNV21:
        case PixelFormat::kUnknown:
        default:
            throw std::runtime_error("Unsupported OpenVINO preprocess input pixel format");
    }
}

ov::preprocess::ColorFormat ToModelColorFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::kBGR24:
            return ov::preprocess::ColorFormat::BGR;
        case PixelFormat::kRGB24:
            return ov::preprocess::ColorFormat::RGB;
        case PixelFormat::kGRAY8:
            return ov::preprocess::ColorFormat::GRAY;
        default:
            throw std::runtime_error("Unsupported OpenVINO preprocess model pixel format");
    }
}

/// @brief 判断 OpenVINO 维度是否为合法的静态通道维度。
///
/// 合法的通道值：
/// - 1：灰度图（单通道）
/// - 3：RGB/BGR 彩色图
/// - 4：RGBA/BGRA 带透明度彩色图
///
/// @param dim OpenVINO 维度对象
/// @return true 表示是静态维度且通道值合法，false 表示动态或通道值不合法
bool IsStaticChannelDim(const ov::Dimension& dim) {
    if (!dim.is_static()) {
        return false;
    }

    const auto value = dim.get_length();
    return value == 1 || value == 3 || value == 4;
}

/// @brief 通过 OpenVINO 模型输入形状推断图像布局
/// @param shape 输入形状
/// @return 图像布局字符串，如 "NCHW"、"NHWC"、"CHW"、"HWC"
std::string InferImageLayout(const ov::PartialShape& shape) {
    if (shape.rank().is_dynamic()) {
        return {};
    }

    const auto rank = shape.rank().get_length();
    if (rank == 4) {
        if (IsStaticChannelDim(shape[1])) {
            return "NCHW";
        }
        if (IsStaticChannelDim(shape[3])) {
            return "NHWC";
        }
    }

    if (rank == 3) {
        if (IsStaticChannelDim(shape[0])) {
            return "CHW";
        }
        if (IsStaticChannelDim(shape[2])) {
            return "HWC";
        }
    }

    return {};
}

OpenVinoPreprocessConfig ResolveOpenVinoPreprocessConfig(const std::shared_ptr<ov::Model>& model, OpenVinoPreprocessConfig config) {
    if (!config.enabled) {
        return config;
    }

    if (model->inputs().size() != 1) {
        throw std::runtime_error("OpenVINO internal image preprocessing requires a single model input");
    }

    const auto inferred_layout = InferImageLayout(model->input().get_partial_shape());
    if (config.model_input_layout.empty()) {
        config.model_input_layout = inferred_layout;
        if (inferred_layout.empty()) {
            LOG_MAIN_WARN_AT("Failed to infer model input layout from OpenVINO model shape");
        } else {
            LOG_MAIN_INFO_AT("Inferred OpenVINO model input layout: {}", config.model_input_layout);
        }
    } else {
        config.model_input_layout = NormalizeImageLayout(std::move(config.model_input_layout));
        // 显式 layout 与可从原始模型确定的 channel 位置冲突时，继续编译可能让
        // 同一 shape 被当成不同的 C/H/W 排列。必须在 PrePostProcessor 建图前
        // 拒绝，避免错误直到首帧才表现为颜色、resize 或 reshape 结果异常。
        if (!inferred_layout.empty() && config.model_input_layout != inferred_layout) {
            throw std::runtime_error("OpenVINO configured model input layout does not match "
                                     "the model input layout: configured=" +
                                     config.model_input_layout + ", inferred=" + inferred_layout);
        }
    }

    return config;
}

std::shared_ptr<ov::Model> ApplyOpenVinoPreprocess(std::shared_ptr<ov::Model> model,
                                                   const OpenVinoPreprocessConfig& config) {
    if (!config.enabled) {
        return model;
    }

    if (model->inputs().size() != 1) {
        throw std::runtime_error("OpenVINO internal image preprocessing requires a single model input");
    }

    const auto model_element_type = model->input().get_element_type();
    ov::preprocess::PrePostProcessor processor(model);
    auto& input = processor.input();
    auto& tensor = input.tensor();

    tensor.set_element_type(ov::element::u8);
    tensor.set_spatial_dynamic_shape();

    switch (config.input_pixel_format) {
        case PixelFormat::kNV12:
            tensor.set_color_format(ov::preprocess::ColorFormat::NV12_TWO_PLANES, {"y", "uv"});
            break;
        case PixelFormat::kI420:
            tensor.set_color_format(ov::preprocess::ColorFormat::I420_THREE_PLANES, {"y", "u", "v"});
            break;
        case PixelFormat::kBGR24:
        case PixelFormat::kRGB24:
        case PixelFormat::kGRAY8:
            tensor.set_layout("NHWC");
            tensor.set_color_format(ToInputColorFormat(config.input_pixel_format));
            break;
        default:
            throw std::runtime_error("Unsupported OpenVINO internal preprocess input format");
    }

    if (!config.model_input_layout.empty()) {
        input.model().set_layout(ov::Layout(config.model_input_layout));
    }

    auto& preprocess = input.preprocess();
    if (IsYuvFormat(config.input_pixel_format) ||
        config.input_pixel_format != config.model_pixel_format) {
        preprocess.convert_color(ToModelColorFormat(config.model_pixel_format));
    }
    preprocess.resize(ov::preprocess::ResizeAlgorithm::RESIZE_LINEAR);
    preprocess.convert_element_type(model_element_type);
    if (config.scale > 0.0f) {
        preprocess.scale(config.scale);
    }

    return processor.build();
}

TensorShape FromOvShape(const ov::Shape& shape) {
    TensorShape result;
    result.dims.reserve(shape.size());
    for (auto dim : shape) {
        if (dim > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            throw std::runtime_error("OpenVINO shape dimension is too large");
        }
        result.dims.push_back(static_cast<int64_t>(dim));
    }
    return result;
}

std::vector<std::size_t> StaticShapeFromPartialShape(const ov::PartialShape& shape) {
    std::vector<std::size_t> result;
    if (shape.rank().is_dynamic()) {
        return result;
    }

    result.reserve(shape.rank().get_length());
    for (const auto& dim : shape) {
        if (!dim.is_static()) {
            result.clear();
            return result;
        }
        result.push_back(static_cast<std::size_t>(dim.get_length()));
    }

    return result;
}

ov::Shape ToOvShape(const TensorShape& shape) {
    ov::Shape result;
    result.reserve(shape.dims.size());
    for (auto dim : shape.dims) {
        if (dim < 0) {
            throw std::runtime_error("Input tensor contains dynamic dimension");
        }
        result.push_back(static_cast<size_t>(dim));
    }
    return result;
}

std::string PortName(const ov::Output<const ov::Node>& port,
                     size_t index,
                     bool is_input) {
    const auto& names = port.get_names();
    if (!names.empty()) {
        return *names.begin();
    }

    try {
        return port.get_any_name();
    } catch (const std::exception&) {
        return std::string(is_input ? "input_" : "output_") + std::to_string(index);
    }
}

/// @brief 将 TensorShape 转换为 OpenVINO 的 ov::Shape
ov::Shape ToStaticReshapeShape(const TensorShape& shape) {
    if (shape.dims.size() != 4) {
        throw std::runtime_error("OpenVINO static reshape input shape must have rank 4");
    }
    /// 仅支持 batch 大小为 1 的 reshape, 每次处理一帧图像
    if (shape.dims.front() != 1) {
        throw std::runtime_error("OpenVINO static reshape supports batch size 1 only");
    }

    ov::Shape result;
    result.reserve(shape.dims.size());
    for (const auto dimension : shape.dims) {
        if (dimension <= 0 || static_cast<std::uintmax_t>(dimension) > std::numeric_limits<size_t>::max()) {
            throw std::runtime_error("OpenVINO static reshape dimensions must be positive");
        }
        result.push_back(static_cast<size_t>(dimension));
    }
    return result;
}

/// @brief 对单输入图像模型应用受限的加载期静态 reshape。
///
/// 形状覆盖发生在 PrePostProcessor 之前，使 resize 的目标仍由重塑后的模型图
/// 决定。仅接受能从原始 I/O 推断为 NCHW 或 NHWC 的 rank-4 输入，且 channel
/// 维度必须保持不变；这样调用方不会意外把 RGB 模型改成单通道或以错误布局重编译。
void ApplyStaticReshape(const std::shared_ptr<ov::Model>& model, const OpenVinoStaticReshapeConfig& config) {
    // 仅支持单输入模型。
    if (!model || model->inputs().size() != 1) {
        throw std::runtime_error("OpenVINO static reshape requires exactly one model input");
    }

    const auto input = model->input();
    const auto source_shape = input.get_partial_shape();
    if (source_shape.rank().is_dynamic() || source_shape.rank().get_length() != 4) {
        throw std::runtime_error("OpenVINO static reshape requires a rank-4 image model input");
    }

    const auto layout = InferImageLayout(source_shape);
    if (layout != "NCHW" && layout != "NHWC") {
        throw std::runtime_error("OpenVINO static reshape cannot infer image input layout");
    }

    const auto target_shape = ToStaticReshapeShape(config.input_shape);
    const auto channel_index = layout == "NCHW" ? 1u : 3u;
    const auto source_channel = source_shape[channel_index];
    if (!source_channel.is_static() || target_shape[channel_index] != static_cast<size_t>(source_channel.get_length())) {
        throw std::runtime_error("OpenVINO static reshape must preserve the image channel dimension");
    }

    // 单输入 profile 用端口索引而不是 tensor 名称作为 reshape key。部分 IR（尤其
    // 是测试或第三方导出的极小图）没有可查询的 tensor name，不能把日志用的
    // `input_0` 回退名误当成 OpenVINO 可用于 reshape 的真实名称。
    std::map<size_t, ov::PartialShape> target_shapes;
    target_shapes.emplace(0, ov::PartialShape(target_shape));
    model->reshape(target_shapes);
}

TensorShape FromPartialShape(const ov::PartialShape& shape) {
    TensorShape result;
    if (shape.rank().is_dynamic()) {
        return result;
    }

    result.dims.reserve(shape.rank().get_length());
    for (const auto& dim : shape) {
        result.dims.push_back(dim.is_static() ? dim.get_length() : -1);
    }
    return result;
}

TensorDesc BuildTensorDesc(const ov::Output<const ov::Node>& port,
                           size_t index,
                           bool is_input) {
    TensorDesc desc;
    desc.name = PortName(port, index, is_input);
    desc.type = FromOvElementType(port.get_element_type());
    desc.shape = FromPartialShape(port.get_partial_shape());
    desc.dynamic_shape = port.get_partial_shape().is_dynamic();
    desc.is_input = is_input;

    if (!desc.dynamic_shape) {
        auto success = desc.shape.TryElementCount(desc.element_count);
        if (!success) {
            throw std::runtime_error("Failed to calculate element count for tensor: " + desc.name);
        }
        desc.bytes = desc.element_count * TensorTypeSize(desc.type);
    }

    return desc;
}

TensorModelDesc BuildModelDesc(const ov::CompiledModel& model) {
    TensorModelDesc desc;

    const auto inputs = model.inputs();
    desc.inputs.reserve(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        desc.inputs.push_back(BuildTensorDesc(inputs[i], i, true));
    }

    const auto outputs = model.outputs();
    desc.outputs.reserve(outputs.size());
    for (size_t i = 0; i < outputs.size(); ++i) {
        desc.outputs.push_back(BuildTensorDesc(outputs[i], i, false));
    }

    return desc;
}

/// @brief 判断原始 OpenVINO 模型图是否声明动态 I/O 维度。
///
/// PrePostProcessor 为了接收任意分辨率的视频帧，会在编译前把 provider 侧的
/// 输入 tensor 设置为动态空间维度。这只是传输层能够接受不同尺寸的原始图像，
/// 并不等价于模型计算图已经支持 reshape。动态能力必须在应用预处理图之前从
/// 原始模型读取，否则固定 640x640 的模型会被错误地标记为 DYNAMIC。
bool HasDynamicShape(const ov::Model& model) {
    const auto has_dynamic = [](const auto& ports) {
        return std::any_of(ports.begin(), ports.end(), [](const auto& port) {
            return port.get_partial_shape().is_dynamic();
        });
    };

    return has_dynamic(model.inputs()) || has_dynamic(model.outputs());
}

const TensorPlane* FindInputTensor(const TensorFrame& frame,
                                  const ov::Output<const ov::Node>& port,
                                  size_t index,
                                  size_t total_inputs) {
    for (const auto& name : port.get_names()) {
        if (const auto* tensor = frame.FindTensor(name)) {
            return tensor;
        }
    }

    try {
        if (const auto* tensor = frame.FindTensor(port.get_any_name())) {
            return tensor;
        }
    } catch (const std::exception&) {
    }

    const auto fallback = std::string("input_") + std::to_string(index);
    if (const auto* tensor = frame.FindTensor(fallback)) {
        return tensor;
    }

    if (total_inputs == 2) {
        const char* plane_names[] = {"y", "uv"};
        if (index < 2) {
            if (const auto* tensor = frame.FindTensor(plane_names[index])) {
                return tensor;
            }
        }
    }

    if (total_inputs == 3) {
        const char* plane_names[] = {"y", "u", "v"};
        if (index < 3) {
            if (const auto* tensor = frame.FindTensor(plane_names[index])) {
                return tensor;
            }
        }
    }

    if (total_inputs == 1 && frame.Size() == 1) {
        return frame.FirstTensor();
    }

    return nullptr;
}

/// @brief 绑定推理请求的输入张量
/// 将 TensorFrame 中的数据绑定到 OpenVINO 推理请求的输入端口。
/// @param request 推理请求
/// @param compiled_model 编译后的模型，用于获取输入端口信息
/// @param input 待绑定的输入张量包
void BindInputs(ov::InferRequest& request,
                const ov::CompiledModel& compiled_model,
                const TensorFrame& input) {
    const auto inputs = compiled_model.inputs();
    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto& port = inputs[i];
        const auto* tensor = FindInputTensor(input, port, i, inputs.size());
        if (!tensor) {
            throw std::runtime_error("Missing input tensor: " + PortName(port, i, true));
        }
        if (!tensor->data || tensor->bytes == 0) {
            throw std::runtime_error("Input tensor has empty data: " + tensor->name);
        }

        const auto ov_type = ToOvElementType(tensor->type);
        const auto ov_shape = ToOvShape(tensor->shape);
        ov::Tensor ov_tensor(ov_type, ov_shape, tensor->data);

        if (ov_tensor.get_byte_size() > tensor->bytes) {
            throw std::runtime_error("Input tensor buffer is smaller than shape requires: " + tensor->name);
        }

        request.set_tensor(port, ov_tensor);
    }
}

TensorFrame CollectOutputs(ov::InferRequest& request,
                             const ov::CompiledModel& compiled_model) {
    TensorFrame output;
    const auto outputs = compiled_model.outputs();
    for (size_t i = 0; i < outputs.size(); ++i) {
        const auto& port = outputs[i];
        auto ov_tensor = request.get_tensor(port);
        const auto bytes = ov_tensor.get_byte_size();
        auto buffer = std::make_shared<CpuTensorBuffer>(bytes);

        if (bytes > 0 && ov_tensor.data()) {
            std::memcpy(buffer->Data(), ov_tensor.data(), bytes);
        }

        auto tensor = std::make_unique<TensorPlane>();
        tensor->name = PortName(port, i, false);
        tensor->type = FromOvElementType(ov_tensor.get_element_type());
        tensor->shape = FromOvShape(ov_tensor.get_shape());
        tensor->SetBuffer(std::move(buffer), TensorMemoryType::OPENVINO_CPU);
        output.AddTensor(std::move(tensor));
    }

    return output;
}

}  // namespace

OpenVinoEngine::OpenVinoEngine() = default;

OpenVinoEngine::~OpenVinoEngine() {
    Release();
}

OpenVinoCpuEngine::OpenVinoCpuEngine() = default;

OpenVinoCpuEngine::~OpenVinoCpuEngine() = default;

bool OpenVinoEngine::LoadModel(const EngineLoadConfig& load_config) {
    const auto& config = load_config.engine;
    std::shared_ptr<OpenVinoInferRequestPool> old_pool;
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        // 同一个 engine 不能同时编译两个候选模型。重复 LoadModel 直接拒绝，
        // 但 Release 期间的加载会等待本轮释放完成后再开始，保证调用方看到
        // 明确的先后顺序，而不是读到半套 compiled_model_。
        if (state_ == EngineState::kLoading) {
            LOG_MAIN_WARN_AT("OpenVINO engine is already loading a model");
            return false;
        }
        while (state_ == EngineState::kReleasing) {
            state_cv_.wait(lock);
        }
        if (state_ == EngineState::kLoading) {
            LOG_MAIN_WARN_AT("OpenVINO engine entered loading while waiting");
            return false;
        }

        state_ = EngineState::kLoading;
        // 先把旧 pool 从共享成员中摘下，再在锁外 shutdown/wait。已经拿到旧
        // pool 副本的推理仍可完成；新的提交在成员为空和 Loading 状态下会失败。
        old_pool = std::move(request_pool_);
        compiled_model_ = {};
        tensor_model_desc_ = {};
        config_ = {};
        preprocess_config_ = {};
        runtime_snapshot_ = {};
    }

    if (old_pool) {
        old_pool->Shutdown();
        old_pool->WaitAll();
    }

    // 编译阶段只操作局部变量。只有全部步骤成功后才一次性提交到成员，避免
    // 其他线程观察到“模型已经可用但 request pool 尚未初始化”的中间状态。
    EngineConfig candidate_config = config;
    candidate_config.backend = "OPENVINO";
    candidate_config.device = NormalizeDevice(config.device.empty() ? kDefaultDevice : config.device);
    if (!IsSupportedDevice(candidate_config.device)) {
        LOG_MAIN_ERROR_AT("Unsupported OpenVINO device: {}", config.device);
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = EngineState::kFailed;
        state_cv_.notify_all();
        return false;
    }
    // request_count 表示可同时持有多少个单帧 infer request，不能被误解为模型
    // batch。当前 TensorFrame、预处理、后处理和 FrameResult 都没有批拆分语义，
    // 因此 provider 入口也必须拒绝非单 batch，不能只依赖 Session factory 约束。
    if (config.batch_size != 1 || config.max_batch_size != 1) {
        LOG_MAIN_ERROR_AT("OpenVINO engine supports batch size 1 only: batch_size={}, max_batch_size={}",
                          config.batch_size,
                          config.max_batch_size);
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = EngineState::kFailed;
        state_cv_.notify_all();
        return false;
    }
    candidate_config.batch_size = 1;
    candidate_config.request_count = std::max<uint32_t>(1, config.request_count);
    candidate_config.support_async = true;
    candidate_config.max_batch_size = 1;

    // cache 默认关闭；调用方一旦显式提供该配置，就必须给出可执行的正容量。
    // 不把 0 悄悄解释成“无限制”或“使用旧容量”，可避免模型切换时产生难以
    // 观察的内存驻留行为。省略配置才是关闭 cache 的唯一兼容方式。
    const auto cache_capacity = load_config.compiled_model_cache.has_value()
                                    ? load_config.compiled_model_cache->capacity
                                    : 0u;
    if (load_config.compiled_model_cache.has_value() && cache_capacity == 0) {
        LOG_MAIN_ERROR_AT("OpenVINO compiled-model cache capacity must be positive when enabled");
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = EngineState::kFailed;
        state_cv_.notify_all();
        return false;
    }

    OpenVinoPreprocessConfig candidate_preprocess_config;
    std::shared_ptr<OpenVinoInferRequestPool> candidate_pool;
    ov::CompiledModel candidate_compiled_model;
    TensorModelDesc candidate_desc;
    OpenVinoRuntimeSnapshot candidate_runtime_snapshot;
    std::optional<std::string> cache_key;
    bool cache_hit = false;
    try {
        // 路径校验放在进入 Loading 之后，确保空路径/不存在路径与 read_model、编译
        // 异常拥有相同的 Failed 状态和清理语义；否则调用方会看到“返回 false 但
        // engine 仍是 Created/Ready”的不一致，无法可靠决定是否可以重试。
        if (candidate_config.model_path.empty()) {
            throw std::runtime_error("OpenVINO model path is empty");
        }
        if (!std::filesystem::exists(candidate_config.model_path)) {
            throw std::runtime_error("OpenVINO model path does not exist: " +
                                     candidate_config.model_path);
        }
        if (cache_capacity > 0) {
            cache_key = BuildCompiledModelCacheKey(load_config, candidate_config);
            std::lock_guard<std::mutex> lock(state_mutex_);
            compiled_model_cache_stats_.capacity = cache_capacity;
            // 容量从较大值调小后先逐出尾部，才能保证本次命中/插入不超过新上限。
            while (compiled_model_cache_.size() > cache_capacity) {
                compiled_model_cache_.pop_back();
                ++compiled_model_cache_stats_.eviction_count;
            }
            if (cache_key.has_value()) {
                const auto cache_it = std::find_if(
                    compiled_model_cache_.begin(),
                    compiled_model_cache_.end(),
                    [&cache_key](const CompiledModelCacheEntry& entry) {
                        return entry.key == *cache_key;
                    });
                if (cache_it != compiled_model_cache_.end()) {
                    // list::splice 只重连节点，不复制或销毁 OpenVINO 资源；随后把
                    // entry 内容复制到本次候选变量，锁外再创建独立 request pool。
                    compiled_model_cache_.splice(compiled_model_cache_.begin(),
                                                 compiled_model_cache_,
                                                 cache_it);
                    const auto& entry = compiled_model_cache_.front();
                    candidate_compiled_model = entry.compiled_model;
                    candidate_config.support_dynamic_shape = entry.support_dynamic_shape;
                    candidate_config.actual_device = entry.actual_device;
                    candidate_desc = entry.tensor_model_desc;
                    candidate_preprocess_config = entry.preprocess_config;
                    candidate_runtime_snapshot = entry.runtime_snapshot;
                    cache_hit = true;
                    ++compiled_model_cache_stats_.hit_count;
                } else {
                    ++compiled_model_cache_stats_.miss_count;
                }
            }
            compiled_model_cache_stats_.entry_count =
                static_cast<uint32_t>(compiled_model_cache_.size());
        } else {
            // 调用方取消 cache 时立即释放本 engine 保留的非活动图，而当前正在
            // 执行的 compiled_model 由局部/成员引用单独维持，不会被此处清空。
            std::lock_guard<std::mutex> lock(state_mutex_);
            compiled_model_cache_.clear();
            compiled_model_cache_stats_.capacity = 0;
            compiled_model_cache_stats_.entry_count = 0;
        }

        if (!cache_hit) {
            auto raw_model = core_.read_model(candidate_config.model_path);
            if (load_config.reshape.has_value()) {
                ApplyStaticReshape(raw_model, *load_config.reshape);
            }
            // 必须在 ApplyOpenVinoPreprocess 之前采样。后者总会为图像输入设置动态
            // 空间维度，若在其之后判断就会把预处理 transport 的动态性误报为模型图
            // 的动态 reshape 能力，进而诱导上层发送当前 adapter 无法处理的动态输出。
            candidate_config.support_dynamic_shape = HasDynamicShape(*raw_model);

            // 设置 openvino 预处理器
            if (load_config.preprocess.has_value()) {
                candidate_preprocess_config =
                    ResolveOpenVinoPreprocessConfig(raw_model, *load_config.preprocess);
            }

            auto model = ApplyOpenVinoPreprocess(std::move(raw_model), candidate_preprocess_config);
            ov::AnyMap runtime_properties;
            if (load_config.runtime.has_value()) {
                runtime_properties = BuildRuntimeProperties(*load_config.runtime);
            }
            candidate_compiled_model = core_.compile_model(model,
                                                           candidate_config.device,
                                                           runtime_properties);
            std::vector<std::string> execution_devices;
            try {
                // execution_devices 是 OpenVINO 对 AUTO 解析结果的标准只读属性。保留
                // vector 的完整内容用于诊断，再压缩到 EngineConfig::actual_device，
                // 让不需要依赖 OpenVINO 头文件的调用方也能读取实际设备。
                execution_devices = candidate_compiled_model.get_property<std::vector<std::string>>(
                    ov::execution_devices);
            } catch (const std::exception& e) {
                // 旧插件可能没有实现该属性；CPU 请求仍可用请求值作为保守回退，
                // AUTO 则只报告 AUTO，不把未知 runtime 结果伪装成某个物理设备。
                LOG_MAIN_WARN_AT("OpenVINO execution device property is unavailable: {}", e.what());
            }
            candidate_config.actual_device = JoinExecutionDevices(execution_devices);
            if (candidate_config.actual_device.empty()) {
                // 个别插件版本可能不暴露 execution_devices；请求 CPU 时该回退值仍然
                // 是可验证的实际设备，AUTO 则保留请求值，避免伪造某个物理设备。
                candidate_config.actual_device = candidate_config.device;
            }
            candidate_desc = BuildModelDesc(candidate_compiled_model);

            candidate_runtime_snapshot = ReadRuntimeSnapshot(
                candidate_compiled_model,
                load_config.runtime.value_or(OpenVinoRuntimeConfig{}));
            if (!candidate_runtime_snapshot.readback_complete) {
                LOG_MAIN_WARN_AT("OpenVINO runtime property readback incomplete: {} attribute(s)",
                                 candidate_runtime_snapshot.readback_errors.size());
            }
        }

        candidate_pool = std::make_shared<OpenVinoInferRequestPool>();
        if (!candidate_pool->Initialize(candidate_compiled_model, candidate_config.request_count)) {
            throw std::runtime_error("Failed to initialize OpenVINO request pool");
        }
        if (cache_key.has_value() && !cache_hit) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            // 本 engine 的 LoadModel 串行执行；仍在插入前再次按 >= 逐出，以覆盖
            // “容量已满且本次为新 profile”的情况。活动 compiled_model 有自己的
            // 引用，即使它刚好也是被逐出的 LRU 条目，当前推理也不会失效。
            while (compiled_model_cache_.size() >= cache_capacity) {
                compiled_model_cache_.pop_back();
                ++compiled_model_cache_stats_.eviction_count;
            }
            CompiledModelCacheEntry entry;
            entry.key = *cache_key;
            entry.compiled_model = candidate_compiled_model;
            entry.support_dynamic_shape = candidate_config.support_dynamic_shape;
            entry.actual_device = candidate_config.actual_device;
            entry.tensor_model_desc = candidate_desc;
            entry.preprocess_config = candidate_preprocess_config;
            entry.runtime_snapshot = candidate_runtime_snapshot;
            compiled_model_cache_.push_front(std::move(entry));
            compiled_model_cache_stats_.entry_count =
                static_cast<uint32_t>(compiled_model_cache_.size());
        }
    } catch (const std::exception& e) {
        LOG_MAIN_ERROR_AT("Failed to load OpenVINO model {}: {}", config.model_path, e.what());
        std::lock_guard<std::mutex> lock(state_mutex_);
        // 失败后不保留候选资源。旧模型已经在进入 Loading 时被有序释放，
        // 因而 Failed 状态不会错误地继续暴露上一代模型。
        request_pool_.reset();
        compiled_model_ = {};
        tensor_model_desc_ = {};
        config_ = {};
        preprocess_config_ = {};
        runtime_snapshot_ = {};
        state_ = EngineState::kFailed;
        state_cv_.notify_all();
        return false;
    } catch (...) {
        LOG_MAIN_ERROR_AT("Failed to load OpenVINO model {}: unknown exception", config.model_path);
        std::lock_guard<std::mutex> lock(state_mutex_);
        request_pool_.reset();
        compiled_model_ = {};
        tensor_model_desc_ = {};
        config_ = {};
        preprocess_config_ = {};
        runtime_snapshot_ = {};
        state_ = EngineState::kFailed;
        state_cv_.notify_all();
        return false;
    }

    const auto loaded_requested_device = candidate_config.device;
    const auto loaded_actual_device = candidate_config.actual_device;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        request_pool_ = std::move(candidate_pool);
        compiled_model_ = std::move(candidate_compiled_model);
        tensor_model_desc_ = std::move(candidate_desc);
        config_ = std::move(candidate_config);
        preprocess_config_ = std::move(candidate_preprocess_config);
        runtime_snapshot_ = std::move(candidate_runtime_snapshot);
        state_ = EngineState::kReady;
        state_cv_.notify_all();
    }
    LOG_MAIN_INFO_AT("Loaded OpenVINO model on {} (requested {}): {}",
                     loaded_actual_device,
                     loaded_requested_device,
                     config.model_path);
    return true;
}

bool OpenVinoEngine::Infer(const TensorFrame& input, TensorFrame& output) {
    return InferWithStatus(input, output).Ok();
}

InferenceStatus OpenVinoEngine::InferWithStatus(const TensorFrame& input, TensorFrame& output) {
    std::shared_ptr<OpenVinoInferRequestPool> pool;
    ov::CompiledModel model;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ != EngineState::kReady || !request_pool_) {
            const auto message = state_ == EngineState::kLoading
                                     ? "OpenVINO engine is loading a model"
                                     : state_ == EngineState::kReleasing
                                           ? "OpenVINO engine is releasing resources"
                                           : "OpenVINO engine is not initialized";
            LOG_MAIN_WARN_AT("{}", message);
            return InferenceStatus::Failure(
                state_ == EngineState::kLoading || state_ == EngineState::kReleasing
                    ? InferenceStatusCode::kShuttingDown
                    : InferenceStatusCode::kNotInitialized,
                InferenceStage::kSubmit,
                message);
        }
        // 只在锁内复制共享资源；实际 OpenVINO 调用在锁外执行，释放线程可以
        // 修改成员状态，而已开始的推理仍由这些局部副本保持资源有效。
        pool = request_pool_;
        model = compiled_model_;
    }
    if (input.Empty()) {
        LOG_MAIN_ERROR_AT("OpenVINO inference input is empty");
        return InferenceStatus::Failure(InferenceStatusCode::kInvalidInput,
                                        InferenceStage::kSubmit,
                                        "OpenVINO inference input is empty");
    }

    auto request = pool->Acquire();
    RequestLease lease(pool, std::move(request));
    if (!lease.Valid()) {
        LOG_MAIN_ERROR_AT("Failed to acquire OpenVINO infer request");
        return InferenceStatus::Failure(InferenceStatusCode::kRequestUnavailable,
                                        InferenceStage::kSubmit,
                                        "OpenVINO infer request is unavailable");
    }

    auto stage = InferenceStage::kSubmit;
    try {
        BindInputs(*lease, model, input);
        stage = InferenceStage::kExecute;
        lease->infer();
        stage = InferenceStage::kOutputCollection;
        output = CollectOutputs(*lease, model);
        output.tensor_meta_ = input.tensor_meta_;
        return InferenceStatus::Success();
    } catch (const std::exception& e) {
        LOG_MAIN_ERROR_AT("OpenVINO inference failed: {}", e.what());
        const auto code = stage == InferenceStage::kOutputCollection
                              ? InferenceStatusCode::kOutputCollectionFailed
                              : InferenceStatusCode::kProviderExecutionFailed;
        const auto message = stage == InferenceStage::kOutputCollection
                                 ? "OpenVINO output collection failed"
                                 : "OpenVINO inference execution failed";
        return InferenceStatus::Failure(code, stage, message, e.what());
    } catch (...) {
        LOG_MAIN_ERROR_AT("OpenVINO inference failed with an unknown exception");
        const auto code = stage == InferenceStage::kOutputCollection
                              ? InferenceStatusCode::kOutputCollectionFailed
                              : InferenceStatusCode::kProviderExecutionFailed;
        const auto message = stage == InferenceStage::kOutputCollection
                                 ? "OpenVINO output collection failed"
                                 : "OpenVINO inference execution failed";
        return InferenceStatus::Failure(code, stage, message, "unknown provider exception");
    }
}

bool OpenVinoEngine::InferAsync(const InferContext& ctx,
                                   const TensorFrame& input,
                                   InferCallback cb) {
    return InferAsyncWithStatus(ctx, input, std::move(cb)).Ok();
}

InferenceStatus OpenVinoEngine::InferAsyncWithStatus(const InferContext& ctx,
                                                         const TensorFrame& input,
                                                         InferCallback cb) {
    std::shared_ptr<OpenVinoInferRequestPool> pool;
    ov::CompiledModel model;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ != EngineState::kReady || !request_pool_) {
            const auto message = state_ == EngineState::kLoading
                                     ? "OpenVINO engine is loading a model"
                                     : state_ == EngineState::kReleasing
                                           ? "OpenVINO engine is releasing resources"
                                           : "OpenVINO engine is not initialized";
            LOG_MAIN_WARN_AT("{}", message);
            return InferenceStatus::Failure(
                state_ == EngineState::kLoading || state_ == EngineState::kReleasing
                    ? InferenceStatusCode::kShuttingDown
                    : InferenceStatusCode::kNotInitialized,
                InferenceStage::kSubmit,
                message);
        }
        // 与同步路径一样，先锁内取得当前 generation 的 pool/model，再在锁外
        // Acquire 和 start_async。Release 会关闭这份 pool，后续 Acquire 会立即
        // 失败，不会在已经停止的请求池中无限等待。
        pool = request_pool_;
        model = compiled_model_;
    }
    if (input.Empty()) {
        LOG_MAIN_ERROR_AT("OpenVINO async inference input is empty");
        return InferenceStatus::Failure(InferenceStatusCode::kInvalidInput,
                                        InferenceStage::kSubmit,
                                        "OpenVINO async inference input is empty");
    }
    if (ctx.cancellation && ctx.cancellation->IsCancellationRequested()) {
        return InferenceStatus::Failure(InferenceStatusCode::kCancelled,
                                        InferenceStage::kSubmit,
                                        "OpenVINO async inference was cancelled before submission");
    }

    auto input_copy = std::make_shared<TensorFrame>(input);
    auto request = pool->Acquire();
    if (!request) {
        LOG_MAIN_ERROR_AT("Failed to acquire OpenVINO async infer request");
        return InferenceStatus::Failure(InferenceStatusCode::kRequestUnavailable,
                                        InferenceStage::kSubmit,
                                        "OpenVINO async infer request is unavailable");
    }
    std::weak_ptr<OpenVinoInferRequestPool> weak_pool = pool;
    // token 回调只持有 request 的 weak_ptr：Session/engine 释放后不能因为调用方
    // 仍保存 token 而延长 request 生命周期，更不能取消已经归还给 pool 的新任务。
    const std::weak_ptr<ov::InferRequest> weak_request = request;
    auto cancellation_requested = std::make_shared<std::atomic_bool>(false);
    auto cancellation_registration =
        std::make_shared<InferenceCancellationToken::Registration>();
    auto lease = std::make_shared<RequestLease>(pool, std::move(request));

    try {
        BindInputs(**lease, model, *input_copy);
        if (ctx.cancellation && ctx.cancellation->IsCancellationRequested()) {
            return InferenceStatus::Failure(
                InferenceStatusCode::kCancelled,
                InferenceStage::kSubmit,
                "OpenVINO async inference was cancelled before start_async");
        }

        // ov::InferRequest::cancel() 是当前 OpenVINO runtime 提供的原生中断入口。
        // 注册在 start_async 前完成，随后会在 start_async 后补一次检查，覆盖 token
        // 恰好在注册与启动之间触发时“第一次 cancel 尚未作用于运行中 request”的窗口。
        auto cancel_request = [weak_request, cancellation_requested]() {
            cancellation_requested->store(true, std::memory_order_release);
            if (const auto request = weak_request.lock()) {
                try {
                    request->cancel();
                } catch (...) {
                    // request 可能尚未启动或已经完成；后续 callback 仍会依据 token
                    // 生成 kCancelled，不能让 provider 的竞态异常穿回 Cancel() 调用者。
                }
            }
        };
        if (ctx.cancellation) {
            *cancellation_registration = ctx.cancellation->Register(cancel_request);
            if (ctx.cancellation->IsCancellationRequested()) {
                return InferenceStatus::Failure(
                    InferenceStatusCode::kCancelled,
                    InferenceStage::kSubmit,
                    "OpenVINO async inference was cancelled before start_async");
            }
        }

        lease->operator->()->set_callback(
            [weak_pool,
             lease,
             model,
             input_copy,
             ctx,
             cancellation_registration,
             cb = std::move(cb)](std::exception_ptr ex) mutable {
                 // 先锁住 pool，保证当前 callback 的 bookkeeping 和最终归还都能
                 // 完成。engine->Release() 即使在用户 callback 内重入，也只会
                 // 等待到 MarkAsyncCompleted() 的完成计数满足条件，不会销毁这里
                 // 仍在使用的 pool。
                 auto pool = weak_pool.lock();

                 // request 由 callback 局部 shared_ptr 持有到回调结束；lease 只负责
                 // 转移所有权，不再反向持有 request/pool，避免 request 与 callback
                 // 之间形成长期强引用环。
                  auto request = lease->TakeRequestForAsync();
                  InferOutput result;

                  if (!request) {
                      result.status = InferenceStatus::Failure(
                          InferenceStatusCode::kRequestUnavailable,
                          InferenceStage::kExecute,
                          "OpenVINO async request is unavailable");
                      LOG_MAIN_ERROR_AT("OpenVINO async inference failed: {}", result.status.message);
                  } else if (ctx.cancellation && ctx.cancellation->IsCancellationRequested()) {
                      // 即使底层插件没有及时以 ov::Cancelled 回调，token 的公共语义
                      // 仍优先于输出采集：取消后不再读取旧输出，也不暴露成功结果。
                      result.status = InferenceStatus::Failure(
                          InferenceStatusCode::kCancelled,
                          InferenceStage::kExecute,
                          "OpenVINO async inference was cancelled");
                  } else if (ex) {
                      // OpenVINO 通过 exception_ptr 报告 provider 执行错误；这里把
                      // 原始异常文本放进 provider_detail，调用方不必依赖日志解析。
                      try {
                          std::rethrow_exception(ex);
                      } catch (const ov::Cancelled& e) {
                          result.status = InferenceStatus::Failure(
                              InferenceStatusCode::kCancelled,
                              InferenceStage::kExecute,
                              "OpenVINO async inference was cancelled",
                              e.what());
                      } catch (const std::exception& e) {
                          result.status = InferenceStatus::Failure(
                              InferenceStatusCode::kProviderExecutionFailed,
                              InferenceStage::kExecute,
                              "OpenVINO async inference execution failed",
                              e.what());
                      } catch (...) {
                          result.status = InferenceStatus::Failure(
                              InferenceStatusCode::kProviderExecutionFailed,
                              InferenceStage::kExecute,
                              "OpenVINO async inference execution failed",
                              "unknown provider exception");
                      }
                      LOG_MAIN_ERROR_AT("OpenVINO async inference failed: {}",
                                        result.status.provider_detail.empty()
                                            ? result.status.message
                                            : result.status.provider_detail);
                  } else {
                      try {
                          result.output = CollectOutputs(*request, model);
                          result.output.tensor_meta_ = input_copy->tensor_meta_;
                          result.status = InferenceStatus::Success();
                          result.success = true;
                      } catch (const std::exception& e) {
                          result.status = InferenceStatus::Failure(
                              InferenceStatusCode::kOutputCollectionFailed,
                              InferenceStage::kOutputCollection,
                              "OpenVINO async output collection failed",
                              e.what());
                          LOG_MAIN_ERROR_AT("OpenVINO async inference failed: {}", e.what());
                      } catch (...) {
                          result.status = InferenceStatus::Failure(
                              InferenceStatusCode::kOutputCollectionFailed,
                              InferenceStage::kOutputCollection,
                              "OpenVINO async output collection failed",
                              "unknown output collection exception");
                          LOG_MAIN_ERROR_AT("OpenVINO async inference failed: {}",
                                            result.status.provider_detail);
                      }
                  }

                 if (pool && request) {
                     // OpenVINO 已经完成 request，先更新 pool 状态再进入用户代码。
                     // 这样用户 callback 内调用 engine->Release() 时，WaitAll() 能
                     // 识别该 request 已完成；request 本身仍不会提前回到空闲队列。
                     pool->MarkAsyncCompleted();
                 }

                 try {
                     if (cb) {
                         cb(ctx, std::move(result));
                     }
                 } catch (const std::exception& e) {
                     LOG_MAIN_ERROR_AT("OpenVINO async callback failed: {}", e.what());
                 } catch (...) {
                     // 用户 callback 也可能抛出非 std::exception 类型。无条件捕获
                     // 保证后续的 callback 清理和 ReleaseAsync() 仍会执行，避免
                     // 异常穿出 OpenVINO 线程后遗留占用中的 request。
                     LOG_MAIN_ERROR_AT("OpenVINO async callback failed with an unknown exception");
                 }

                 // request 内部可能继续保存当前 OpenVINO callback。用户 callback
                 // 已经执行完后立即清空 std::function 捕获，避免 request/pool 的
                 // 延迟析构把用户上下文和大对象继续保留到下一次生命周期。
                 cb = {};
                 // 回调已完成，先解除 token 到 request 的订阅，再归还 request。
                 // 这一步是防止旧 token 在 request 被 pool 复用后误调用 cancel() 的
                 // 所有权边界，必须位于 ReleaseAsync() 之前。
                 cancellation_registration.reset();

                 if (pool && request) {
                     // 用户 callback 返回后才归还 request，确保 callback 执行期间不
                     // 会有另一个推理复用 OpenVINO 的同一 request 实例。
                     pool->ReleaseAsync(std::move(request));
                 }
             });
        lease->operator->()->start_async();
        if (cancellation_requested->load(std::memory_order_acquire)) {
            // Register() 与 start_async() 之间可能已经收到取消。第一次 cancel 在
            // request 未运行时可能无效，因此启动后再尝试一次原生取消。
            cancel_request();
        }
        return InferenceStatus::Success();
    } catch (const std::exception& e) {
        LOG_MAIN_ERROR_AT("Failed to start OpenVINO async inference: {}", e.what());
        // 如果 set_callback 或 start_async 失败，必须先清掉 request 内已经安装的
        // callback，再让 lease 析构归还 request；否则失败路径同样可能留下引用环。
        if (lease->Valid()) {
            try {
                lease->operator->()->set_callback(std::function<void(std::exception_ptr)>{});
            } catch (const std::exception& clear_error) {
                LOG_MAIN_WARN_AT("Failed to clear OpenVINO async callback: {}", clear_error.what());
            }
        }
        return InferenceStatus::Failure(InferenceStatusCode::kProviderExecutionFailed,
                                        InferenceStage::kSubmit,
                                        "OpenVINO async submission failed",
                                        e.what());
    } catch (...) {
        LOG_MAIN_ERROR_AT("Failed to start OpenVINO async inference with an unknown exception");
        if (lease->Valid()) {
            try {
                lease->operator->()->set_callback(std::function<void(std::exception_ptr)>{});
            } catch (...) {
                LOG_MAIN_WARN_AT("Failed to clear OpenVINO async callback after unknown exception");
            }
        }
        return InferenceStatus::Failure(InferenceStatusCode::kProviderExecutionFailed,
                                        InferenceStage::kSubmit,
                                        "OpenVINO async submission failed",
                                        "unknown provider exception");
    }
}

void OpenVinoEngine::Release() {
    std::shared_ptr<OpenVinoInferRequestPool> pool;
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        // LoadModel 正在编译时不持有 engine 锁；Release 等待其完成后重新判断，
        // 确保不会把刚提交的 Ready 模型误认为已释放或留下半初始化成员。
        while (state_ == EngineState::kLoading || state_ == EngineState::kReleasing) {
            state_cv_.wait(lock);
        }
        if (state_ == EngineState::kReleased) {
            return;
        }

        state_ = EngineState::kReleasing;
        pool = std::move(request_pool_);
        compiled_model_ = {};
        tensor_model_desc_ = {};
        config_ = {};
        preprocess_config_ = {};
        runtime_snapshot_ = {};
    }

    if (pool) {
        pool->Shutdown();
        pool->WaitAll();
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = EngineState::kReleased;
        state_cv_.notify_all();
    }
}

EngineCapability OpenVinoEngine::Supports() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    EngineCapability capability = EngineCapability::NONE;
    const auto actual_device = (state_ == EngineState::kReady && !config_.actual_device.empty())
                                   ? config_.actual_device
                                   : (state_ == EngineState::kReady && config_.device == "CPU"
                                          ? "CPU"
                                          : std::string{});
    if (HasExecutionDevice(actual_device, "CPU")) {
        capability |= EngineCapability::CPU;
    }
    if (HasExecutionDevice(actual_device, "GPU")) {
        capability |= EngineCapability::GPU;
    }
    if (config_.support_async) {
        capability |= EngineCapability::ASYNC;
        // OpenVINO 2026.2 的 ov::InferRequest 提供 cancel()；只有异步 request
        // 才存在可中断的运行窗口，因此不对同步 Infer() 声明 CANCEL capability。
        capability |= EngineCapability::CANCEL;
    }
    if (config_.support_dynamic_shape) {
        capability |= EngineCapability::DYNAMIC;
    }
    // BATCH 在公共枚举中保留给后续 provider，但当前 OpenVINO 单帧 adapter
    // 没有 batch 输入拼接和 FrameResult 拆分协议，不能声明该 capability。
    return capability;
}

TensorModelDesc OpenVinoEngine::GetModelDesc() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_ != EngineState::kReady) {
        return {};
    }
    return tensor_model_desc_;
}

EngineConfig OpenVinoEngine::GetEngineConfig() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    EngineConfig info = config_;
    info.backend = "OPENVINO";
    return info;
}

EngineState OpenVinoEngine::GetState() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

OpenVinoRuntimeSnapshot OpenVinoEngine::GetRuntimeSnapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return runtime_snapshot_;
}

OpenVinoCompiledModelCacheStats OpenVinoEngine::GetCompiledModelCacheStats() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return compiled_model_cache_stats_;
}