#include "media/converter/media_frame_converter.h"

#include "media/converter/ffmpeg_audio_converter.h"
#include "media/converter/ffmpeg_video_converter.h"
#include "media/ffmpeg_frame_buffer.h"
#include "media/ffmpeg_raw_frame_buffer.h"
#include "media/simple_buffer.h"
#include "media/ffmpeg_format.h"

#include <chrono>
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

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
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


/// @brief 从 AVFrame 填充 MediaFrame 的类型和元数据，不复制平面数据。
/// Raw AVFrame 的平面地址不一定能用 offset 表示，所以这里的 offset 仅
/// 保留为 0；实际平面地址由 FFmpegRawFrameBuffer::PlaneData() 提供。
/// 只要 MediaFrame.backend.type 为 FFMPEG，MediaFrameToAVFrame() 就会走
/// av_frame_ref() 快速路径，不会根据这些 offset 重新读取 buffer。
///
/// @param frame AVFrame 引用
/// @param media_type 输出的媒体类型
/// @param meta 输出的元数据
/// @param error 输出的错误信息
/// @return true 成功 false 失败
bool FillRawFrameMeta(const AVFrame& frame, MediaType& media_type, FrameMeta& meta, std::string& error) {
    if (frame.width > 0 && frame.height > 0) {
        const auto format = static_cast<AVPixelFormat>(frame.format);
        const PixelFormat pixel_format = FromAVPixelFormat(format);
        const int plane_count = av_pix_fmt_count_planes(format);
        if (pixel_format == PixelFormat::kUnknown || !frame.data[0] || plane_count <= 0 || plane_count > 8) {
            error = "Unsupported or empty video AVFrame";
            return false;
        }

        VideoFrameMeta video{};
        video.pixel_format = pixel_format;
        video.width = frame.width;
        video.height = frame.height;
        video.plane_count = plane_count;

        // 元数据中的 stride 仍然保留，便于知道每行在 AVFrame 中的布局。
        // offset 对 raw buffer 没有意义，因此统一为 0。
        for (int plane = 0; plane < plane_count; ++plane) {
            const int stride = plane < AV_NUM_DATA_POINTERS ? frame.linesize[plane] : 0;
            video.plane_info[plane].offset = 0;
            video.plane_info[plane].stride = stride;
            video.plane_info[plane].size = 0;
        }

        media_type = MediaType::VIDEO;
        meta = video;
        return true;
    }

    if (frame.nb_samples > 0 && frame.ch_layout.nb_channels > 0) {
        const auto format = static_cast<AVSampleFormat>(frame.format);
        const SampleFormat sample_format = FromAVSampleFormat(format);
        const int channels = frame.ch_layout.nb_channels;
        const int bytes_per_sample = av_get_bytes_per_sample(format);
        const bool planar = av_sample_fmt_is_planar(format) != 0;
        const int plane_count = planar ? channels : 1;
        if (sample_format == SampleFormat::Unknown || frame.sample_rate <= 0 ||
            bytes_per_sample <= 0 || plane_count <= 0 || plane_count > 8 ||
            !frame.extended_data) {
            error = "Unsupported or empty audio AVFrame";
            return false;
        }

        AudioFrameMeta audio{};
        audio.sample_format = sample_format;
        audio.sample_rate = frame.sample_rate;
        audio.channels = channels;
        audio.channel_layout = frame.ch_layout.order == AV_CHANNEL_ORDER_NATIVE
            ? frame.ch_layout.u.mask : 0;
        audio.nb_samples = frame.nb_samples;
        audio.bytes_per_sample = bytes_per_sample;
        audio.planar = planar;
        audio.plane_count = plane_count;

        const size_t one_plane_size = static_cast<size_t>(frame.nb_samples) *
                                      static_cast<size_t>(bytes_per_sample);
        const size_t packed_plane_size = one_plane_size * static_cast<size_t>(channels);
        const size_t max_int32 = static_cast<size_t>(std::numeric_limits<int32_t>::max());
        if (one_plane_size > max_int32 || packed_plane_size > max_int32) {
            error = "Audio AVFrame plane size exceeds MediaFrame metadata range";
            return false;
        }
        for (int plane = 0; plane < plane_count; ++plane) {
            if (!frame.extended_data[plane]) {
                error = "Audio AVFrame plane is null";
                return false;
            }
            audio.planes[plane].offset = 0;
            audio.planes[plane].stride = static_cast<int32_t>(planar ? one_plane_size : packed_plane_size);
            audio.planes[plane].size = static_cast<int32_t>(planar ? one_plane_size : packed_plane_size);
        }

        media_type = MediaType::AUDIO;
        meta = audio;
        return true;
    }

    error = "AVFrame is neither a valid video nor audio frame";
    return false;
}


}  // namespace

