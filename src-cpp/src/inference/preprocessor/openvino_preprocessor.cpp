#include "preprocessor/openvino_preprocessor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "inference/inference_logger.h"

namespace {
/// @brief 媒体帧平面张量缓冲区（适配多种格式）
class MediaFramePlaneTensorBuffer : public TensorBuffer {
public:
    MediaFramePlaneTensorBuffer(std::shared_ptr<IMediaBuffer> media_buffer,
                                size_t offset,
                                size_t bytes)
        : media_buffer_(std::move(media_buffer)),
          offset_(offset),
          bytes_(bytes) {
    }

    void* Data() override {
        return media_buffer_ ? media_buffer_->Data() + offset_ : nullptr;
    }

    const void* Data() const override {
        return media_buffer_ ? media_buffer_->Data() + offset_ : nullptr;
    }

    size_t Size() const override {
        return bytes_;
    }

private:
    std::shared_ptr<IMediaBuffer> media_buffer_;
    size_t offset_{0};
    size_t bytes_{0};
};

/// @brief 获取图像平面的偏移量
/// @param frame 输入的原始 MediaFrame（包含 buffer、尺寸、stride 等信息）
/// @param index 平面索引
/// @param fallback 默认值
/// @return 平面数据在 buffer 中的偏移量
size_t PlaneOffset(const MediaFrame& frame, int index, size_t fallback) {
    if (frame.PlaneCount() > index && frame.PlaneOffset(index) > 0) {
        return static_cast<size_t>(frame.PlaneOffset(index));
    }
    return fallback;
}

/// @brief 获取图像平面的跨度
/// @param frame 输入的原始 MediaFrame（包含 buffer、尺寸、stride 等信息）
/// @param index 平面索引
/// @param fallback 默认值
int32_t PlaneStride(const MediaFrame& frame, int index, int32_t fallback) {
    if (frame.Stride(index) > 0) {
        return frame.Stride(index);
    }
    return fallback;
}

/// @brief 根据输入图像平面参数，安全地指向或创建一个 TensorBuffer。
/// 上层提供 offset、stride 等信息，避免使用 MediaFrame 中不完整的元数据。
/// 从 buffer 的 offset 位置取得 rows 行数据；每行有效长度为 row_bytes，实际步长为 stride_bytes。
/// 无 padding 时零拷贝共享，有 padding 时逐行去除 padding 后拷贝。
/// @param frame 输入的原始 MediaFrame（包含 buffer、尺寸、stride 等信息）
/// @param offset 当前平面数据在 buffer 中的偏移量
/// @param rows 当前平面的行数
/// @param row_bytes 当前平面每行的字节数
/// @param stride_bytes 当前平面的行跨度（字节数）
/// @return 创建的 TensorBuffer 指针；参数无效时返回 nullptr
std::shared_ptr<TensorBuffer> MakePlaneBuffer(const MediaFrame& frame,
                                              size_t offset,
                                              size_t rows,
                                              size_t row_bytes,
                                              size_t stride_bytes) {
    if (!frame.buffer || !frame.buffer->Data()) {
        return nullptr;
    }
    
    // 空plane
    if (rows == 0 || row_bytes == 0) {
        return std::make_shared<CpuTensorBuffer>(0);
    }
    
    if (stride_bytes < row_bytes) {
        LOG_MAIN_ERROR_AT("MediaFrame plane stride is smaller than row bytes, row_bytes={}, stride={}",
                          row_bytes, stride_bytes);
        return nullptr;
    }

    const auto buffer_size = frame.buffer->Size();
    // 使用减法和除法验证最后一行边界，不能依赖 offset + rows * stride：来自
    // 解码器或外部调用方的 stride/offset 可能足够大，使 size_t 加乘法回绕。
    if (offset > buffer_size || row_bytes > buffer_size - offset) {
        LOG_MAIN_ERROR_AT("MediaFrame plane is out of buffer range, offset={}, rows={}, row_bytes={}, stride={}, size={}",
                          offset,
                          rows,
                          row_bytes,
                          stride_bytes,
                          buffer_size);
        return nullptr;
    }
    const auto remaining_after_first_row = buffer_size - offset - row_bytes;
    if (rows > 1 && rows - 1 > remaining_after_first_row / stride_bytes) {
        LOG_MAIN_ERROR_AT("MediaFrame plane rows exceed buffer range, offset={}, rows={}, row_bytes={}, stride={}, size={}",
                          offset,
                          rows,
                          row_bytes,
                          stride_bytes,
                          buffer_size);
        return nullptr;
    }
    if (rows > std::numeric_limits<size_t>::max() / row_bytes) {
        LOG_MAIN_ERROR_AT("MediaFrame plane compact size overflows, rows={}, row_bytes={}",
                          rows,
                          row_bytes);
        return nullptr;
    }

    // 行跨度等于行字节数，创建对应的MediaFramePlaneTensorBuffer
    if (stride_bytes == row_bytes) {
        return std::make_shared<MediaFramePlaneTensorBuffer>(frame.buffer, offset, rows * row_bytes);
    }
    
    // 行跨度不等于行字节数时，分配紧凑的新内存，大小为 rows × row_bytes（不含 padding）。
    auto compact = std::make_shared<CpuTensorBuffer>(rows * row_bytes);
    const auto* src = frame.buffer->Data() + offset;
    auto* dst = static_cast<uint8_t*>(compact->Data());
    for (size_t row = 0; row < rows; ++row) {
        // 只拷贝每一行有效的数据
        std::memcpy(dst + row * row_bytes, src + row * stride_bytes, row_bytes);
    }
    return compact;
}

/// @brief 创建一个 TensorPlane 实例，用于存储图像平面数据
std::unique_ptr<TensorPlane> MakeTensor(const std::string& name,
                                       TensorShape shape,
                                       std::shared_ptr<TensorBuffer> buffer) {
    if (!buffer) {
        return nullptr;
    }

    auto tensor = std::make_unique<TensorPlane>();
    tensor->name = name;
    tensor->type = TensorType::UINT8;
    tensor->shape = std::move(shape);
    tensor->SetBuffer(std::move(buffer), TensorMemoryType::OPENVINO_CPU);
    return tensor;
}

bool IsYuv420Format(PixelFormat format) {
    return format == PixelFormat::kNV12 || format == PixelFormat::kNV21 ||
           format == PixelFormat::kI420;
}

bool ValidateFrame(const MediaFrame& frame) {
    if (!frame.buffer || !frame.buffer->Data() || frame.buffer->Size() == 0) {
        LOG_MAIN_ERROR_AT("MediaFrame buffer is empty");
        return false;
    }
    const auto width = frame.Width();
    const auto height = frame.Height();
    if (width <= 0 || height <= 0) {
        LOG_MAIN_ERROR_AT("MediaFrame size is invalid: {}x{}", width, height);
        return false;
    }
    // 只有 4:2:0 的 YUV 格式要求宽高均为偶数；对 packed RGB/BGR 保留该限制
    // 会错误拒绝摄像头和截图常见的奇数尺寸，例如 641x481。
    if (IsYuv420Format(frame.PixelFormat()) && ((width % 2) != 0 || (height % 2) != 0)) {
        LOG_MAIN_ERROR_AT("YUV420 frame size must be even: {}x{}", width, height);
        return false;
    }
    return true;
}

TensorMeta BuildTensorMeta(const MediaFrame& frame, uint32_t input_width, uint32_t input_height) {
    TensorMeta meta;
    const auto width = frame.Width();
    const auto height = frame.Height();
    meta.src_width = width;
    meta.src_height = height;
    meta.input_width = static_cast<int>(input_width);
    meta.input_height = static_cast<int>(input_height);

    if (width > 0 && height > 0) {
        meta.letterbox.scale_x = static_cast<float>(input_width) / static_cast<float>(width);
        meta.letterbox.scale_y = static_cast<float>(input_height) / static_cast<float>(height);
    }
    meta.letterbox.pad_x = 0.0f;
    meta.letterbox.pad_y = 0.0f;
    return meta;
}

/// @brief Letterbox 后写入模型空间的实际几何参数。
///
/// YUV420 的色度平面以 2x2 像素为单位，因此缩放后的宽高和左上 padding 必须
/// 对齐到偶数像素。右侧和下侧的剩余一个偶数像素由画布本身保留，不影响坐标恢复。
struct LetterboxGeometry {
    int resized_width{0};
    int resized_height{0};
    int pad_left{0};
    int pad_top{0};
    float scale_x{1.0f};
    float scale_y{1.0f};
};

/// @brief 为 YUV420 目标画布计算等比缩放和居中 padding。
bool BuildLetterboxGeometry(int source_width,
                            int source_height,
                            int target_width,
                            int target_height,
                            LetterboxGeometry& geometry) {
    if (source_width <= 0 || source_height <= 0 || target_width <= 0 || target_height <= 0 ||
        (target_width % 2) != 0 || (target_height % 2) != 0) {
        return false;
    }

    const auto uniform_scale = std::min(static_cast<double>(target_width) / source_width,
                                        static_cast<double>(target_height) / source_height);
    // 先按比例取最接近的整数，再向下对齐到偶数，保证每个色度样本仍对应完整的 2x2 区域。
    const auto to_even = [](double value, int limit) {
        auto rounded = std::clamp(static_cast<int>(std::lround(value)), 2, limit);
        if ((rounded % 2) != 0) {
            --rounded;
        }
        return std::max(2, rounded);
    };

    geometry.resized_width = to_even(source_width * uniform_scale, target_width);
    geometry.resized_height = to_even(source_height * uniform_scale, target_height);

    // 左/上 padding 向下对齐到偶数；多出的像素留在右/下边缘，中心最多偏移一个像素。
    geometry.pad_left = ((target_width - geometry.resized_width) / 2) & ~1;
    geometry.pad_top = ((target_height - geometry.resized_height) / 2) & ~1;
    geometry.scale_x = static_cast<float>(geometry.resized_width) / source_width;
    geometry.scale_y = static_cast<float>(geometry.resized_height) / source_height;
    return true;
}

/// @brief 为 packed RGB/BGR 画布计算等比缩放和居中 padding。
///
/// packed 三通道图像没有 4:2:0 色度采样约束，源图、目标图和 ROI 可以使用任意正整数
/// 尺寸。与 YUV420 版本分开实现，避免偶数对齐规则悄悄裁掉奇数行/列或改变检测框坐标。
bool BuildPackedLetterboxGeometry(int source_width,
                                  int source_height,
                                  int target_width,
                                  int target_height,
                                  LetterboxGeometry& geometry) {
    if (source_width <= 0 || source_height <= 0 || target_width <= 0 || target_height <= 0) {
        return false;
    }

    const auto uniform_scale = std::min(static_cast<double>(target_width) / source_width,
                                        static_cast<double>(target_height) / source_height);
    const auto to_dimension = [](double value, int limit) {
        // 比例最多到目标边界，仍以 long long 接收 lround 结果，避免在极大合法尺寸
        // 上先截断到 int 造成未定义行为；随后显式夹在 [1, limit]。
        const auto rounded = static_cast<int64_t>(std::lround(value));
        return static_cast<int>(std::clamp(rounded, int64_t{1}, static_cast<int64_t>(limit)));
    };
    geometry.resized_width = to_dimension(source_width * uniform_scale, target_width);
    geometry.resized_height = to_dimension(source_height * uniform_scale, target_height);
    // 左/上保留 floor 后的 padding，额外的一个像素自然留给右/下边。模型坐标恢复
    // 只使用这里记录的左/上偏移，因而不会把非对称的最后一行/列误当成源图内容。
    geometry.pad_left = (target_width - geometry.resized_width) / 2;
    geometry.pad_top = (target_height - geometry.resized_height) / 2;
    geometry.scale_x = static_cast<float>(geometry.resized_width) / source_width;
    geometry.scale_y = static_cast<float>(geometry.resized_height) / source_height;
    return true;
}

/// @brief 可直接交给 swscale 的 YUV 平面视图。
struct YuvPlaneView {
    const uint8_t* data{nullptr};
    int stride{0};
    size_t offset{0};
};

/// @brief 校验并解析一个源平面，保留原始 stride 供 swscale 直接读取。
bool ResolvePlaneView(const MediaFrame& frame,
                      int index,
                      size_t fallback_offset,
                      size_t rows,
                      size_t row_bytes,
                      int fallback_stride,
                      YuvPlaneView& view) {
    if (!frame.buffer || !frame.buffer->Data() || frame.PlaneCount() <= index ||
        rows == 0 || row_bytes == 0 || fallback_stride <= 0) {
        return false;
    }

    const auto offset = PlaneOffset(frame, index, fallback_offset);
    const auto stride = PlaneStride(frame, index, fallback_stride);
    if (stride <= 0 || static_cast<size_t>(stride) < row_bytes) {
        LOG_MAIN_ERROR_AT("Letterbox source plane stride is invalid, plane={}, row_bytes={}, stride={}",
                          index,
                          row_bytes,
                          stride);
        return false;
    }

    const auto buffer_size = frame.buffer->Size();
    if (offset > buffer_size || row_bytes > buffer_size - offset) {
        LOG_MAIN_ERROR_AT("Letterbox source plane begins outside MediaFrame buffer, plane={}, offset={}, row_bytes={}, size={}",
                          index,
                          offset,
                          row_bytes,
                          buffer_size);
        return false;
    }

    // 使用减法和除法检查最后一行地址，避免 rows * stride 在 size_t 中回绕。
    const auto remaining_after_first_row = buffer_size - offset - row_bytes;
    if (rows > 1 && rows - 1 > remaining_after_first_row / static_cast<size_t>(stride)) {
        LOG_MAIN_ERROR_AT("Letterbox source plane exceeds MediaFrame buffer, plane={}, rows={}, row_bytes={}, stride={}, size={}",
                          index,
                          rows,
                          row_bytes,
                          stride,
                          buffer_size);
        return false;
    }

    view.data = frame.buffer->Data() + offset;
    view.stride = stride;
    view.offset = offset;
    return true;
}

/// @brief 解析 NV12、NV21 或 I420 的全部源平面，并生成 FFmpeg 源像素格式。
bool ResolveYuv420Source(const MediaFrame& frame,
                         std::array<const uint8_t*, 4>& data,
                         std::array<int, 4>& linesizes,
                         AVPixelFormat& pixel_format) {
    const auto width = frame.Width();
    const auto height = frame.Height();
    if (width <= 0 || height <= 0) {
        return false;
    }

    const auto luma_width = static_cast<size_t>(width);
    const auto luma_height = static_cast<size_t>(height);
    const auto chroma_width = luma_width / 2;
    const auto chroma_height = luma_height / 2;

    YuvPlaneView y;
    if (!ResolvePlaneView(frame, 0, 0, luma_height, luma_width, width, y)) {
        return false;
    }

    data[0] = y.data;
    linesizes[0] = y.stride;
    const auto chroma_offset = y.offset + static_cast<size_t>(y.stride) * luma_height;

    switch (frame.PixelFormat()) {
    case PixelFormat::kNV12:
    case PixelFormat::kNV21: {
        YuvPlaneView uv;
        if (!ResolvePlaneView(frame, 1, chroma_offset, chroma_height, luma_width, width, uv)) {
            return false;
        }
        data[1] = uv.data;
        linesizes[1] = uv.stride;
        // NV21 和 NV12 的第二平面分别是 VU/UV。必须把真实 source format
        // 传给 swscale，后续 letterbox 路径再统一输出 NV12，不能直接标错。
        pixel_format = frame.PixelFormat() == PixelFormat::kNV21 ? AV_PIX_FMT_NV21
                                                                      : AV_PIX_FMT_NV12;
        return true;
    }
    case PixelFormat::kI420: {
        YuvPlaneView u;
        if (!ResolvePlaneView(frame,
                              1,
                              chroma_offset,
                              chroma_height,
                              chroma_width,
                              width / 2,
                              u)) {
            return false;
        }
        YuvPlaneView v;
        const auto v_offset = u.offset + static_cast<size_t>(u.stride) * chroma_height;
        if (!ResolvePlaneView(frame,
                              2,
                              v_offset,
                              chroma_height,
                              chroma_width,
                              width / 2,
                              v)) {
            return false;
        }
        data[1] = u.data;
        data[2] = v.data;
        linesizes[1] = u.stride;
        linesizes[2] = v.stride;
        pixel_format = AV_PIX_FMT_YUV420P;
        return true;
    }
    default:
        return false;
    }
}

/// @brief 使用 swscale 将一个 YUV420 帧缩放到预填充的 Letterbox 画布中央。
///
/// 目标画布按 Y、UV 或 Y、U、V 独立分配，TensorPlane 因而能保持紧凑内存布局，
/// 可直接沿用 OpenVINO 的多平面输入绑定，不需要引入 BGR/RGB 或 OpenCV。
TensorFrame PackLetterboxYuv420(const MediaFrame& frame,
                                uint32_t input_width,
                                uint32_t input_height,
                                SwsContext*& swscale_context,
                                std::mutex& swscale_mutex) {
    if (input_width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        input_height > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        LOG_MAIN_ERROR_AT("Letterbox target size exceeds swscale integer range");
        return {};
    }

    const auto target_width = static_cast<int>(input_width);
    const auto target_height = static_cast<int>(input_height);
    LetterboxGeometry geometry;
    if (!BuildLetterboxGeometry(frame.Width(), frame.Height(), target_width, target_height, geometry)) {
        LOG_MAIN_ERROR_AT("Letterbox requires positive, even YUV420 target size: {}x{}",
                          target_width,
                          target_height);
        return {};
    }

    std::array<const uint8_t*, 4> source_data{};
    std::array<int, 4> source_linesizes{};
    AVPixelFormat source_format = AV_PIX_FMT_NONE;
    if (!ResolveYuv420Source(frame, source_data, source_linesizes, source_format)) {
        LOG_MAIN_ERROR_AT("Letterbox source planes are invalid or unsupported");
        return {};
    }

    const auto target_width_size = static_cast<size_t>(target_width);
    const auto target_height_size = static_cast<size_t>(target_height);
    if (target_width_size > std::numeric_limits<size_t>::max() / target_height_size) {
        LOG_MAIN_ERROR_AT("Letterbox target buffer size overflow");
        return {};
    }
    const auto target_luma_bytes = target_width_size * target_height_size;

    // 先为 swscale 分配紧凑的缩放结果，再拷贝到最终画布 ROI。
    // 这样 swscale 始终只面对从第 0 行开始的完整平面，避免不同像素格式对
    // 目标指针偏移和色度行数的解释差异破坏画布边界。
    const auto resized_width_size = static_cast<size_t>(geometry.resized_width);
    const auto resized_height_size = static_cast<size_t>(geometry.resized_height);
    if (resized_width_size > std::numeric_limits<size_t>::max() / resized_height_size) {
        LOG_MAIN_ERROR_AT("Letterbox resized buffer size overflow");
        return {};
    }
    const auto resized_luma_bytes = resized_width_size * resized_height_size;
    // swscale 的 SIMD 输出路径可能在有效像素之后访问少量对齐字节。最终 Tensor
    // 仍保持精确尺寸；仅供 swscale 写入的中间平面额外保留保护区，随后只逐行
    // 复制有效区域到画布，避免对最终 TensorBuffer 的尾部造成越界写入。
    constexpr size_t kSwscaleGuardBytes = 64;

    // YOLO 常用 114 作为无色中性 padding；U/V 取 128，确保边框不引入色相。
    constexpr uint8_t kLetterboxLuma = 114;
    constexpr uint8_t kNeutralChroma = 128;
    auto y_canvas = std::make_shared<CpuTensorBuffer>(target_luma_bytes);
    std::fill_n(static_cast<uint8_t*>(y_canvas->Data()), target_luma_bytes, kLetterboxLuma);
    auto y_resized = std::make_shared<CpuTensorBuffer>(resized_luma_bytes + kSwscaleGuardBytes);

    std::array<uint8_t*, 4> resized_data{};
    std::array<int, 4> resized_linesizes{};
    resized_data[0] = static_cast<uint8_t*>(y_resized->Data());
    resized_linesizes[0] = geometry.resized_width;

    TensorFrame tensor_frame;
    tensor_frame.AddTensor(MakeTensor("y",
                                      TensorShape{{1, target_height, target_width, 1}},
                                      y_canvas));

    const auto is_two_plane_source = source_format == AV_PIX_FMT_NV12 ||
                                     source_format == AV_PIX_FMT_NV21;
    // 部分 swscale 版本在 NV21 缩放时即使请求 NV12 output 仍保留 VU 顺序；不让
    // provider 版本决定颜色语义。这里固定以 source format 生成缩放中间平面，
    // 随后写入最终画布时对 NV21 显式交换每对色度字节，最终 y/uv 一律是 NV12。
    const auto swscale_output_format = source_format;
    const auto needs_nv21_to_nv12_swap = source_format == AV_PIX_FMT_NV21;
    if (is_two_plane_source) {
        auto uv_canvas = std::make_shared<CpuTensorBuffer>(target_luma_bytes / 2);
        std::fill_n(static_cast<uint8_t*>(uv_canvas->Data()), target_luma_bytes / 2, kNeutralChroma);
        auto uv_resized = std::make_shared<CpuTensorBuffer>(resized_luma_bytes / 2 + kSwscaleGuardBytes);
        resized_data[1] = static_cast<uint8_t*>(uv_resized->Data());
        resized_linesizes[1] = geometry.resized_width;
        tensor_frame.AddTensor(MakeTensor("uv",
                                          TensorShape{{1, target_height / 2, target_width / 2, 2}},
                                          uv_canvas));

        {
            std::lock_guard<std::mutex> lock(swscale_mutex);
            swscale_context = sws_getCachedContext(swscale_context,
                                                   frame.Width(),
                                                   frame.Height(),
                                                   swscale_output_format,
                                                   geometry.resized_width,
                                                   geometry.resized_height,
                                                   swscale_output_format,
                                                   SWS_BILINEAR,
                                                   nullptr,
                                                   nullptr,
                                                   nullptr);
            if (!swscale_context) {
                LOG_MAIN_ERROR_AT("Failed to create swscale context for Letterbox");
                return {};
            }
            const auto written_rows = sws_scale(swscale_context,
                                                source_data.data(),
                                                source_linesizes.data(),
                                                0,
                                                frame.Height(),
                                                resized_data.data(),
                                                resized_linesizes.data());
            if (written_rows != geometry.resized_height) {
                LOG_MAIN_ERROR_AT("Letterbox swscale wrote unexpected row count: expected={}, actual={}",
                                  geometry.resized_height,
                                  written_rows);
                return {};
            }
        }

        auto* y_dst = static_cast<uint8_t*>(y_canvas->Data()) +
                      static_cast<size_t>(geometry.pad_top) * target_width + geometry.pad_left;
        auto* uv_dst = static_cast<uint8_t*>(uv_canvas->Data()) +
                       static_cast<size_t>(geometry.pad_top / 2) * target_width + geometry.pad_left;
        const auto* y_src = static_cast<const uint8_t*>(y_resized->Data());
        const auto* uv_src = static_cast<const uint8_t*>(uv_resized->Data());
        for (int row = 0; row < geometry.resized_height; ++row) {
            std::memcpy(y_dst + static_cast<size_t>(row) * target_width,
                        y_src + static_cast<size_t>(row) * geometry.resized_width,
                        static_cast<size_t>(geometry.resized_width));
        }
        for (int row = 0; row < geometry.resized_height / 2; ++row) {
            auto* destination = uv_dst + static_cast<size_t>(row) * target_width;
            const auto* source = uv_src + static_cast<size_t>(row) * geometry.resized_width;
            if (!needs_nv21_to_nv12_swap) {
                std::memcpy(destination, source, static_cast<size_t>(geometry.resized_width));
                continue;
            }

            // NV21 色度行的相邻两个字节是 V,U；OpenVINO `NV12_TWO_PLANES`
            // 需要 U,V。几何缩放已完成，逐对交换不会影响色度采样位置。
            for (int column = 0; column < geometry.resized_width; column += 2) {
                destination[column] = source[column + 1];
                destination[column + 1] = source[column];
            }
        }
    } else {
        const auto chroma_bytes = target_luma_bytes / 4;
        auto u_canvas = std::make_shared<CpuTensorBuffer>(chroma_bytes);
        auto v_canvas = std::make_shared<CpuTensorBuffer>(chroma_bytes);
        std::fill_n(static_cast<uint8_t*>(u_canvas->Data()), chroma_bytes, kNeutralChroma);
        std::fill_n(static_cast<uint8_t*>(v_canvas->Data()), chroma_bytes, kNeutralChroma);
        auto u_resized = std::make_shared<CpuTensorBuffer>(resized_luma_bytes / 4 + kSwscaleGuardBytes);
        auto v_resized = std::make_shared<CpuTensorBuffer>(resized_luma_bytes / 4 + kSwscaleGuardBytes);
        const auto chroma_stride = target_width / 2;
        resized_data[1] = static_cast<uint8_t*>(u_resized->Data());
        resized_data[2] = static_cast<uint8_t*>(v_resized->Data());
        resized_linesizes[1] = geometry.resized_width / 2;
        resized_linesizes[2] = geometry.resized_width / 2;
        tensor_frame.AddTensor(MakeTensor("u",
                                          TensorShape{{1, target_height / 2, target_width / 2, 1}},
                                          u_canvas));
        tensor_frame.AddTensor(MakeTensor("v",
                                          TensorShape{{1, target_height / 2, target_width / 2, 1}},
                                          v_canvas));

        {
            std::lock_guard<std::mutex> lock(swscale_mutex);
            swscale_context = sws_getCachedContext(swscale_context,
                                                   frame.Width(),
                                                   frame.Height(),
                                                   source_format,
                                                   geometry.resized_width,
                                                   geometry.resized_height,
                                                   source_format,
                                                   SWS_BILINEAR,
                                                   nullptr,
                                                   nullptr,
                                                   nullptr);
            if (!swscale_context) {
                LOG_MAIN_ERROR_AT("Failed to create swscale context for Letterbox");
                return {};
            }
            const auto written_rows = sws_scale(swscale_context,
                                                source_data.data(),
                                                source_linesizes.data(),
                                                0,
                                                frame.Height(),
                                                resized_data.data(),
                                                resized_linesizes.data());
            if (written_rows != geometry.resized_height) {
                LOG_MAIN_ERROR_AT("Letterbox swscale wrote unexpected row count: expected={}, actual={}",
                                  geometry.resized_height,
                                  written_rows);
                return {};
            }
        }

        auto* y_dst = static_cast<uint8_t*>(y_canvas->Data()) +
                      static_cast<size_t>(geometry.pad_top) * target_width + geometry.pad_left;
        const auto chroma_offset = static_cast<size_t>(geometry.pad_top / 2) * chroma_stride +
                                       geometry.pad_left / 2;
        auto* u_dst = static_cast<uint8_t*>(u_canvas->Data()) + chroma_offset;
        auto* v_dst = static_cast<uint8_t*>(v_canvas->Data()) + chroma_offset;
        const auto* y_src = static_cast<const uint8_t*>(y_resized->Data());
        const auto* u_src = static_cast<const uint8_t*>(u_resized->Data());
        const auto* v_src = static_cast<const uint8_t*>(v_resized->Data());
        for (int row = 0; row < geometry.resized_height; ++row) {
            std::memcpy(y_dst + static_cast<size_t>(row) * target_width,
                        y_src + static_cast<size_t>(row) * geometry.resized_width,
                        static_cast<size_t>(geometry.resized_width));
        }
        for (int row = 0; row < geometry.resized_height / 2; ++row) {
            std::memcpy(u_dst + static_cast<size_t>(row) * chroma_stride,
                        u_src + static_cast<size_t>(row) * (geometry.resized_width / 2),
                        static_cast<size_t>(geometry.resized_width / 2));
            std::memcpy(v_dst + static_cast<size_t>(row) * chroma_stride,
                        v_src + static_cast<size_t>(row) * (geometry.resized_width / 2),
                        static_cast<size_t>(geometry.resized_width / 2));
        }
    }

    tensor_frame.tensor_meta_ = BuildTensorMeta(frame, input_width, input_height);
    tensor_frame.tensor_meta_.letterbox.scale_x = geometry.scale_x;
    tensor_frame.tensor_meta_.letterbox.scale_y = geometry.scale_y;
    tensor_frame.tensor_meta_.letterbox.pad_x = static_cast<float>(geometry.pad_left);
    tensor_frame.tensor_meta_.letterbox.pad_y = static_cast<float>(geometry.pad_top);
    return tensor_frame;
}

/// @brief 将 packed RGB24/BGR24 缩放并写入同色彩空间的 Letterbox 三通道画布。
///
/// OpenVINO 的 packed 输入约定是一个 `image` NHWC tensor，而非 YUV 的 `y/u/v` 平面。
/// 因此 swscale 的 source/destination 格式保持完全相同，只负责几何缩放；RGB/BGR
/// 通道顺序与模型颜色空间转换仍由 engine 的 OpenVINO preprocess 显式解释。
TensorFrame PackLetterboxPackedColor(const MediaFrame& frame,
                                     uint32_t input_width,
                                     uint32_t input_height,
                                     SwsContext*& swscale_context,
                                     std::mutex& swscale_mutex) {
    if (input_width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        input_height > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        LOG_MAIN_ERROR_AT("Packed letterbox target size exceeds swscale integer range");
        return {};
    }

    const auto target_width = static_cast<int>(input_width);
    const auto target_height = static_cast<int>(input_height);
    LetterboxGeometry geometry;
    if (!BuildPackedLetterboxGeometry(frame.Width(),
                                      frame.Height(),
                                      target_width,
                                      target_height,
                                      geometry)) {
        LOG_MAIN_ERROR_AT("Packed letterbox requires positive target size: {}x{}",
                          target_width,
                          target_height);
        return {};
    }

    constexpr size_t kChannels = 3;
    const auto source_width = static_cast<size_t>(frame.Width());
    const auto source_height = static_cast<size_t>(frame.Height());
    if (source_width > std::numeric_limits<size_t>::max() / kChannels ||
        source_width * kChannels > static_cast<size_t>(std::numeric_limits<int>::max())) {
        LOG_MAIN_ERROR_AT("Packed letterbox source row byte count is unsupported, width={}",
                          frame.Width());
        return {};
    }
    const auto source_row_bytes = source_width * kChannels;
    YuvPlaneView source;
    if (!ResolvePlaneView(frame,
                          0,
                          0,
                          source_height,
                          source_row_bytes,
                          static_cast<int>(source_row_bytes),
                          source)) {
        LOG_MAIN_ERROR_AT("Packed letterbox source plane is invalid");
        return {};
    }

    const auto source_format = frame.PixelFormat() == PixelFormat::kRGB24 ? AV_PIX_FMT_RGB24
                                                                               : AV_PIX_FMT_BGR24;
    const auto target_width_size = static_cast<size_t>(target_width);
    const auto target_height_size = static_cast<size_t>(target_height);
    if (target_width_size > std::numeric_limits<size_t>::max() / target_height_size) {
        LOG_MAIN_ERROR_AT("Packed letterbox target pixel count overflows");
        return {};
    }
    const auto target_pixels = target_width_size * target_height_size;
    if (target_pixels > std::numeric_limits<size_t>::max() / kChannels) {
        LOG_MAIN_ERROR_AT("Packed letterbox target byte count overflows");
        return {};
    }
    const auto target_bytes = target_pixels * kChannels;

    const auto resized_width_size = static_cast<size_t>(geometry.resized_width);
    const auto resized_height_size = static_cast<size_t>(geometry.resized_height);
    if (resized_width_size > std::numeric_limits<size_t>::max() / resized_height_size) {
        LOG_MAIN_ERROR_AT("Packed letterbox resized pixel count overflows");
        return {};
    }
    const auto resized_pixels = resized_width_size * resized_height_size;
    if (resized_pixels > std::numeric_limits<size_t>::max() / kChannels ||
        resized_width_size > static_cast<size_t>(std::numeric_limits<int>::max()) / kChannels) {
        LOG_MAIN_ERROR_AT("Packed letterbox resized byte count or stride overflows");
        return {};
    }
    const auto resized_row_bytes = resized_width_size * kChannels;
    const auto resized_bytes = resized_pixels * kChannels;
    // swscale 的 SIMD 路径可能访问有效行末尾后的少量对齐字节。临时缩放 buffer
    // 额外保留保护区，最终 `image` tensor 仍只保存严格的 target_bytes。
    constexpr size_t kSwscaleGuardBytes = 64;
    if (resized_bytes > std::numeric_limits<size_t>::max() - kSwscaleGuardBytes) {
        LOG_MAIN_ERROR_AT("Packed letterbox resize guard byte count overflows");
        return {};
    }

    // 检测模型常用 (114,114,114) 作为灰色边框。三个通道写入同一数值，使 RGB/BGR
    // 在本层共享相同的边框语义；此处不交换颜色字节。
    constexpr uint8_t kLetterboxChannel = 114;
    auto canvas = std::make_shared<CpuTensorBuffer>(target_bytes);
    std::fill_n(static_cast<uint8_t*>(canvas->Data()), target_bytes, kLetterboxChannel);
    auto resized = std::make_shared<CpuTensorBuffer>(resized_bytes + kSwscaleGuardBytes);

    std::array<const uint8_t*, 4> source_data{};
    std::array<int, 4> source_linesizes{};
    std::array<uint8_t*, 4> resized_data{};
    std::array<int, 4> resized_linesizes{};
    source_data[0] = source.data;
    source_linesizes[0] = source.stride;
    resized_data[0] = static_cast<uint8_t*>(resized->Data());
    resized_linesizes[0] = static_cast<int>(resized_row_bytes);
    {
        std::lock_guard<std::mutex> lock(swscale_mutex);
        swscale_context = sws_getCachedContext(swscale_context,
                                               frame.Width(),
                                               frame.Height(),
                                               source_format,
                                               geometry.resized_width,
                                               geometry.resized_height,
                                               source_format,
                                               SWS_BILINEAR,
                                               nullptr,
                                               nullptr,
                                               nullptr);
        if (!swscale_context) {
            LOG_MAIN_ERROR_AT("Failed to create swscale context for packed Letterbox");
            return {};
        }
        const auto written_rows = sws_scale(swscale_context,
                                            source_data.data(),
                                            source_linesizes.data(),
                                            0,
                                            frame.Height(),
                                            resized_data.data(),
                                            resized_linesizes.data());
        if (written_rows != geometry.resized_height) {
            LOG_MAIN_ERROR_AT("Packed letterbox swscale wrote unexpected row count: expected={}, actual={}",
                              geometry.resized_height,
                              written_rows);
            return {};
        }
    }

    auto* destination = static_cast<uint8_t*>(canvas->Data()) +
                        (static_cast<size_t>(geometry.pad_top) * target_width_size +
                         static_cast<size_t>(geometry.pad_left)) *
                            kChannels;
    const auto* source_pixels = static_cast<const uint8_t*>(resized->Data());
    for (int row = 0; row < geometry.resized_height; ++row) {
        std::memcpy(destination + static_cast<size_t>(row) * target_width_size * kChannels,
                    source_pixels + static_cast<size_t>(row) * resized_row_bytes,
                    resized_row_bytes);
    }

    TensorFrame tensor_frame;
    tensor_frame.AddTensor(MakeTensor("image",
                                      TensorShape{{1, target_height, target_width, 3}},
                                      std::move(canvas)));
    tensor_frame.tensor_meta_ = BuildTensorMeta(frame, input_width, input_height);
    tensor_frame.tensor_meta_.letterbox.scale_x = geometry.scale_x;
    tensor_frame.tensor_meta_.letterbox.scale_y = geometry.scale_y;
    tensor_frame.tensor_meta_.letterbox.pad_x = static_cast<float>(geometry.pad_left);
    tensor_frame.tensor_meta_.letterbox.pad_y = static_cast<float>(geometry.pad_top);
    return tensor_frame;
}

TensorFrame PackNv12(const MediaFrame& frame) {
    TensorFrame tensor_frame;

    const auto frame_width = frame.Width();
    const auto frame_height = frame.Height();
    const auto width = static_cast<size_t>(frame_width);
    const auto height = static_cast<size_t>(frame_height);
    const auto y_stride = static_cast<size_t>(PlaneStride(frame, 0, frame_width));
    const auto uv_stride = static_cast<size_t>(PlaneStride(frame, 1, frame_width));
    const auto y_offset = PlaneOffset(frame, 0, 0);
    const auto uv_offset = PlaneOffset(frame, 1, y_stride * height);

    auto y = MakeTensor("y",
                        TensorShape{{1, frame_height, frame_width, 1}},
                        MakePlaneBuffer(frame, y_offset, height, width, y_stride));
    auto uv = MakeTensor("uv",
                         TensorShape{{1, frame_height / 2, frame_width / 2, 2}},
                         MakePlaneBuffer(frame, uv_offset, height / 2, width, uv_stride));

    if (y) {
        tensor_frame.AddTensor(std::move(y));
    }
    if (uv) {
        tensor_frame.AddTensor(std::move(uv));
    }
    return tensor_frame;
}

/// @brief 将 NV21 的 VU 交错色度显式转换为 OpenVINO 可接受的 NV12 UV 平面。
///
/// OpenVINO PrePostProcessor 当前只提供 `NV12_TWO_PLANES`，没有 NV21 的颜色
/// 枚举。直接把 NV21 命名为 uv 会使红蓝通道交换，因此本函数保留原始 Y 平面，
/// 仅为色度分配紧凑副本并逐像素对调 VU -> UV。无 padding 时这也是 NV21 路径
/// 唯一的额外内存复制；有 stride 时 `MakePlaneBuffer` 先安全压紧有效列。
TensorFrame PackNv21AsNv12(const MediaFrame& frame) {
    TensorFrame tensor_frame;

    const auto frame_width = frame.Width();
    const auto frame_height = frame.Height();
    const auto width = static_cast<size_t>(frame_width);
    const auto height = static_cast<size_t>(frame_height);
    const auto y_stride = static_cast<size_t>(PlaneStride(frame, 0, frame_width));
    const auto vu_stride = static_cast<size_t>(PlaneStride(frame, 1, frame_width));
    const auto y_offset = PlaneOffset(frame, 0, 0);
    const auto vu_offset = PlaneOffset(frame, 1, y_stride * height);

    auto y_buffer = MakePlaneBuffer(frame, y_offset, height, width, y_stride);
    auto vu_buffer = MakePlaneBuffer(frame, vu_offset, height / 2, width, vu_stride);
    if (!y_buffer || !vu_buffer) {
        return tensor_frame;
    }

    // NV21/NV12 的每行色度字节数都等于 luma 宽度；YUV420 已由 ValidateFrame
    // 保证宽高为偶数，因此每对 VU 字节都恰好能交换为一个 UV 采样。
    const auto chroma_bytes = width * height / 2;
    auto uv_buffer = std::make_shared<CpuTensorBuffer>(chroma_bytes);
    const auto* vu = static_cast<const uint8_t*>(vu_buffer->Data());
    auto* uv = static_cast<uint8_t*>(uv_buffer->Data());
    if (!vu || !uv) {
        return {};
    }
    for (size_t offset = 0; offset < chroma_bytes; offset += 2) {
        uv[offset] = vu[offset + 1];
        uv[offset + 1] = vu[offset];
    }

    auto y = MakeTensor("y", TensorShape{{1, frame_height, frame_width, 1}}, std::move(y_buffer));
    auto uv_tensor = MakeTensor("uv",
                                TensorShape{{1, frame_height / 2, frame_width / 2, 2}},
                                std::move(uv_buffer));
    if (y) {
        tensor_frame.AddTensor(std::move(y));
    }
    if (uv_tensor) {
        tensor_frame.AddTensor(std::move(uv_tensor));
    }
    return tensor_frame;
}

/// @brief 将一帧 packed RGB24 或 BGR24 作为单个 NHWC `image` tensor 封装。
///
/// 该函数不交换任何通道：`PixelFormat` 与 engine 的 OpenVINO preprocess 配置共同
/// 决定数据的颜色语义。这里唯一允许的复制来自去除每行 padding；紧凑帧仍通过
/// `MediaFramePlaneTensorBuffer` 零拷贝持有原始 buffer。
TensorFrame PackRgb24(const MediaFrame& frame) {
    TensorFrame tensor_frame;
    const auto frame_width = frame.Width();
    const auto frame_height = frame.Height();
    const auto width = static_cast<size_t>(frame_width);
    const auto height = static_cast<size_t>(frame_height);
    constexpr size_t kChannels = 3;
    if (width > std::numeric_limits<size_t>::max() / kChannels) {
        LOG_MAIN_ERROR_AT("Packed RGB frame row byte count overflows, width={}", frame_width);
        return tensor_frame;
    }
    const auto row_bytes = width * kChannels;
    if (row_bytes > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        LOG_MAIN_ERROR_AT("Packed RGB frame stride cannot fit int32: {}", row_bytes);
        return tensor_frame;
    }
    const auto stride = PlaneStride(frame, 0, static_cast<int32_t>(row_bytes));
    if (stride <= 0) {
        LOG_MAIN_ERROR_AT("Packed RGB frame stride is invalid: {}", stride);
        return tensor_frame;
    }
    const auto offset = PlaneOffset(frame, 0, 0);
    auto image_buffer = MakePlaneBuffer(frame,
                                        offset,
                                        height,
                                        row_bytes,
                                        static_cast<size_t>(stride));
    auto image = MakeTensor("image",
                            TensorShape{{1, frame_height, frame_width, static_cast<int64_t>(kChannels)}},
                            std::move(image_buffer));
    if (image) {
        tensor_frame.AddTensor(std::move(image));
    }
    return tensor_frame;
}

/// @brief 将一帧 packed GRAY8 作为单通道 NHWC `image` tensor 封装。
///
/// GRAY8 的每个像素只有一个字节，形状固定为 `[1,H,W,1]`。和 RGB/BGR 路径一样，
/// 紧凑帧直接持有 MediaFrame buffer；带 stride 的帧仅复制有效像素而丢弃每行尾部
/// padding。当前 factory 仅允许 GRAY8 -> GRAY 同色空间，不能在本层或 provider
/// 中假设可扩展为 RGB/BGR；本层绝不在 CPU 端伪造三通道数据。
TensorFrame PackGray8(const MediaFrame& frame) {
    TensorFrame tensor_frame;
    const auto frame_width = frame.Width();
    const auto frame_height = frame.Height();
    const auto width = static_cast<size_t>(frame_width);
    const auto height = static_cast<size_t>(frame_height);
    const auto stride = PlaneStride(frame, 0, frame_width);
    if (stride <= 0) {
        LOG_MAIN_ERROR_AT("Packed GRAY8 frame stride is invalid: {}", stride);
        return tensor_frame;
    }

    const auto offset = PlaneOffset(frame, 0, 0);
    auto image_buffer = MakePlaneBuffer(frame,
                                        offset,
                                        height,
                                        width,
                                        static_cast<size_t>(stride));
    auto image = MakeTensor("image",
                            TensorShape{{1, frame_height, frame_width, 1}},
                            std::move(image_buffer));
    if (image) {
        tensor_frame.AddTensor(std::move(image));
    }
    return tensor_frame;
}

TensorFrame PackI420(const MediaFrame& frame) {
    TensorFrame tensor_frame;

    const auto frame_width = frame.Width();
    const auto frame_height = frame.Height();
    const auto width = static_cast<size_t>(frame_width);
    const auto height = static_cast<size_t>(frame_height);
    const auto half_width = width / 2;
    const auto half_height = height / 2;
    const auto y_stride = static_cast<size_t>(PlaneStride(frame, 0, frame_width));
    const auto u_stride = static_cast<size_t>(PlaneStride(frame, 1, frame_width / 2));
    const auto v_stride = static_cast<size_t>(PlaneStride(frame, 2, frame_width / 2));
    const auto y_offset = PlaneOffset(frame, 0, 0);
    const auto u_offset = PlaneOffset(frame, 1, y_stride * height);
    const auto v_offset = PlaneOffset(frame, 2, u_offset + u_stride * half_height);

    auto y = MakeTensor("y", TensorShape{{1, frame_height, frame_width, 1}},
                        MakePlaneBuffer(frame, y_offset, height, width, y_stride));
    auto u = MakeTensor("u", TensorShape{{1, frame_height / 2, frame_width / 2, 1}},
                        MakePlaneBuffer(frame, u_offset, half_height, half_width, u_stride));
    auto v = MakeTensor("v", TensorShape{{1, frame_height / 2, frame_width / 2, 1}},
                        MakePlaneBuffer(frame, v_offset, half_height, half_width, v_stride));

    if (y) {
        tensor_frame.AddTensor(std::move(y));
    }
    if (u) {
        tensor_frame.AddTensor(std::move(u));
    }
    if (v) {
        tensor_frame.AddTensor(std::move(v));
    }
    return tensor_frame;
}

}  // namespace

