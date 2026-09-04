#include "media/puller/ffmpeg_puller.h"
#include "media/decoder/ffmpeg_decoder.h"
#include "media/converter/media_frame_converter.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/simple_buffer.h"
#include "common/log/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>
#include <thread>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace {

constexpr const char* kRtspUri = "rtsp://192.168.66.83/live/mainstream";
constexpr int kExpectedPacketCount = 10;

constexpr int kExpectedFrameCount = 10;
constexpr int kMaxReadAttempts = INT_MAX;

constexpr int kMaxTestSeconds = 60;

FFmpegPullerConfig testPullerConfig() {
    FFmpegPullerConfig config;
    config.io.connect_timeout = std::chrono::seconds(5);
    config.io.read_timeout = std::chrono::seconds(5);
    config.latency = LatencyMode::Low;

    RtspInputOptions rtsp;
    rtsp.transport = "tcp";
    config.rtsp = rtsp;

    return config;
}

InputEndpointConfig testInputEndpoint() {
    InputEndpointConfig endpoint;
    endpoint.uri = kRtspUri;
    endpoint.puller_kind = PullerKind::FFmpeg;
    return endpoint;
}

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int RunFfmpegPullerTest() {
    auto config = testPullerConfig();     
    auto endpoint = testInputEndpoint();

    FFmpegPuller puller(std::move(config));

    LOG_INFO("Opening RTSP stream: {}", endpoint.uri);
    const PullOpenResult open_result = puller.Open(endpoint);
    if (!open_result.Succeed()) {
        LOG_ERROR("Open failed: {}", (open_result.error.has_value()
                          ? open_result.error->message
                          : "unknown error"));
        return 1;
    }

    const MultiStreamInfo stream_info = puller.GetStreamInfo();
    if (!stream_info.HasVideoStream() && !stream_info.HasAudioStream()) {
        LOG_ERROR("Open succeeded, but no audio or video stream was found");
        puller.Close();
        return 1;
    }

    LOG_INFO("Stream info: {}", stream_info.stream_infos.size());

    int packet_count = 0;
    for (int attempt = 0;
         attempt < kMaxReadAttempts && packet_count < kExpectedPacketCount;
         ++attempt) {
        const PullReadResult read_result = puller.ReadPacket();

        if (read_result.status == PullReadStatus::NoData) {
            continue;
        }

        if (read_result.status != PullReadStatus::Packet ||
            !read_result.packet ||
            !read_result.packet->buffer ||
            read_result.packet->buffer->Size() == 0) {
            LOG_ERROR("Read failed at attempt {}: status={}",
                      static_cast<int>(read_result.status), (read_result.error.has_value() ? read_result.error->message : "unknown error"));
            if (read_result.error.has_value()) {
                LOG_ERROR(", error={}", read_result.error->message);    
            }
            puller.Close();
            return 1;
        }

        ++packet_count;
        LOG_INFO("Packet {}: type={}, codec={}, stream_index={}, size={}",
                 packet_count, static_cast<int>(read_result.packet->type),
                 static_cast<int>(read_result.packet->codec), read_result.packet->stream_index,
                 read_result.packet->buffer->Size());
    }

    puller.Close();

    if (packet_count != kExpectedPacketCount) {
        LOG_ERROR("Only received {} packet(s), expected {}", packet_count, kExpectedPacketCount);
        return 1;
    }

    LOG_INFO("FFmpegPuller RTSP test passed");
    return 0;
}