void FFmpegAudioConverterDeleter::operator()(FFmpegAudioConverter* converter) const noexcept {
    delete converter;
}

void FFmpegVideoConverterDeleter::operator()(FFmpegVideoConverter* converter) const noexcept {
    delete converter;
}

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
        ffmpeg_video_converter_.reset(new FFmpegVideoConverter());
        if (!ffmpeg_video_converter_->Open(video_config_.width, video_config_.height,
                ToAVPixelFormat(video_config_.pixel_format), video_config_.sws_flags)) {
            const std::string error = ffmpeg_video_converter_->LastError();
            Close();
            last_error_ = error;
            return false;
        }
    }

    if (audio_requested) {
        ffmpeg_audio_converter_.reset(new FFmpegAudioConverter());
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
#if CONVERT_STATS_ENABLE
        ++stats.convert_errors;
#endif
        return false;
    }

#if CONVERT_STATS_ENABLE
    int64_t convert_start_time = Now();
    ++stats.convert_calls;
#endif

    bool result = false;
    if (input.type == MediaType::VIDEO) {
        result = ffmpegVideoConvert(input, output);
    } else if (input.type == MediaType::AUDIO) {
        result = ffmpegAudioConvert(input, output);
    } else {
        last_error_ = "Unsupported MediaFrame type";
#if CONVERT_STATS_ENABLE
        ++stats.convert_errors;
#endif
        return false;
    }

#if CONVERT_STATS_ENABLE
    if (result) {
        ++stats.convert_frames;
        int64_t convert_time_us = Now() - convert_start_time;
        stats.total_convert_time_us += convert_time_us;
        if (convert_time_us > stats.max_convert_time_us) {
            stats.max_convert_time_us = convert_time_us;
        }
        if (convert_time_us < stats.min_convert_time_us) {
            stats.min_convert_time_us = convert_time_us;
        }
    } else {
        ++stats.convert_errors;
    }
#endif

    return result;
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
    if (!MediaFrameToAVFrame(input, source)) {
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

#if !RAW_FRAME_BUFFER
    // 创建 MediaFrame 并将转换后的 AVFrame 转换为 MediaFrame
    // 注意：MediaFrame.buffer 仍然负责保持原始 AVFrame 的生命周期，backend.ptr 只是指向该对象的非拥有指针。
    output = std::make_shared<MediaFrame>();
    if (!AvFrameToMediaFrame(*converted, output.get())) {
        output.reset();
        av_frame_free(&converted);
        return false;
    }

    av_frame_free(&converted);
#else
    // 转换器已经把数据写入 converted。这里直接把 AVFrame 交给输出
    // MediaFrame，避免再次打包到 SimpleBuffer。
    if (!AdoptAVFrame(converted, output)) {
        return false;
    }
#endif
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

    if (!MediaFrameToAVFrame(input, source)) {
        av_frame_free(&source);
        return false;
    }

    AVFrame* converted = ffmpeg_audio_converter_->Convert(source);
    av_frame_free(&source);
    if (!converted) {
        last_error_ = ffmpeg_audio_converter_->LastError();
        return false;
    }

#if !RAW_FRAME_BUFFER
    output = std::make_shared<MediaFrame>();
    if (!AvFrameToMediaFrame(*converted, output.get())) {
        output.reset();
        av_frame_free(&converted);
        return false;
    }

    av_frame_free(&converted);
#else
    // 音频和视频使用相同的所有权转移规则：输出 MediaFrame 的 buffer
    // 持有 AVFrame，backend.ptr 指向同一个 AVFrame 但不单独负责释放。
    if (!AdoptAVFrame(converted, output)) {
        return false;
    }
#endif
    last_error_.clear();
    return true;
}

bool MediaFrameConverter::MediaFrameToAVFrame(const MediaFrame& input, AVFrame* av_frame) {
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
        const auto* raw_buffer =
            dynamic_cast<const FFmpegRawFrameBuffer*>(input.buffer.get());
        const auto* packed_buffer =
            dynamic_cast<const FFmpegFrameBuffer*>(input.buffer.get());
        const bool backend_matches_buffer =
            (raw_buffer && raw_buffer->GetFrame() == source) ||
            (packed_buffer && packed_buffer->GetFrame() == source);
        if (!backend_matches_buffer || (raw_buffer && !raw_buffer->IsValid())) {
            last_error_ = "MediaFrame raw buffer and backend frame do not match";
            return false;
        }
        av_frame_unref(av_frame);
        const int ret = av_frame_ref(av_frame, source);
        if (ret < 0) {
            last_error_ = "av_frame_ref failed: " + AvErrorString(ret);
            return false;
        }
        SetAVFrameTime(input, av_frame);
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
        SetAVFrameTime(input, av_frame);
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

        SetAVFrameTime(input, av_frame);
        return true;
    }

    last_error_ = "Unsupported MediaFrame type";
    return false;
}

