#include "media/converter/ffmpeg_audio_converter.h"

#include <limits>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswresample/version.h>
}

namespace {

std::string AvErrorString(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_make_error_string(buffer, sizeof(buffer), error_code);
    return buffer;
}

// FFmpeg 5.1 / libswresample 4 开始使用 AVChannelLayout，并提供
// swr_alloc_set_opts2()。较早版本仍使用 uint64_t 声道掩码。
#if LIBSWRESAMPLE_VERSION_MAJOR >= 4

bool CopyInputChannelLayout(const AVFrame* input,
                            AVChannelLayout& layout,
                            uint64_t& cache_mask,
                            std::string& error) {
    if (input->ch_layout.nb_channels <= 0) {
        error = "Input frame has no channel layout";
        return false;
    }

    const int ret = av_channel_layout_copy(&layout, &input->ch_layout);
    if (ret < 0) {
        error = "Failed to copy input channel layout: " + AvErrorString(ret);
        return false;
    }

    // Open() 的输出布局接口仍是 uint64_t 位掩码。对于 native layout，掩码
    // 可以用于缓存比较；自定义布局不能无损表示为掩码，令其每帧重建上下文，
    // 以优先保证正确性而不是错误复用 SwrContext。
    cache_mask = layout.order == AV_CHANNEL_ORDER_NATIVE ? layout.u.mask : 0;
    return true;
}

int InputChannelCount(const AVFrame* input) {
    return input->ch_layout.nb_channels;
}

SwrContext* CreateSwrContext(const AVChannelLayout& source_layout,
                             AVSampleFormat source_format,
                             int source_sample_rate,
                             uint64_t destination_channel_layout,
                             AVSampleFormat destination_format,
                             int destination_sample_rate,
                             std::string& error) {
    AVChannelLayout destination_layout{};
    int ret = av_channel_layout_from_mask(&destination_layout,
                                          destination_channel_layout);
    if (ret < 0) {
        error = "Invalid destination channel layout: " + AvErrorString(ret);
        return nullptr;
    }

    SwrContext* context = nullptr;
    ret = swr_alloc_set_opts2(&context,
                              &destination_layout,
                              destination_format,
                              destination_sample_rate,
                              &source_layout,
                              source_format,
                              source_sample_rate,
                              0,
                              nullptr);
    av_channel_layout_uninit(&destination_layout);
    if (ret < 0 || !context) {
        error = "swr_alloc_set_opts2 failed: " + AvErrorString(ret);
        return nullptr;
    }

    ret = swr_init(context);
    if (ret < 0) {
        error = "swr_init failed: " + AvErrorString(ret);
        swr_free(&context);
        return nullptr;
    }

    return context;
}

bool PrepareOutputLayout(AVFrame* output,
                         uint64_t destination_channel_layout,
                         std::string& error) {
    const int ret = av_channel_layout_from_mask(&output->ch_layout,
                                                destination_channel_layout);
    if (ret < 0) {
        error = "Failed to set output channel layout: " + AvErrorString(ret);
        return false;
    }
    return true;
}

#else

bool GetInputChannelLayout(const AVFrame* input,
                           uint64_t& layout,
                           std::string& error) {
    layout = input->channel_layout;
    if (layout == 0 && input->channels > 0) {
        layout = av_get_default_channel_layout(input->channels);
    }
    if (layout == 0) {
        error = "Input frame has no channel layout";
        return false;
    }
    return true;
}

int InputChannelCount(const AVFrame* input) {
    return input->channels;
}

SwrContext* CreateSwrContext(uint64_t source_channel_layout,
                             AVSampleFormat source_format,
                             int source_sample_rate,
                             uint64_t destination_channel_layout,
                             AVSampleFormat destination_format,
                             int destination_sample_rate,
                             std::string& error) {
    SwrContext* context = swr_alloc_set_opts(nullptr,
                                             destination_channel_layout,
                                             destination_format,
                                             destination_sample_rate,
                                             source_channel_layout,
                                             source_format,
                                             source_sample_rate,
                                             0,
                                             nullptr);
    if (!context) {
        error = "swr_alloc_set_opts failed";
        return nullptr;
    }

    const int ret = swr_init(context);
    if (ret < 0) {
        error = "swr_init failed: " + AvErrorString(ret);
        swr_free(&context);
        return nullptr;
    }
    return context;
}

bool PrepareOutputLayout(AVFrame* output,
                         uint64_t destination_channel_layout,
                         std::string&) {
    output->channel_layout = destination_channel_layout;
    output->channels = av_get_channel_layout_nb_channels(destination_channel_layout);
    return output->channels > 0;
}

#endif

/// @brief 设置 AVFrame 的声道布局信息
/// @param frame 目标 AVFrame 指针
/// @param channels 声道数量（如 2 表示立体声）
/// @param channel_layout 声道布局掩码（如 AV_CH_LAYOUT_STEREO）
/// @note 如果 channel_layout 为 0，则根据 channels 自动生成默认布局
void setFrameChannelLayout(AVFrame* frame, int channels, uint64_t channel_layout) {
    if (!frame) {
        return;
    }

    av_channel_layout_uninit(&frame->ch_layout);
    if (channel_layout != 0) {
        av_channel_layout_from_mask(&frame->ch_layout, channel_layout);
    } else {
        av_channel_layout_default(&frame->ch_layout, channels);
    }
}

}  // namespace