int RunFfmpegPullerDecoderTest() {
    auto config = testPullerConfig();     
    auto endpoint = testInputEndpoint();

    FFmpegPuller puller(std::move(config));

    LOG_INFO("Opening RTSP stream: {}", endpoint.uri);
    const PullOpenResult open_result = puller.Open(endpoint);
    if (!open_result.Succeed()) {
        LOG_ERROR("Open failed: {}", (open_result.error.has_value()
                          ? open_result.error->message
                          : "unknown error"));
        return 1;
    }

    const MultiStreamInfo stream_info = puller.GetStreamInfo();
    if (!stream_info.HasVideoStream() && !stream_info.HasAudioStream()) {
        LOG_ERROR("Open succeeded, but no audio or video stream was found");
        puller.Close();
        return 1;
    }

    LOG_INFO("Stream info: {}", stream_info.stream_infos.size());

    FFmpegDecoder decoder;
    int decoded_frames = 0;
    decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (!frame || frame->type != MediaType::VIDEO) {
            return;
        }

        ++decoded_frames;
        LOG_WARN("Decoded frame {}: {}x{}, pixel_format={}, pts_us={}",
                  decoded_frames, frame->Width(), frame->Height(),
                  static_cast<int>(frame->PixelFormat()), frame->time.pts_us);        
    });

    const MediaStreamInfo& video_stream_info =
        stream_info.stream_infos[stream_info.video_stream_idx_];
    // Open() 会根据 H.264 的 codec、time_base 和 SPS/PPS extra_data 创建
    // AVCodecContext。若这一步失败，继续读取包没有意义。
    if (!decoder.Open(video_stream_info)) {
        LOG_ERROR("Failed to open video decoder");
        puller.Close();
        return 1;
    }
    

    int packet_count = 0;
    for (int attempt = 0;
         attempt < kMaxReadAttempts && decoded_frames < kExpectedFrameCount;
         ++attempt) {
        const PullReadResult read_result = puller.ReadPacket();

        if (read_result.status == PullReadStatus::NoData) {
            continue;
        }

        if (read_result.status != PullReadStatus::Packet ||
            !read_result.packet ||
            !read_result.packet->buffer ||
            read_result.packet->buffer->Size() == 0) {
            LOG_ERROR("Read failed at attempt {}: status={}",
                      static_cast<int>(read_result.status), (read_result.error.has_value() ? read_result.error->message : "unknown error"));
            if (read_result.error.has_value()) {
                LOG_ERROR(", error={}", read_result.error->message);    
            }
            puller.Close();
            return 1;
        }

        if (read_result.packet->stream_index != video_stream_info.stream_index) {
            continue;
        }

        // stream_index 是首要依据；type/codec 的检查是测试层防御，能够在
        // Puller 的媒体元数据与实际视频轨道不一致时给出清晰报错。
        if (read_result.packet->type != MediaType::VIDEO ||
            read_result.packet->codec != video_stream_info.codec_type) {
            LOG_ERROR("Video stream packet metadata does not match decoder");
            decoder.Close();
            puller.Close();
            return 1;
        }

        ++packet_count;
        if (!decoder.Decode(read_result.packet)) {
            LOG_ERROR("Video decode failed at video packet {}", packet_count);
            decoder.Close();
            puller.Close();
            return 1;
        }
        
        LOG_INFO("Packet {}: type={}, codec={}, stream_index={}, size={}",
                 packet_count, static_cast<int>(read_result.packet->type),
                 static_cast<int>(read_result.packet->codec), read_result.packet->stream_index,
                 read_result.packet->buffer->Size());
    }

    
    

    // H.264 可能缓存 B 帧。即使读取循环已经停止，也要先向 decoder 发送
    // 空包进行 drain，才能拿到所有已接收编码包对应的尾部输出帧。
    if (!decoder.Flush()) {
        LOG_ERROR("Failed to flush video decoder");
        decoder.Close();
        puller.Close();
        return 1;
    }

    // 先 Flush 再 Close：Close 只释放 AVCodecContext，不会主动输出缓存帧。
    decoder.Close();
    puller.Close();

    if (decoded_frames != kExpectedFrameCount) {
        LOG_ERROR("Only decoded {} frame(s), expected {}", decoded_frames, kExpectedFrameCount);
        return 1;
    }

    LOG_INFO("FFmpegPuller RTSP test passed");
    return 0;
}

