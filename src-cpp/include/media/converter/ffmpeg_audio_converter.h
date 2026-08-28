#pragma once

#include <string>
#include <vector>
#include <cstdint>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}


class FFmpegAudioConverter {
public:
    FFmpegAudioConverter();
    ~FFmpegAudioConverter();

    /// @brief 打开音频转换器
    /// @param width 目标宽度
    /// @param height 目标高度
    /// @param pixel_format 目标像素格式
    /// @param sws_flags 转换标志
    /// @return 是否成功打开
    bool Open(uint64_t ch_layout, int sample_rate, AVSampleFormat sample_format);

    /// @brief 转换音频帧
    /// @param input 输入音频帧
    /// @return 转换后的音频帧
    AVFrame* Convert(const AVFrame* input);

    /// @brief 关闭音频转换器
    void Close();

    /// @brief 获取最近错误信息
    const std::string& LastError() const;

private:
    struct SwrContext* swr_ctx_{nullptr}; ///< 转换上下文
    std::string last_error_{}; ///< 最近错误信息
    
    uint64_t dst_ch_layout_{0}; ///< 目标通道布局
    int dst_sample_rate_{0}; ///< 目标采样率
    AVSampleFormat dst_sample_format_{AV_SAMPLE_FMT_NONE}; ///< 目标样本格式

    uint64_t cached_src_ch_layout_{0}; ///< 缓存的上一帧输入通道布局
    int cached_src_sample_rate_{0}; ///< 缓存的上一帧输入采样率
    AVSampleFormat cached_src_sample_format_{AV_SAMPLE_FMT_NONE}; ///< 缓存的上一帧输入样本格式
};