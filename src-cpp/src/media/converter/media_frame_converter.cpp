#include "media/converter/media_frame_converter.h"

#include "media/converter/ffmpeg_audio_converter.h"
#include "media/converter/ffmpeg_video_converter.h"
#include "media/simple_buffer.h"
#include "media/ffmpeg_format.h"

#include <cstring>
#include <limits>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

using namespace Media;

namespace {

std::string AvErrorString(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_make_error_string(buffer, sizeof(buffer), error_code);
    return buffer;
}

/// @brief 根据声道数量生成默认布局掩码
/// @param channels 声道数
/// @return 默认布局掩码
uint64_t defaultChannelLayoutMask(int channels) {
    AVChannelLayout layout{};
    if (channels <= 0) {
        return 0;
    }
    av_channel_layout_default(&layout, channels);
    const uint64_t mask = layout.order == AV_CHANNEL_ORDER_NATIVE ? layout.u.mask : 0;
    av_channel_layout_uninit(&layout);
    return mask;
}

/// @brief 安全地获取 AVFrame 指定平面（plane）的数据指针
/// @param frame AVFrame 引用
/// @param plane 平面索引
/// @return 数据指针，或 nullptr 如果平面索引无效或未分配数据
const uint8_t* framePlane(const AVFrame& frame, int plane) {
    if (plane < 0) {
        return nullptr;
    }
    // 优先使用 extended_data，音频声道数大于8时ffmpeg会使用 extended_data 来存储数据
    if (frame.extended_data) {
        return frame.extended_data[plane];
    }
    return plane < AV_NUM_DATA_POINTERS ? frame.data[plane] : nullptr;
}

/// @brief 验证 MediaFrame 缓冲区中的偏移量和大小是否合法
bool isBufferRangeValid(const MediaFrame& frame, int32_t offset, int32_t size) {
    if (!frame.buffer || offset < 0 || size < 0) {
        return false;
    }

    const size_t start = static_cast<size_t>(offset);
    const size_t length = static_cast<size_t>(size);
    return start <= frame.buffer->Size() && length <= frame.buffer->Size() - start;
}

/// @brief 设置 AVFrame 时间戳
/// @param input 输入 MediaFrame
/// @param output 输出 AVFrame
void setAVFrameTime(const MediaFrame& input, AVFrame* output) {
    // MediaFrame 的时间单位是微秒。转换器内部的 AVFrame 也暂时约定使用
    // 微秒刻度；真正送入编码器时，编码器再按自己的 time_base 换算。
    // kNoTimestamp 是工程层的“无时间戳”标记，不能直接写入 AVFrame，
    // 必须转换为 FFmpeg 约定的 AV_NOPTS_VALUE。
    output->pts = IsValidTimestamp(input.time.pts_us) ? input.time.pts_us : AV_NOPTS_VALUE;
    output->pkt_dts = IsValidTimestamp(input.time.dts_us) ? input.time.dts_us : AV_NOPTS_VALUE;
    output->duration = IsValidTimestamp(input.time.duration_us) ? input.time.duration_us : 0;
}

/// @brief 设置 MediaFrame 时间戳
/// @param input 输入 AVFrame
/// @param output 输出 MediaFrame
void setAVFrameTime(const AVFrame& input, MediaFrame* output) {
    // Converter 当前约定 AVFrame 中的时间戳也使用微秒刻度，因此这里不做
    // time_base 换算。注意：kNoTimestamp 与 0 不同，0 是合法的首帧时间戳。
    output->time.pts_us = input.pts == AV_NOPTS_VALUE ? kNoTimestamp : input.pts;
    output->time.dts_us = input.pkt_dts == AV_NOPTS_VALUE ? kNoTimestamp : input.pkt_dts;
    output->time.duration_us = input.duration == AV_NOPTS_VALUE ? kNoTimestamp : input.duration;
}

}  // namespace

MediaFrameConverter::MediaFrameConverter() = default;

MediaFrameConverter::~MediaFrameConverter() {
    Close();
}