OpenVinoFramePreprocessor::~OpenVinoFramePreprocessor() {
    std::lock_guard<std::mutex> lock(swscale_mutex_);
    sws_freeContext(swscale_context_);
    swscale_context_ = nullptr;
}

bool OpenVinoFramePreprocessor::Initialize(const OpenVinoFramePreprocessorConfig& config) {
    if (config.input_width == 0 || config.input_height == 0 ||
        config.input_width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        config.input_height > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        LOG_MAIN_ERROR_AT("OpenVINO preprocessor input size is invalid: {}x{}", config.input_width, config.input_height);
        return false;
    }
    if (config.resize_mode == ResizeMode::kCenterCrop) {
        LOG_MAIN_ERROR_AT("OpenVINO preprocessor center-crop mode is not implemented yet");
        return false;
    }
    if (config.resize_mode == ResizeMode::kLetterbox && IsYuv420Format(config.pixel_format) &&
        ((config.input_width % 2) != 0 || (config.input_height % 2) != 0)) {
        // Letterbox 目标本身也是 YUV420 画布；奇数边会让 UV 平面无法与 Y 平面一一对应。
        LOG_MAIN_ERROR_AT("OpenVINO letterbox target size must be even: {}x{}",
                          config.input_width,
                          config.input_height);
        return false;
    }

    {
        // 重新初始化可能切换输入尺寸或模式；旧缓存不能继续带入新的缩放参数。
        std::lock_guard<std::mutex> lock(swscale_mutex_);
        sws_freeContext(swscale_context_);
        swscale_context_ = nullptr;
    }

    input_width_ = config.input_width;
    input_height_ = config.input_height;
    resize_mode_ = config.resize_mode;
    pixel_format_ = config.pixel_format;
    return true;
}