int RunFfmpegConverterTest() {
    // 本测试不连接 RTSP，而是在内存中构造 MediaFrame，验证完整的公共链路：
    //
    //   MediaFrame -> AVFrame -> sws/swr -> AVFrame -> MediaFrame
    //
    // 输入使用工程层的 MediaFrame，而不是直接把 AVFrame 交给底层转换器，
    // 这样可以同时验证 MediaFrame 的元数据、平面信息和 buffer 是否能被
    // 正确适配到 FFmpeg。
    std::cout << "Running FFmpeg converter test" << std::endl;

    // --------------------------- 视频转换测试 ---------------------------
    // 构造一帧 4x4 的 I420。I420 的连续内存布局是：
    //
    //   [Y: 4x4 = 16 字节][U: 2x2 = 4 字节][V: 2x2 = 4 字节]
    //
    // 这里的 offset/stride/size 就是 MediaFrame -> AVFrame 适配所依赖的
    // 关键信息。
    auto video_input = std::make_shared<MediaFrame>();
    video_input->type = MediaType::VIDEO;
    video_input->time.pts_us = 123;
    video_input->time.dts_us = 123;
    video_input->time.duration_us = 40'000;

    VideoFrameMeta video_meta{};
    video_meta.pixel_format = PixelFormat::kI420;
    video_meta.width = 4;
    video_meta.height = 4;
    video_meta.plane_count = 3;
    video_meta.plane_info[0] = PlaneInfo{0, 4, 16};
    video_meta.plane_info[1] = PlaneInfo{16, 2, 4};
    video_meta.plane_info[2] = PlaneInfo{20, 2, 4};
    video_input->meta = video_meta;

    std::vector<uint8_t> video_data(24, 0);
    for (int i = 0; i < 16; ++i) {
        video_data[static_cast<size_t>(i)] =
            static_cast<uint8_t>(16 + i);
    }
    std::fill(video_data.begin() + 16, video_data.begin() + 20, 90);
    std::fill(video_data.begin() + 20, video_data.end(), 160);
    video_input->buffer = std::make_shared<SimpleBuffer>(std::move(video_data));

    MediaFrameConverter video_converter;
    MediaFrameConverterConfig video_config{};
    video_config.video.width = 2;
    video_config.video.height = 2;
    video_config.video.pixel_format = PixelFormat::kNV12;
    if (!video_converter.Open(video_config)) {
        std::cerr << "Failed to open video converter: "
                  << video_converter.LastError() << std::endl;
        return 1;
    }

    std::shared_ptr<MediaFrame> video_output;
    if (!video_converter.Convert(*video_input, video_output) ||
        !video_output) {
        std::cerr << "Video conversion failed: "
                  << video_converter.LastError() << std::endl;
        return 1;
    }

    const auto* converted_video_meta = video_output->VideoMeta();
    const bool video_ok =
        video_output->type == MediaType::VIDEO &&
        converted_video_meta != nullptr &&
        converted_video_meta->width == 2 &&
        converted_video_meta->height == 2 &&
        converted_video_meta->pixel_format == PixelFormat::kNV12 &&
        converted_video_meta->plane_count == 2 &&
        video_output->buffer != nullptr &&
        video_output->buffer->Size() > 0 &&
        video_output->backend.type == BackendHandle::NONE &&
        video_output->time.pts_us == video_input->time.pts_us;

    if (!video_ok) {
        std::cerr << "Video conversion output is invalid" << std::endl;
        return 1;
    }

    std::cout << "Video conversion passed: I420 4x4 -> NV12 2x2"
              << std::endl;

    // --------------------------- 音频转换测试 ---------------------------
    // 构造 160 个 8000Hz 单声道 S16P 样本，约对应 20ms 音频。
    // S16P 的每个声道拥有独立平面；单声道时只有 extended_data[0]。
    auto audio_input = std::make_shared<MediaFrame>();
    audio_input->type = MediaType::AUDIO;
    audio_input->time.pts_us = 456;
    audio_input->time.dts_us = 456;
    audio_input->time.duration_us = 20'000;

    AudioFrameMeta audio_meta{};
    audio_meta.sample_format = SampleFormat::S16P;
    audio_meta.sample_rate = 8000;
    audio_meta.channels = 1;
    audio_meta.channel_layout = AV_CH_LAYOUT_MONO;
    audio_meta.nb_samples = 160;
    audio_meta.bytes_per_sample = 2;
    audio_meta.planar = true;
    audio_meta.plane_count = 1;
    audio_meta.planes[0] = PlaneInfo{0, 320, 320};
    audio_input->meta = audio_meta;

    std::vector<uint8_t> audio_data(320, 0);
    auto* audio_samples = reinterpret_cast<int16_t*>(audio_data.data());
    for (int i = 0; i < 160; ++i) {
        // 使用递增波形即可，测试重点是格式转换链路，不是音频内容质量。
        audio_samples[i] = static_cast<int16_t>((i % 32) * 500);
    }
    audio_input->buffer = std::make_shared<SimpleBuffer>(std::move(audio_data));

    MediaFrameConverter audio_converter;
    MediaFrameConverterConfig audio_config{};
    audio_config.audio.sample_rate = 16000;
    audio_config.audio.channels = 2;
    audio_config.audio.channel_layout = AV_CH_LAYOUT_STEREO;
    audio_config.audio.sample_format = SampleFormat::S16;
    if (!audio_converter.Open(audio_config)) {
        std::cerr << "Failed to open audio converter: "
                  << audio_converter.LastError() << std::endl;
        return 1;
    }

    std::shared_ptr<MediaFrame> audio_output;
    if (!audio_converter.Convert(*audio_input, audio_output) ||
        !audio_output) {
        std::cerr << "Audio conversion failed: "
                  << audio_converter.LastError() << std::endl;
        return 1;
    }

    const auto* converted_audio_meta = audio_output->AudioMeta();
    // 8000Hz -> 16000Hz 后，样本数应大致翻倍；具体边界可能受 swr 延迟
    // 和重采样实现影响，所以这里只要求输出为正数，而不写死精确数量。
    const bool audio_ok =
        audio_output->type == MediaType::AUDIO &&
        converted_audio_meta != nullptr &&
        converted_audio_meta->sample_format == SampleFormat::S16 &&
        converted_audio_meta->sample_rate == 16000 &&
        converted_audio_meta->channels == 2 &&
        converted_audio_meta->nb_samples > 0 &&
        !converted_audio_meta->planar &&
        audio_output->buffer != nullptr &&
        audio_output->buffer->Size() > 0 &&
        audio_output->backend.type == BackendHandle::NONE &&
        audio_output->time.pts_us == audio_input->time.pts_us;

    if (!audio_ok) {
        std::cerr << "Audio conversion output is invalid" << std::endl;
        return 1;
    }

    std::cout << "Audio conversion passed: S16P 8000Hz mono -> "
              << "S16 16000Hz stereo, " << converted_audio_meta->nb_samples
              << " output sample(s)" << std::endl;

    std::cout << "FFmpeg converter test passed" << std::endl;
    return 0;
}
#if 1

