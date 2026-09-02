#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "media/media_frame.h"

struct AVFrame;
class FFmpegAudioConverter;
class FFmpegVideoConverter;

enum class ConvertBackend {
    FFmpeg,
    OpenCV,
    SIMD
};

/// @brief 视频转换配置
/// @param width 目标宽度
/// @param height 目标高度
/// @param pixel_format 目标像素格式
struct VideoConvertConfig {
    int width{0};
    int height{0};
    PixelFormat pixel_format{PixelFormat::kUnknown};
    int sws_flags{2};
};

/// @brief 音频转换配置
/// @param sample_rate 目标采样率
/// @param channels 目标通道数
/// @param channel_layout 目标通道布局
/// @param sample_format 目标样本格式
struct AudioConvertConfig {
    int sample_rate{0};
    int channels{0};
    uint64_t channel_layout{0};
    SampleFormat sample_format{SampleFormat::Unknown};
};

/// @brief 媒体帧转换配置
/// @param backend 转换后端
/// @param video 视频转换配置
/// @param audio 音频转换配置
struct MediaFrameConverterConfig {
    ConvertBackend backend{ConvertBackend::FFmpeg};
    VideoConvertConfig video;
    AudioConvertConfig audio;
};


class MediaFrameConverter {
public:
    // 构造函数放在 cpp 中定义。成员 unique_ptr 指向前置声明类型，
    // 只有在 cpp 已包含完整类型定义后，编译器才能正确生成销毁逻辑。
    MediaFrameConverter();
    ~MediaFrameConverter();

    MediaFrameConverter(const MediaFrameConverter&) = delete;
    MediaFrameConverter& operator=(const MediaFrameConverter&) = delete;

    /// @brief 打开媒体帧转换器
    /// @param config 转换配置
    /// @return 是否成功打开
    bool Open(const MediaFrameConverterConfig& config);
    
    /// @brief 转换媒体帧
    /// 输入帧由调用者拥有，converter 只在转换时读取它
    /// 输出帧在多个模块之间共享，使用shared_ptr管理它的生命周期，
    /// @param input 输入媒体帧
    /// @param output 输出媒体帧
    /// @return 是否成功转换
    bool Convert(const MediaFrame& input, std::shared_ptr<MediaFrame>& output);
    
    /// @brief 关闭媒体帧转换器
    void Close();
    
    /// @brief 获取最近错误信息
    static const std::string& LastError();

    /// @brief 构建AVFrame
    /// @param input 输入媒体帧
    /// @param av_frame 输出AVFrame
    /// @return 是否成功构建AVFrame
    static bool MediaFrameToAVFrame(const MediaFrame& input, AVFrame* av_frame);
    
    /// @brief 构建媒体帧
    /// @param av_frame 输入AVFrame
    /// @param media_frame 输出媒体帧
    /// @return 是否成功构建媒体帧
    static bool AvFrameToMediaFrame(const AVFrame& av_frame, MediaFrame* media_frame);

    /// @brief 设置 AVFrame 时间戳
    /// @param input 输入 MediaFrame
    /// @param output 输出 AVFrame
    static void SetAVFrameTime(const MediaFrame& input, AVFrame* output);

    /// @brief 设置 MediaFrame 时间戳
    /// @param input 输入 AVFrame
    /// @param output 输出 MediaFrame
    static void SetMediaFrameTime(const AVFrame& input, MediaFrame* output);

private:
    /// @brief FFmpeg视频转换，基于swscale
    /// @param input 输入视频帧
    /// @param output 输出视频帧
    /// @return 是否成功转换
    bool ffmpegVideoConvert(const MediaFrame& input,
                            std::shared_ptr<MediaFrame>& output);

    /// @brief FFmpeg音频转换，基于swresample
    /// @param input 输入音频帧
    /// @param output 输出音频帧
    /// @return 是否成功转换
    bool ffmpegAudioConvert(const MediaFrame& input,
                            std::shared_ptr<MediaFrame>& output);    

    VideoConvertConfig video_config_{};
    AudioConvertConfig audio_config_{};
    ConvertBackend backend_{ConvertBackend::FFmpeg};
    inline static std::string last_error_; ///< 最近错误信息

    // MediaFrameConverter 负责输入输出适配；具体的 sws/swr 调用由这两个
    // 已有的 FFmpeg converter 完成。使用 unique_ptr 可以在头文件中隐藏
    // FFmpeg converter 的完整定义，降低公共接口对 FFmpeg 实现的暴露。
    std::unique_ptr<FFmpegVideoConverter> ffmpeg_video_converter_{};
    std::unique_ptr<FFmpegAudioConverter> ffmpeg_audio_converter_{};
    bool opened_{false};

};