bool MediaFrameConverter::Open(const MediaFrameConverterConfig& config) {
    Close();
    // 暂时只实现了 ffmpeg 的 sws、swr 转换功能
    if (config.backend != ConvertBackend::FFmpeg) {
        last_error_ = "Only FFmpeg converter backend is implemented";
        return false;
    }
    // 检查是否有有效配置
    const bool video_requested = config.video.width > 0 ||
        config.video.height > 0 || config.video.pixel_format != PixelFormat::kUnknown;
    const bool audio_requested = config.audio.sample_rate > 0 ||
        config.audio.channels > 0 || config.audio.channel_layout != 0 ||
        config.audio.sample_format != SampleFormat::Unknown;

    if (!video_requested && !audio_requested) {
        last_error_ = "At least one media conversion configuration is required";
        return false;
    }

    if (video_requested && (config.video.width <= 0 || config.video.height <= 0 ||
         ToAVPixelFormat(config.video.pixel_format) == AV_PIX_FMT_NONE)) {
        last_error_ = "Invalid video converter configuration";
        return false;
    }

    if (audio_requested && (config.audio.sample_rate <= 0 || config.audio.channels <= 0 ||
         ToAVSampleFormat(config.audio.sample_format) == AV_SAMPLE_FMT_NONE)) {
        last_error_ = "Invalid audio converter configuration";
        return false;
    }

    video_config_ = config.video;
    audio_config_ = config.audio;
    backend_ = config.backend;

    if (audio_requested && audio_config_.channel_layout == 0) {
        audio_config_.channel_layout = defaultChannelLayoutMask(audio_config_.channels);
    }
    if (audio_requested && audio_config_.channel_layout == 0) {
        last_error_ = "Unable to determine audio channel layout";
        return false;
    }

    if (video_requested) {
        ffmpeg_video_converter_ = std::make_unique<FFmpegVideoConverter>();
        if (!ffmpeg_video_converter_->Open(video_config_.width, video_config_.height,
                ToAVPixelFormat(video_config_.pixel_format), video_config_.sws_flags)) {
            const std::string error = ffmpeg_video_converter_->LastError();
            Close();
            last_error_ = error;
            return false;
        }
    }

    if (audio_requested) {
        ffmpeg_audio_converter_ = std::make_unique<FFmpegAudioConverter>();
        if (!ffmpeg_audio_converter_->Open( audio_config_.channel_layout,
                audio_config_.sample_rate, ToAVSampleFormat(audio_config_.sample_format))) {
            const std::string error = ffmpeg_audio_converter_->LastError();
            Close();
            last_error_ = error;
            return false;
        }
    }

    opened_ = true;
    last_error_.clear();
    return true;
}

bool MediaFrameConverter::Convert(const MediaFrame& input, std::shared_ptr<MediaFrame>& output) {
    output.reset();

    if (!opened_) {
        last_error_ = "MediaFrameConverter is not opened";
        return false;
    }

    if (input.type == MediaType::VIDEO) {
        return ffmpegVideoConvert(input, output);
    }
    if (input.type == MediaType::AUDIO) {
        return ffmpegAudioConvert(input, output);
    }

    last_error_ = "Unsupported MediaFrame type";
    return false;
}

bool MediaFrameConverter::ffmpegVideoConvert(const MediaFrame& input,
    std::shared_ptr<MediaFrame>& output) {
    if (!ffmpeg_video_converter_) {
        last_error_ = "FFmpeg video converter is not configured";
        return false;
    }
    // 创建 AVFrame
    AVFrame* source = av_frame_alloc();
    if (!source) {
        last_error_ = "av_frame_alloc for video source failed";
        return false;
    }
    // 转换为 AVFrame
    if (!mediaFrameToAVFrame(input, source)) {
        av_frame_free(&source);
        return false;
    }
    // 转换
    AVFrame* converted = ffmpeg_video_converter_->Convert(source);
    av_frame_free(&source);
    if (!converted) {
        last_error_ = ffmpeg_video_converter_->LastError();
        return false;
    }

    // 创建 MediaFrame 并将转换后的 AVFrame 转换为 MediaFrame
    // 注意：MediaFrame.buffer 仍然负责保持原始 AVFrame 的生命周期，backend.ptr 只是指向该对象的非拥有指针。
    output = std::make_shared<MediaFrame>();
    if (!avFrameToMediaFrame(*converted, output.get())) {
        output.reset();
        av_frame_free(&converted);
        return false;
    }

    av_frame_free(&converted);
    last_error_.clear();
    return true;
}

bool MediaFrameConverter::ffmpegAudioConvert(const MediaFrame& input,
    std::shared_ptr<MediaFrame>& output) {
    if (!ffmpeg_audio_converter_) {
        last_error_ = "FFmpeg audio converter is not configured";
        return false;
    }

    AVFrame* source = av_frame_alloc();
    if (!source) {
        last_error_ = "av_frame_alloc for audio source failed";
        return false;
    }

    if (!mediaFrameToAVFrame(input, source)) {
        av_frame_free(&source);
        return false;
    }

    AVFrame* converted = ffmpeg_audio_converter_->Convert(source);
    av_frame_free(&source);
    if (!converted) {
        last_error_ = ffmpeg_audio_converter_->LastError();
        return false;
    }

    output = std::make_shared<MediaFrame>();
    if (!avFrameToMediaFrame(*converted, output.get())) {
        output.reset();
        av_frame_free(&converted);
        return false;
    }

    av_frame_free(&converted);
    last_error_.clear();
    return true;
}

