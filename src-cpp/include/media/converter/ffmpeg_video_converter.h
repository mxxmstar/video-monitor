#pragma once

#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}


class FFmpegVideoConverter {
public:
    FFmpegVideoConverter();
    ~FFmpegVideoConverter();

    /// @brief 打开视频转换器
    /// @param width 目标宽度
    /// @param height 目标高度
    /// @param pixel_format 目标像素格式
    /// @param sws_flags 转换标志
    /// @return 是否成功打开
    bool Open(int width, int height, AVPixelFormat pixel_format, int sws_flags);

    /// @brief 转换视频帧
    /// @param input 输入视频帧
    /// @return 转换后的视频帧
    AVFrame* Convert(const AVFrame* input);

    /// @brief 关闭视频转换器
    void Close();

    /// @brief 获取最近错误信息
    const std::string& LastError() const;

private:
    struct SwsContext* sws_ctx_{nullptr}; ///< 转换上下文
    std::string last_error_{}; ///< 最近错误信息
    
    int dst_width_{0}; ///< 目标宽度
    int dst_height_{0}; ///< 目标高度
    int dst_flags_{SWS_BILINEAR}; ///< 目标转换标志
    AVPixelFormat dst_pixel_format_{AV_PIX_FMT_NONE}; ///< 目标像素格式

    int cached_src_width_{0}; ///< 缓存的上一帧输入宽度
    int cached_src_height_{0}; ///< 缓存的上一帧输入高度
    AVPixelFormat cached_src_pixel_format_{AV_PIX_FMT_NONE}; ///< 缓存的上一帧输入像素格式
};