FFmpegAudioConverter::FFmpegAudioConverter() = default;

FFmpegAudioConverter::~FFmpegAudioConverter() {
    Close();
}

bool FFmpegAudioConverter::Open(uint64_t ch_layout,
                                int sample_rate,
                                AVSampleFormat sample_format) {
    Close();

    if (ch_layout == 0) {
        last_error_ = "Invalid destination channel layout";
        return false;
    }
    if (sample_rate <= 0) {
        last_error_ = "Invalid destination sample rate";
        return false;
    }
    if (sample_format == AV_SAMPLE_FMT_NONE) {
        last_error_ = "Invalid destination sample format";
        return false;
    }

    dst_ch_layout_ = ch_layout;
    dst_sample_rate_ = sample_rate;
    dst_sample_format_ = sample_format;
    last_error_.clear();
    return true;
}

AVFrame* FFmpegAudioConverter::Convert(const AVFrame* input) {
    if (!input) {
        last_error_ = "Invalid input frame";
        return nullptr;
    }
    if (dst_ch_layout_ == 0 || dst_sample_rate_ <= 0 ||
        dst_sample_format_ == AV_SAMPLE_FMT_NONE) {
        last_error_ = "Audio converter is not opened";
        return nullptr;
    }

    const int source_samples = input->nb_samples;
    const int source_sample_rate = input->sample_rate;
    const auto source_sample_format = static_cast<AVSampleFormat>(input->format);
    if (source_samples <= 0 || source_sample_rate <= 0 ||
        source_sample_format == AV_SAMPLE_FMT_NONE) {
        last_error_ = "Input frame has invalid audio parameters";
        return nullptr;
    }

#if LIBSWRESAMPLE_VERSION_MAJOR >= 4
    AVChannelLayout source_layout{};
    uint64_t source_layout_mask = 0;
    if (!CopyInputChannelLayout(input, source_layout, source_layout_mask, last_error_)) {
        return nullptr;
    }
#else
    uint64_t source_layout_mask = 0;
    if (!GetInputChannelLayout(input, source_layout_mask, last_error_)) {
        return nullptr;
    }
#endif

    const int source_channels = InputChannelCount(input);
    if (source_channels <= 0 || !input->extended_data) {
        last_error_ = "Input frame has no audio samples";
#if LIBSWRESAMPLE_VERSION_MAJOR >= 4
        av_channel_layout_uninit(&source_layout);
#endif
        return nullptr;
    }

    const bool input_shape_changed =
        source_layout_mask == 0 ||
        source_layout_mask != cached_src_ch_layout_ ||
        source_sample_rate != cached_src_sample_rate_ ||
        source_sample_format != cached_src_sample_format_;

    if (!swr_ctx_ || input_shape_changed) {
        swr_free(&swr_ctx_);
#if LIBSWRESAMPLE_VERSION_MAJOR >= 4
        swr_ctx_ = CreateSwrContext(source_layout,
                                    source_sample_format,
                                    source_sample_rate,
                                    dst_ch_layout_,
                                    dst_sample_format_,
                                    dst_sample_rate_,
                                    last_error_);
        av_channel_layout_uninit(&source_layout);
#else
        swr_ctx_ = CreateSwrContext(source_layout_mask,
                                    source_sample_format,
                                    source_sample_rate,
                                    dst_ch_layout_,
                                    dst_sample_format_,
                                    dst_sample_rate_,
                                    last_error_);
#endif
        if (!swr_ctx_) {
            return nullptr;
        }

        cached_src_ch_layout_ = source_layout_mask;
        cached_src_sample_rate_ = source_sample_rate;
        cached_src_sample_format_ = source_sample_format;
    }
#if LIBSWRESAMPLE_VERSION_MAJOR >= 4
    else {
        av_channel_layout_uninit(&source_layout);
    }
#endif

    // swr 在采样率转换时可能缓存样本。输出容量必须包含已有延迟和当前输入，
    // 否则剩余样本会滞留在内部 FIFO，导致本帧的输出被截断。
    const int64_t delay = swr_get_delay(swr_ctx_, source_sample_rate);
    const int64_t output_capacity64 = av_rescale_rnd(
        delay + source_samples,
        dst_sample_rate_,
        source_sample_rate,
        AV_ROUND_UP);
    if (output_capacity64 <= 0 ||
        output_capacity64 > std::numeric_limits<int>::max()) {
        last_error_ = "Calculated output sample count is invalid";
        return nullptr;
    }
    const int output_capacity = static_cast<int>(output_capacity64);

    AVFrame* output = av_frame_alloc();
    if (!output) {
        last_error_ = "av_frame_alloc failed";
        return nullptr;
    }

    output->format = dst_sample_format_;
    output->sample_rate = dst_sample_rate_;
    output->nb_samples = output_capacity;
    if (!PrepareOutputLayout(output, dst_ch_layout_, last_error_)) {
        av_frame_free(&output);
        return nullptr;
    }

    const int ret = av_frame_get_buffer(output, 0);
    if (ret < 0) {
        last_error_ = "av_frame_get_buffer failed: " + AvErrorString(ret);
        av_frame_free(&output);
        return nullptr;
    }

    const int input_plane_count = av_sample_fmt_is_planar(source_sample_format)
        ? source_channels
        : 1;
    std::vector<const uint8_t*> input_data(
        static_cast<size_t>(input_plane_count));
    for (int plane = 0; plane < input_plane_count; ++plane) {
        input_data[static_cast<size_t>(plane)] = input->extended_data[plane];
        if (!input_data[static_cast<size_t>(plane)]) {
            last_error_ = "Input audio plane is null";
            av_frame_free(&output);
            return nullptr;
        }
    }

    const int converted_samples = swr_convert(swr_ctx_,
                                              output->extended_data,
                                              output_capacity,
                                              input_data.data(),
                                              source_samples);
    if (converted_samples < 0) {
        last_error_ = "swr_convert failed: " + AvErrorString(converted_samples);
        av_frame_free(&output);
        return nullptr;
    }

    output->nb_samples = converted_samples;
    // AVFrame 本身没有通用 time_base 字段。这里保留调用者提供的 pts，
    // 时间基的重标定由持有时间基信息的上层（例如 encoder）负责。
    output->pts = input->pts;
    output->duration = input->duration;
    last_error_.clear();
    return output;
}

void FFmpegAudioConverter::Close() {
    swr_free(&swr_ctx_);
    dst_ch_layout_ = 0;
    dst_sample_rate_ = 0;
    dst_sample_format_ = AV_SAMPLE_FMT_NONE;
    cached_src_ch_layout_ = 0;
    cached_src_sample_rate_ = 0;
    cached_src_sample_format_ = AV_SAMPLE_FMT_NONE;
    last_error_.clear();
}

const std::string& FFmpegAudioConverter::LastError() const {
    return last_error_;
}