bool MediaFrameConverter::mediaFrameToAVFrame(const MediaFrame& input, AVFrame* av_frame) {
    if (!av_frame) {
        last_error_ = "Output AVFrame is null";
        return false;
    }

    // 这是 FFmpeg 后端的快速路径。MediaFrame.buffer 仍然负责保持原始
    // AVFrame 的生命周期，backend.ptr 只是指向该对象的非拥有指针。
    // av_frame_ref 会为 AVFrame 的引用计数数据增加一份引用；本函数释放
    // 临时 source 时不会影响输入 MediaFrame 的数据。
    if (input.backend.type == BackendHandle::FFMPEG && input.backend.ptr) {
        const auto* source = static_cast<const AVFrame*>(input.backend.ptr);
        av_frame_unref(av_frame);
        const int ret = av_frame_ref(av_frame, source);
        if (ret < 0) {
            last_error_ = "av_frame_ref failed: " + AvErrorString(ret);
            return false;
        }
        setAVFrameTime(input, av_frame);
        return true;
    }

    if (!input.buffer || !input.buffer->Data() || input.buffer->Size() == 0) {
        last_error_ = "Input MediaFrame buffer is empty";
        return false;
    }

    if (input.type == MediaType::VIDEO) {
        const auto* meta = input.VideoMeta();
        if (!meta) {
            last_error_ = "Video MediaFrame has no metadata";
            return false;
        }

        const AVPixelFormat format = ToAVPixelFormat(meta->pixel_format);
        if (format == AV_PIX_FMT_NONE || meta->width <= 0 ||
            meta->height <= 0) {
            last_error_ = "Invalid video MediaFrame metadata";
            return false;
        }

        const int plane_count = av_pix_fmt_count_planes(format);
        if (plane_count <= 0 || plane_count > 4 ||
            meta->plane_count < plane_count) {
            last_error_ = "Unsupported or incomplete video plane metadata";
            return false;
        }

        av_frame_unref(av_frame);
        av_frame->format = format;
        av_frame->width = meta->width;
        av_frame->height = meta->height;
        int ret = av_frame_get_buffer(av_frame, 32);
        if (ret < 0) {
            last_error_ = "av_frame_get_buffer for video failed: " +
                          AvErrorString(ret);
            return false;
        }

        const uint8_t* source_data[4]{};
        int source_linesize[4]{};
        for (int plane = 0; plane < plane_count; ++plane) {
            const PlaneInfo& plane_info = meta->plane_info[plane];
            if (plane_info.stride <= 0 || plane_info.size <= 0 ||
                !isBufferRangeValid(input,
                                    plane_info.offset,
                                    plane_info.size)) {
                last_error_ = "Invalid video plane range";
                av_frame_unref(av_frame);
                return false;
            }
            source_data[plane] = input.buffer->Data() + plane_info.offset;
            source_linesize[plane] = plane_info.stride;
        }

        av_image_copy(av_frame->data,
                      av_frame->linesize,
                      source_data,
                      source_linesize,
                      format,
                      meta->width,
                      meta->height);
        setAVFrameTime(input, av_frame);
        return true;
    }

    if (input.type == MediaType::AUDIO) {
        const auto* meta = input.AudioMeta();
        if (!meta) {
            last_error_ = "Audio MediaFrame has no metadata";
            return false;
        }

        const AVSampleFormat format = ToAVSampleFormat(meta->sample_format);
        if (format == AV_SAMPLE_FMT_NONE || meta->sample_rate <= 0 ||
            meta->channels <= 0 || meta->nb_samples <= 0) {
            last_error_ = "Invalid audio MediaFrame metadata";
            return false;
        }

        const uint64_t channel_layout = meta->channel_layout != 0
            ? meta->channel_layout
            : defaultChannelLayoutMask(meta->channels);
        if (channel_layout == 0) {
            last_error_ = "Audio MediaFrame has no supported channel layout";
            return false;
        }

        const bool planar = av_sample_fmt_is_planar(format) != 0;
        const int plane_count = planar ? meta->channels : 1;
        const int bytes_per_sample = av_get_bytes_per_sample(format);
        if (bytes_per_sample <= 0 || plane_count <= 0 || plane_count > 8 ||
            meta->plane_count < plane_count) {
            last_error_ = "Invalid audio plane metadata";
            return false;
        }

        av_frame_unref(av_frame);
        av_frame->format = format;
        av_frame->sample_rate = meta->sample_rate;
        av_frame->nb_samples = meta->nb_samples;
        int ret = av_channel_layout_from_mask(&av_frame->ch_layout,
                                              channel_layout);
        if (ret < 0) {
            last_error_ = "Failed to set audio channel layout: " +
                          AvErrorString(ret);
            return false;
        }
        ret = av_frame_get_buffer(av_frame, 32);
        if (ret < 0) {
            last_error_ = "av_frame_get_buffer for audio failed: " +
                          AvErrorString(ret);
            return false;
        }

        const size_t one_plane_size = static_cast<size_t>(meta->nb_samples) *
                                      static_cast<size_t>(bytes_per_sample);
        const size_t packed_size = one_plane_size *
                                   static_cast<size_t>(meta->channels);
        for (int plane = 0; plane < plane_count; ++plane) {
            const PlaneInfo& plane_info = meta->planes[plane];
            const size_t required_size = planar ? one_plane_size : packed_size;
            if (plane_info.size < 0 ||
                static_cast<size_t>(plane_info.size) < required_size ||
                !isBufferRangeValid(input,
                                    plane_info.offset,
                                    plane_info.size)) {
                last_error_ = "Invalid audio plane range";
                av_frame_unref(av_frame);
                return false;
            }

            uint8_t* destination = planar
                ? av_frame->extended_data[plane]
                : av_frame->extended_data[0];
            if (!destination) {
                last_error_ = "Audio AVFrame plane is null";
                av_frame_unref(av_frame);
                return false;
            }
            std::memcpy(destination,
                        input.buffer->Data() + plane_info.offset,
                        required_size);
        }

        setAVFrameTime(input, av_frame);
        return true;
    }

    last_error_ = "Unsupported MediaFrame type";
    return false;
}

