#include "media/converter/ffmpeg_video_converter.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace {

std::string AvErrorString(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_make_error_string(buffer, sizeof(buffer), error_code);
    return buffer;
}

}  // namespace

FFmpegVideoConverter::FFmpegVideoConverter() = default;

FFmpegVideoConverter::~FFmpegVideoConverter() {
    Close();
}



bool FFmpegVideoConverter::Open(int width, int height, AVPixelFormat pixel_format, int sws_flags) {
    Close();

    if (width <= 0 || height <= 0) {
        last_error_ = "Invalid width or height";
        return false;
    }
    if (pixel_format == AV_PIX_FMT_NONE) {
        last_error_ = "Invalid pixel format";
        return false;
    }
    if (sws_flags < 0) {
        last_error_ = "Invalid sws flags";
        return false;
    }

    dst_width_ = width;
    dst_height_ = height;
    dst_pixel_format_ = pixel_format;
    dst_flags_ = sws_flags;
    return true;
}

AVFrame* FFmpegVideoConverter::Convert(const AVFrame* input) {
    if (!input) {
        last_error_ = "Invalid input frame";
        return nullptr;
    }
    if (dst_width_ <= 0 || dst_height_ <= 0 ||
        dst_pixel_format_ == AV_PIX_FMT_NONE) {
        last_error_ = "Video converter is not opened";
        return nullptr;
    }

    // 检查输入帧的格式
    int src_width = input->width;
    int src_height = input->height;
    AVPixelFormat src_pixel_format = static_cast<AVPixelFormat>(input->format);
    if (src_width <= 0 || src_height <= 0) {
        last_error_ = "Invalid input frame width or height";
        return nullptr;
    }
    if (src_pixel_format == AV_PIX_FMT_NONE) {
        last_error_ = "Invalid input frame pixel format";
        return nullptr;
    }

    // sws_scale 需要每个输入平面都有有效地址。不同像素格式的平面数量
    // 不同，但 data[0] 对所有有效视频帧都必须存在。
    if (!input->data[0]) {
        last_error_ = "Input frame has no video data";
        return nullptr;
    }

    // 输入参数发生变化时，需要重新构造 sws 上下文
    if (!sws_ctx_ || src_width != cached_src_width_ || src_height != cached_src_height_ 
        || src_pixel_format != cached_src_pixel_format_) {        
        // 释放旧的 sws 上下文
        if (sws_ctx_) {
            sws_freeContext(sws_ctx_);
            sws_ctx_ = nullptr;
        }
        sws_ctx_ = sws_getContext(src_width, src_height, src_pixel_format,
                                  dst_width_, dst_height_, dst_pixel_format_,
                                  dst_flags_, nullptr, nullptr, nullptr);
        if (!sws_ctx_) {
            last_error_ = "Failed to create sws context";
            return nullptr;
        }
        
        // 更新缓存的输入参数
        cached_src_width_ = src_width;
        cached_src_height_ = src_height;
        cached_src_pixel_format_ = src_pixel_format;
    }    

    // 创建输出帧
    AVFrame* output = av_frame_alloc();
    if (!output) {
        last_error_ = "av_frame_alloc failed";
        return nullptr;
    }
    output->width = dst_width_;
    output->height = dst_height_;
    output->format = dst_pixel_format_;
    int ret = av_frame_get_buffer(output, 0);
    if (ret < 0) {
        last_error_ = "av_frame_get_buffer failed: " + AvErrorString(ret);
        av_frame_free(&output);
        return nullptr;
    }

    // 转换输入帧
    ret = sws_scale(sws_ctx_, input->data, input->linesize, 0, src_height,
                    output->data, output->linesize);
    if (ret != dst_height_) {
        last_error_ = "sws_scale failed: converted " + std::to_string(ret) +
                      " rows, expected " + std::to_string(dst_height_);
        av_frame_free(&output);
        return nullptr;
    }
    // 复制时间戳
    output->pts = input->pts;
    output->pkt_dts = input->pkt_dts;
    output->duration = input->duration;
    output->sample_aspect_ratio = input->sample_aspect_ratio;
    return output;
}

void FFmpegVideoConverter::Close() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    dst_width_ = 0;
    dst_height_ = 0;
    dst_pixel_format_ = AV_PIX_FMT_NONE;
    cached_src_width_ = 0;
    cached_src_height_ = 0;
    cached_src_pixel_format_ = AV_PIX_FMT_NONE;
    last_error_.clear();
}

const std::string& FFmpegVideoConverter::LastError() const {
    return last_error_;
}