TensorFrame OpenVinoFramePreprocessor::Process(const MediaFrame& frame) {
    if (!ValidateFrame(frame)) {
        return {};
    }

    const auto frame_pixel_format = frame.PixelFormat();
    if (pixel_format_ != PixelFormat::kUnknown && frame_pixel_format != pixel_format_) {
        LOG_MAIN_WARN_AT("MediaFrame pixel format does not match preprocessor config");
    }

    if (resize_mode_ == ResizeMode::kLetterbox) {
        if (frame_pixel_format == PixelFormat::kRGB24 || frame_pixel_format == PixelFormat::kBGR24) {
            return PackLetterboxPackedColor(frame,
                                             input_width_,
                                             input_height_,
                                             swscale_context_,
                                             swscale_mutex_);
        }
        if (frame_pixel_format == PixelFormat::kGRAY8) {
            // 单通道画布需要独立定义填充值和 GRAY 模型的 resize/归一化契约。它不能
            // 复用三通道 114 边框，也不能落入 YUV420 多平面路径，故继续明确拒绝。
            LOG_MAIN_ERROR_AT("GRAY8 letterbox is not implemented; use stretch");
            return {};
        }
        if (IsYuv420Format(frame_pixel_format)) {
            return PackLetterboxYuv420(frame,
                                       input_width_,
                                       input_height_,
                                       swscale_context_,
                                       swscale_mutex_);
        }
        LOG_MAIN_ERROR_AT("Unsupported OpenVINO Letterbox pixel format");
        return {};
    }

    TensorFrame tensor_frame;
    switch (frame_pixel_format) {
        case PixelFormat::kNV12:
            tensor_frame = PackNv12(frame);
            break;
        case PixelFormat::kI420:
            tensor_frame = PackI420(frame);
            break;
        case PixelFormat::kNV21:
            tensor_frame = PackNv21AsNv12(frame);
            break;
        case PixelFormat::kRGB24:
        case PixelFormat::kBGR24:
            tensor_frame = PackRgb24(frame);
            break;
        case PixelFormat::kGRAY8:
            tensor_frame = PackGray8(frame);
            break;
        default:
            LOG_MAIN_ERROR_AT("Unsupported OpenVINO frame preprocess pixel format");
            return {};
    }

    tensor_frame.tensor_meta_ = BuildTensorMeta(frame, input_width_, input_height_);
    return tensor_frame;
}
