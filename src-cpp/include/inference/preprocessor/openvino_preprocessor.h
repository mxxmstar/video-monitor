#pragma once

#include <mutex>

#include "media/media_frame.h"
#include "preprocessor/i_preprocessor.h"
#include "tensordata/tensor_data.h"

struct SwsContext;

/// @brief 模型输入几何变换策略。
///
/// Stretch 直接把原始 YUV 交给 OpenVINO 缩放；Letterbox 先在 CPU 端构造
/// 等比缩放后的 YUV 画布，再由 OpenVINO 完成颜色转换。CenterCrop 尚未实现，
/// 因此必须拒绝，不能静默退回其他几何策略。
enum class ResizeMode {
    kStretch,   ///< 直接拉伸输入图像到模型输入大小
    kLetterbox, ///< 保持宽高比，构造 Letterbox 画布
    kCenterCrop ///< 保持宽高比，居中裁剪输入图像
};

/// @brief OpenVINO 视频帧预处理器配置。
///
/// 该类型把视频管线中的 NV12/I420 平面或 packed RGB/BGR/GRAY8 帧安全封装为 TensorFrame，
/// 并将 NV21 的 VU 色度平面显式转换成 OpenVINO 可识别的 NV12 UV 平面；在需要时
/// 还可构造 YUV letterbox 画布。颜色转换、模型颜色布局和归一化仍由 OpenVINO 的
/// `OpenVinoPreprocessConfig` 明确指定。因此它既可被检测模型使用，也可被分类
/// 等消费同一条 YUV 视频管线的模型复用。
struct OpenVinoFramePreprocessorConfig {
    /// @brief 模型输入高度
    uint32_t input_height{640};
    /// @brief 模型输入宽度
    uint32_t input_width{640};
    /// @brief 输入图像到模型空间的几何策略
    ResizeMode resize_mode{ResizeMode::kStretch};
    /// @brief 模型输入像素格式，Unknown 表示以 MediaFrame 为准
    PixelFormat pixel_format{PixelFormat::kUnknown};
};

/// @brief OpenVINO 视频帧预处理器。
/// Stretch 模式封装 NV12/I420 或 packed RGB/BGR/GRAY8，NV21 会显式转为 NV12；Letterbox
/// 模式通过 swscale 构造目标 YUV 画布。颜色转换、模型侧 resize 和归一化由 OpenVINO
/// 内部预处理完成。NV21 Letterbox 先由 swscale 缩放 VU，再在最终画布显式写入
/// NV12 UV；packed RGB/BGR 会在自身色彩空间构造三通道 Letterbox 画布，GRAY8 仍仅
/// 支持 Stretch，不能静默套用 YUV 或三通道 Letterbox。
class OpenVinoFramePreprocessor : public IPreprocessor {
public:
    /// @brief 释放仅由 Letterbox 路径缓存的 swscale 上下文。
    ///
    /// SwsContext 不能由默认析构释放；显式析构可保证模型重建或 Session 销毁时
    /// 不遗留 FFmpeg 的缩放资源。
    ~OpenVinoFramePreprocessor() override;

    /// @brief 初始化预处理器
    bool Initialize(const OpenVinoFramePreprocessorConfig& config);

    TensorFrame Process(const MediaFrame& frame) override;

private:
    uint32_t input_height_ = 640;    ///< 模型输入高度
    uint32_t input_width_ = 640;     ///< 模型输入宽度
    ResizeMode resize_mode_{ResizeMode::kStretch}; ///< 已验证的几何变换策略
    PixelFormat pixel_format_{PixelFormat::kUnknown}; ///< 模型输入像素格式
    /// @brief Letterbox 的 swscale 缓存；仅在 Letterbox 模式下创建和使用。
    SwsContext* swscale_context_{nullptr};
    /// @brief 保护 swscale 上下文的创建、复用和销毁，避免并发 Process 复配上下文。
    std::mutex swscale_mutex_;
};

/// @brief 兼容已完成 NV12/I420/NV21 接入时使用的 YUV 配置别名。
///
/// 新名称覆盖 packed RGB/BGR，因此后续新模型应优先使用
/// `OpenVinoFramePreprocessorConfig`；别名让已有调用方无需随能力扩展修改。
using OpenVinoYuvPreprocessorConfig = OpenVinoFramePreprocessorConfig;

/// @brief 兼容已完成 NV12/I420/NV21 接入时使用的 YUV 预处理器别名。
using OpenVinoYuvPreprocessor = OpenVinoFramePreprocessor;

/// @brief 兼容早期 YOLO 调用方的旧配置别名。
using OpenVinoYoloPreprocessorConfig = OpenVinoFramePreprocessorConfig;

/// @brief 兼容早期 YOLO 调用方的旧预处理器别名。
using OpenVinoYoloPreprocessor = OpenVinoFramePreprocessor;