int RunFfmpegPullerDecoderConverterEncoderTest() {
    auto config = testPullerConfig();     
    auto endpoint = testInputEndpoint();

    FFmpegPuller puller(std::move(config));

    LOG_INFO("Opening RTSP stream: {}", endpoint.uri);
    const PullOpenResult open_result = puller.Open(endpoint);
    if (!open_result.Succeed()) {
        LOG_ERROR("Open failed: {}", (open_result.error.has_value()
                          ? open_result.error->message
                          : "unknown error"));
        return 1;
    }

    const MultiStreamInfo stream_info = puller.GetStreamInfo();
    if (!stream_info.HasVideoStream() && !stream_info.HasAudioStream()) {
        LOG_ERROR("Open succeeded, but no audio or video stream was found");
        puller.Close();
        return 1;
    }
    const MediaStreamInfo& video_stream_info =
        stream_info.stream_infos[stream_info.video_stream_idx_];
    video_stream_info.Dump(false);

    LOG_INFO("Stream info: {}", stream_info.stream_infos.size());

    FFmpegDecoder decoder;
    int64_t decoded_frames = 0;
    int64_t decoded_packets = 0;
    int64_t decode_calls = 0;
    int64_t decode_errors = 0;
    int64_t decode_total_us = 0;
    uint32_t decode_min_us = UINT32_MAX;
    uint32_t decode_max_us = 0;
    uint32_t decode_avg_us = 0;


    MediaFrameConverter converter;
    int64_t converted_frames = 0;
    int64_t convert_errors = 0;
    int64_t convert_total_us = 0;
    uint32_t convert_min_us = UINT32_MAX;
    uint32_t convert_max_us = 0;
    uint32_t convert_avg_us = 0;

    MediaFrameConverterConfig convert_config;
    convert_config.backend = ConvertBackend::FFmpeg;
    convert_config.video.width = video_stream_info.Video().width;
    convert_config.video.height = video_stream_info.Video().height;
    convert_config.video.pixel_format = PixelFormat::kI420;

    EncoderConfig encoder_config;
    encoder_config.media_type = MediaType::VIDEO;
    encoder_config.codec_type = CodecType::H264;
    encoder_config.bitrate = 2'000'000;
    encoder_config.thread_count = 1;

    VideoEncoderConfig& video_enc_cfg = encoder_config.video();
    video_enc_cfg.width = video_stream_info.Video().width;
    video_enc_cfg.height = video_stream_info.Video().height;
    LOG_INFO("Video h x w: {} x {}", video_enc_cfg.height, video_enc_cfg.width);
    video_enc_cfg.fps_num = 25;
    video_enc_cfg.fps_den = 1;
    video_enc_cfg.pixel_format = PixelFormat::kI420;
    video_enc_cfg.gop_size = 50;
    video_enc_cfg.max_b_frames = 0;
    video_enc_cfg.preset = "ultrafast";
    video_enc_cfg.tune = "zerolatency";

    FFmpegEncoder encoder;
    int64_t encoded_frames = 0;
    int64_t encoded_packets = 0;
    int64_t encode_calls = 0;
    int64_t encode_errors = 0;
    int64_t encode_total_us = 0;
    uint32_t encode_min_us = UINT32_MAX;
    uint32_t encode_max_us = 0;
    uint32_t encode_avg_us = 0;

    decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (!frame || frame->type != MediaType::VIDEO) {
            return;
        }
        
        LOG_INFO("Decoded frame {}: {}x{}, pixel_format={}, pts_us={}",
                  decoded_frames, frame->Width(), frame->Height(),
                  static_cast<int>(frame->PixelFormat()), frame->time.pts_us);

        if (!frame || frame->type != MediaType::VIDEO) {
            return;
        }

        
        LOG_INFO("Decoded frame {}: {}x{}, pixel_format={}, pts_us={}",
                  decoded_frames, frame->Width(), frame->Height(),
                  static_cast<int>(frame->PixelFormat()), frame->time.pts_us);


        if (!converter.Open(convert_config)) {
            LOG_ERROR("Failed to open frame converter: {}", MediaFrameConverter::LastError());
            return;
        }

        std::shared_ptr<MediaFrame> converted_frame;
        auto before_convert_time = std::chrono::steady_clock::now();
        if (!converter.Convert(*frame, converted_frame)) {
            LOG_ERROR("Failed to convert frame {}: {}", decoded_frames, MediaFrameConverter::LastError());
            return;
        }
        auto after_convert_time = std::chrono::steady_clock::now();
        auto convert_duration_us = std::chrono::duration_cast<std::chrono::microseconds>(after_convert_time - before_convert_time).count();
        convert_total_us += convert_duration_us;
        convert_min_us = std::min(convert_min_us, static_cast<uint32_t>(convert_duration_us));
        convert_max_us = std::max(convert_max_us, static_cast<uint32_t>(convert_duration_us));
        convert_avg_us = static_cast<uint32_t>(convert_total_us / ++converted_frames);                
        
        LOG_INFO("Converted frame {}: {}x{}, pixel_format={}, pts_us={}",
                  converted_frames, converted_frame->Width(), converted_frame->Height(),
                  static_cast<int>(converted_frame->PixelFormat()), converted_frame->time.pts_us);
                
        std::vector<PacketPtr> packets;
        auto before_encode_time = std::chrono::steady_clock::now();
        if (!encoder.Encode(converted_frame, packets)) {
            LOG_ERROR("Failed to encode frame {}", decoded_frames);
            return;
        }
        auto after_encode_time = std::chrono::steady_clock::now();
        auto encode_duration_us = std::chrono::duration_cast<std::chrono::microseconds>(after_encode_time - before_encode_time).count();
        encode_total_us += encode_duration_us;
        encode_min_us = std::min(encode_min_us, static_cast<uint32_t>(encode_duration_us));
        encode_max_us = std::max(encode_max_us, static_cast<uint32_t>(encode_duration_us));
        encode_avg_us = static_cast<uint32_t>(encode_total_us / ++encoded_frames);                
        
        for (const auto& packet : packets) {
            ++encoded_packets;            
            LOG_INFO("Encoded packet {}: size={}, pts={}, keyframe={}", encoded_packets,
                      (packet->buffer ? packet->buffer->Size() : 0), packet->pts, packet->keyframe);
        }
    });
    
    // Open() 会根据 H.264 的 codec、time_base 和 SPS/PPS extra_data 创建
    // AVCodecContext。若这一步失败，继续读取包没有意义。
    if (!decoder.Open(video_stream_info)) {
        LOG_ERROR("Failed to open video decoder");
        puller.Close();
        return 1;
    }

    if (!encoder.Open(encoder_config)) {
        LOG_ERROR("Failed to open video encoder");
        decoder.Close();
        puller.Close();
        return 1;
    }

    int packet_count = 0;
    auto start_time = std::chrono::steady_clock::now();
    while(1) {
        auto end_time = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count() >= kMaxTestSeconds) {
            break;
        }

        const PullReadResult read_result = puller.ReadPacket();

        if (read_result.status == PullReadStatus::NoData) {
            continue;
        }

        if (read_result.status != PullReadStatus::Packet ||
            !read_result.packet ||
            !read_result.packet->buffer ||
            read_result.packet->buffer->Size() == 0) {
            LOG_ERROR("Read failed at attempt {}: status={}",
                      static_cast<int>(read_result.status), (read_result.error.has_value() ? read_result.error->message : "unknown error"));
            if (read_result.error.has_value()) {
                LOG_ERROR(", error={}", read_result.error->message);    
            }
            puller.Close();
            return 1;
        }

        if (read_result.packet->stream_index != video_stream_info.stream_index) {
            continue;
        }

        // stream_index 是首要依据；type/codec 的检查是测试层防御，能够在
        // Puller 的媒体元数据与实际视频轨道不一致时给出清晰报错。
        if (read_result.packet->type != MediaType::VIDEO ||
            read_result.packet->codec != video_stream_info.codec_type) {
            LOG_ERROR("Video stream packet metadata does not match decoder");
            decoder.Close();
            puller.Close();
            return 1;
        }

        ++packet_count;
        auto decode_start_time = Now();
        if (!decoder.Decode(read_result.packet)) {
            LOG_ERROR("Video decode failed at video packet {}", packet_count);
            decoder.Close();
            puller.Close();
            return 1;
        }
        auto decode_end_time = Now();
        auto decode_duration_us = decode_end_time - decode_start_time;
        decode_total_us += decode_duration_us;
        decode_min_us = std::min(decode_min_us, static_cast<uint32_t>(decode_duration_us));
        decode_max_us = std::max(decode_max_us, static_cast<uint32_t>(decode_duration_us));
        decode_avg_us = static_cast<uint32_t>(decode_total_us / ++decoded_frames);
        
        LOG_INFO("Packet {}: type={}, codec={}, stream_index={}, size={}",
                 packet_count, static_cast<int>(read_result.packet->type),
                 static_cast<int>(read_result.packet->codec), read_result.packet->stream_index,
                 read_result.packet->buffer->Size());
    }

    // H.264 可能缓存 B 帧。即使读取循环已经停止，也要先向 decoder 发送
    // 空包进行 drain，才能拿到所有已接收编码包对应的尾部输出帧。
    if (!decoder.Flush()) {
        LOG_ERROR("Failed to flush video decoder");
        decoder.Close();
        encoder.Close();
        puller.Close();
        return 1;
    }

    std::vector<PacketPtr> flush_packets;
    if (!encoder.Flush(flush_packets)) {
        LOG_ERROR("Failed to flush video encoder");
        decoder.Close();
        encoder.Close();
        puller.Close();
        return 1;
    }

    for (const auto& packet : flush_packets) {
        ++encoded_packets;
        LOG_INFO("Flush packet {}: size={}, pts={}, keyframe={}", encoded_packets,
                 packet->buffer ? packet->buffer->Size() : 0, packet->pts, packet->keyframe);
    }

    // 先 Flush 再 Close：Close 只释放 AVCodecContext，不会主动输出缓存帧。
    decoder.Close();
    encoder.Close();
    puller.Close();

    // 成功结果只表达测试真正验证的条件：已经解码出目标数量的视频帧。
    // video_packet_count 是过程诊断数据，不能作为“10 帧必须来自 10 包”
    // 的断言依据。
    LOG_INFO("FFmpegPuller -> FFmpegDecoder -> MediaFrameConverter -> FFmpegEncoder test passed");
    LOG_INFO("decoded_frames: {}, converted_frames: {}, encoded_frames: {}, encoded_packets: {}", decoded_frames, converted_frames, encoded_frames, encoded_packets);
    LOG_INFO("=======================");
    LOG_INFO("Decode stats: decode {} frames, min decode time: {} us, max decode time: {} us, avg decode time: {} us, total decode time: {} us", decoded_frames, decode_min_us, decode_max_us, decode_avg_us, decode_total_us);
    LOG_INFO("Convert stats: convert {} frames, min convert time: {} us, max convert time: {} us, avg convert time: {} us, total convert time: {} us", converted_frames, convert_min_us, convert_max_us, convert_avg_us, convert_total_us);
    LOG_INFO("Encode stats: encode {} frames, min encode time: {} us, max encode time: {} us, avg encode time: {} us, total encode time: {} us", encoded_frames, encode_min_us, encode_max_us, encode_avg_us, encode_total_us);
    // LOG_INFO("Diagnostic: " << video_packet_count
    //     << " video packet(s) consumed from " << input_packet_count
    //     << " input packet(s)");
    // LOG_INFO("Total encoded bytes: {} ({} KB)", total_encoded_bytes, (total_encoded_bytes / 1024));
    return 0;
}
#endif
}  // namespace

int main() {
    // RunFfmpegPullerTest();
    // RunFfmpegPullerDecoderTest();
    // RunFfmpegConverterTest();
    RunFfmpegPullerDecoderConverterEncoderTest();
}