bool MediaFrameConverter::AvFrameToMediaFrame(const AVFrame& av_frame, MediaFrame* media_frame) {
    if (!media_frame) {
        last_error_ = "Output MediaFrame is null";
        return false;
    }
    // 后面是将avframe中的数据复制到 SimpleBuffer 中，所有权为 MediaFrame
    *media_frame = MediaFrame{};
    media_frame->backend = {};
    SetMediaFrameTime(av_frame, media_frame);

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

void MediaFrameConverter::SetAVFrameTime(const MediaFrame& input, AVFrame* output) {
    // MediaFrame 的时间单位是微秒。转换器内部的 AVFrame 也暂时约定使用
    // 微秒刻度；真正送入编码器时，编码器再按自己的 time_base 换算。
    // kNoTimestamp 是工程层的“无时间戳”标记，不能直接写入 AVFrame，
    // 必须转换为 FFmpeg 约定的 AV_NOPTS_VALUE。
    output->pts = IsValidTimestamp(input.time.pts_us) ? input.time.pts_us : AV_NOPTS_VALUE;
    output->pkt_dts = IsValidTimestamp(input.time.dts_us) ? input.time.dts_us : AV_NOPTS_VALUE;
    output->duration = IsValidTimestamp(input.time.duration_us) ? input.time.duration_us : 0;
}


void MediaFrameConverter::SetMediaFrameTime(const AVFrame& input, MediaFrame* output) {
    // Converter 当前约定 AVFrame 中的时间戳也使用微秒刻度，因此这里不做
    // time_base 换算。注意：kNoTimestamp 与 0 不同，0 是合法的首帧时间戳。
    output->time.pts_us = input.pts == AV_NOPTS_VALUE ? kNoTimestamp : input.pts;
    output->time.dts_us = input.pkt_dts == AV_NOPTS_VALUE ? kNoTimestamp : input.pkt_dts;
    output->time.duration_us = input.duration == AV_NOPTS_VALUE ? kNoTimestamp : input.duration;
}

bool MediaFrameConverter::AdoptAVFrame(AVFrame* av_frame, std::shared_ptr<MediaFrame>& output) {
    output.reset();
    if (!av_frame) {
        last_error_ = "AVFrame is null";
        return false;
    }

    // 先验证并生成元数据，再把 AVFrame 的所有权交给 raw buffer。
    // 这样任何校验失败都仍由本函数负责释放传入的 AVFrame。
    const auto frame_deleter = [](AVFrame* frame) {
        av_frame_free(&frame);
    };
    std::unique_ptr<AVFrame, decltype(frame_deleter)> owned_frame(av_frame, frame_deleter);

    MediaType media_type = MediaType::UNKNOWN;
    FrameMeta meta;
    if (!FillRawFrameMeta(*av_frame, media_type, meta, last_error_)) {
        return false;
    }

    // make_shared 失败时 owned_frame 仍会释放 AVFrame。构造成功后再 release，
    // 从而把唯一所有权明确转交给 FFmpegRawFrameBuffer。
    auto raw_buffer = std::make_shared<FFmpegRawFrameBuffer>(owned_frame.get());
    owned_frame.release();
    MediaFrame result;
    result.type = media_type;
    result.meta = std::move(meta);
    result.buffer = raw_buffer;
    result.backend.type = BackendHandle::FFMPEG;
    result.backend.ptr = raw_buffer->GetFrame();
    SetMediaFrameTime(*raw_buffer->GetFrame(), &result);
    output = std::make_shared<MediaFrame>(std::move(result));
    return true;
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
#if CONVERT_STATS_ENABLE
    stats = ConvertStats{};
#endif
}

const std::string& MediaFrameConverter::LastError() {
    return last_error_;
}

#if CONVERT_STATS_ENABLE
void MediaFrameConverter::PrintStats() {
    uint64_t avg_convert_time = stats.convert_frames > 0 
        ? stats.total_convert_time_us / stats.convert_frames : 0;
        
    LOG_INFO("FFmpegConverterStats: convert_calls: {}, convert_frames: {}, convert_errors: {}", 
             stats.convert_calls, stats.convert_frames, stats.convert_errors);
    LOG_INFO("FFmpegConverterStats: total_convert_time(s): {}", stats.total_convert_time_us / 1000000.0);
    LOG_INFO("FFmpegConverterStats: max_convert_time(ms): {}", stats.max_convert_time_us / 1000.0);
    LOG_INFO("FFmpegConverterStats: min_convert_time(ms): {}", stats.min_convert_time_us / 1000.0);
    LOG_INFO("FFmpegConverterStats: avg_convert_time(ms): {}", avg_convert_time / 1000.0);
}
#endif