bool MediaFrameConverter::avFrameToMediaFrame(const AVFrame& av_frame, MediaFrame* media_frame) {
    if (!media_frame) {
        last_error_ = "Output MediaFrame is null";
        return false;
    }
    // 后面是将avframe中的数据复制到 SimpleBuffer 中，所有权为 MediaFrame
    *media_frame = MediaFrame{};
    media_frame->backend = {};
    setAVFrameTime(av_frame, media_frame);

    if (av_frame.width > 0 && av_frame.height > 0) {
        const auto format = static_cast<AVPixelFormat>(av_frame.format);
        const PixelFormat pixel_format = FromAVPixelFormat(format);
        if (pixel_format == PixelFormat::kUnknown || !av_frame.data[0]) {
            last_error_ = "Unsupported or empty video AVFrame";
            return false;
        }
        // 计算视频缓冲区大小
        const int total_size = av_image_get_buffer_size(format, av_frame.width,
            av_frame.height, 1);
        if (total_size <= 0) {
            last_error_ = "Failed to calculate video buffer size: " + AvErrorString(total_size);
            return false;
        }
        // 分配视频缓冲区
        std::vector<uint8_t> packed(static_cast<size_t>(total_size));
        const uint8_t* source_data[4] = {
            av_frame.data[0],
            av_frame.data[1],
            av_frame.data[2],
            av_frame.data[3],
        };
        int ret = av_image_copy_to_buffer(
            packed.data(),
            total_size,
            source_data,
            av_frame.linesize,
            format,
            av_frame.width,
            av_frame.height,
            1);
        if (ret < 0) {
            last_error_ = "av_image_copy_to_buffer failed: " +
                          AvErrorString(ret);
            return false;
        }

        const int plane_count = av_pix_fmt_count_planes(format);
        int packed_linesize[4]{};
        ptrdiff_t packed_linesize_ptrdiff[4]{};
        size_t plane_sizes[4]{};
        if (plane_count <= 0 || plane_count > 4 ||
            av_image_fill_linesizes(packed_linesize, format, av_frame.width) < 0) {
            last_error_ = "Failed to calculate video plane layout";
            return false;
        }
        for (int plane = 0; plane < 4; ++plane) {
            packed_linesize_ptrdiff[plane] = packed_linesize[plane];
        }
        ret = av_image_fill_plane_sizes(plane_sizes, format, av_frame.height, packed_linesize_ptrdiff);
        if (ret < 0) {
            last_error_ = "Failed to calculate video plane sizes: " + AvErrorString(ret);
            return false;
        }

        VideoFrameMeta meta{};
        meta.pixel_format = pixel_format;
        meta.width = av_frame.width;
        meta.height = av_frame.height;
        meta.plane_count = plane_count;

        size_t offset = 0;
        for (int plane = 0; plane < plane_count; ++plane) {
            if (plane_sizes[plane] > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
                offset > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
                last_error_ = "Video plane metadata exceeds int32 range";
                return false;
            }
            meta.plane_info[plane].offset = static_cast<int32_t>(offset);
            meta.plane_info[plane].stride = packed_linesize[plane];
            meta.plane_info[plane].size = static_cast<int32_t>(plane_sizes[plane]);
            offset += plane_sizes[plane];
        }

        media_frame->type = MediaType::VIDEO;
        media_frame->meta = meta;
        media_frame->buffer = std::make_shared<SimpleBuffer>(std::move(packed));
        last_error_.clear();
        return true;
    }

    if (av_frame.nb_samples > 0 && av_frame.ch_layout.nb_channels > 0) {
        const auto format = static_cast<AVSampleFormat>(av_frame.format);
        const SampleFormat sample_format = FromAVSampleFormat(format);
        const int channels = av_frame.ch_layout.nb_channels;
        const int bytes_per_sample = av_get_bytes_per_sample(format);
        if (sample_format == SampleFormat::Unknown ||
            av_frame.sample_rate <= 0 || bytes_per_sample <= 0) {
            last_error_ = "Unsupported or empty audio AVFrame";
            return false;
        }

        const bool planar = av_sample_fmt_is_planar(format) != 0;
        const int plane_count = planar ? channels : 1;
        const size_t one_plane_size = static_cast<size_t>(av_frame.nb_samples) *
                                      static_cast<size_t>(bytes_per_sample);
        // planar：每个声道一个平面，每个平面包含 nb_samples 个样本。
        // packed：所有声道交错存放在同一个平面，所以同一个平面要包含
        // nb_samples * channels 个样本。两种布局的总字节数实际都等于：
        // nb_samples * bytes_per_sample * channels。
        const size_t total_size = one_plane_size *
                                  static_cast<size_t>(channels);
        if (total_size == 0 ||
            total_size > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
            plane_count <= 0 || plane_count > 8) {
            last_error_ = "Invalid audio buffer size";
            return false;
        }

        std::vector<uint8_t> packed(total_size);
        for (int plane = 0; plane < plane_count; ++plane) {
            const uint8_t* source = framePlane(av_frame, plane);
            if (!source) {
                last_error_ = "Audio AVFrame plane is null";
                return false;
            }

            const size_t copy_size = planar
                ? one_plane_size
                : one_plane_size * static_cast<size_t>(channels);
            const size_t destination_offset = planar
                ? static_cast<size_t>(plane) * one_plane_size
                : 0;
            std::memcpy(packed.data() + destination_offset,
                        source,
                        copy_size);
        }

        AudioFrameMeta meta{};
        meta.sample_format = sample_format;
        meta.sample_rate = av_frame.sample_rate;
        meta.channels = channels;
        meta.channel_layout = av_frame.ch_layout.order == AV_CHANNEL_ORDER_NATIVE
            ? av_frame.ch_layout.u.mask
            : 0;
        meta.nb_samples = av_frame.nb_samples;
        meta.bytes_per_sample = bytes_per_sample;
        meta.planar = planar;
        meta.plane_count = plane_count;

        for (int plane = 0; plane < plane_count; ++plane) {
            const size_t offset = planar
                ? static_cast<size_t>(plane) * one_plane_size
                : 0;
            const size_t plane_size = planar
                ? one_plane_size
                : one_plane_size * static_cast<size_t>(channels);
            meta.planes[plane].offset = static_cast<int32_t>(offset);
            meta.planes[plane].stride = static_cast<int32_t>(plane_size);
            meta.planes[plane].size = static_cast<int32_t>(plane_size);
        }

        media_frame->type = MediaType::AUDIO;
        media_frame->meta = meta;
        media_frame->buffer = std::make_shared<SimpleBuffer>(std::move(packed));
        last_error_.clear();
        return true;
    }

    last_error_ = "AVFrame is neither a valid video nor audio frame";
    return false;
}

void MediaFrameConverter::Close() {
    if (ffmpeg_video_converter_) {
        ffmpeg_video_converter_->Close();
        ffmpeg_video_converter_.reset();
    }
    if (ffmpeg_audio_converter_) {
        ffmpeg_audio_converter_->Close();
        ffmpeg_audio_converter_.reset();
    }
    opened_ = false;
    video_config_ = {};
    audio_config_ = {};
    backend_ = ConvertBackend::FFmpeg;
    last_error_.clear();
}

const std::string& MediaFrameConverter::LastError() const {
    return last_error_;
}
