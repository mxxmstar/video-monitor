/// @file test_ffmpeg_puller.cpp
/// @brief FFmpegPuller 的 RTSP 集成测试。

#include "media/puller/ffmpeg_puller.h"
#include "media/decoder/ffmpeg_decoder.h"
#include "media/converter/media_frame_converter.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/simple_buffer.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace {

constexpr const char* kRtspUri = "rtsp://192.168.66.125/live/mainstream";
constexpr int kExpectedPacketCount = 10;
constexpr int kMaxReadAttempts = 10000;

int RunFfmpegPullerTest() {
    FFmpegPullerConfig config;
    config.io.connect_timeout = std::chrono::seconds(5);
    config.io.read_timeout = std::chrono::seconds(5);
    config.latency = LatencyMode::Low;

    RtspInputOptions rtsp;
    rtsp.transport = "tcp";
    config.rtsp = rtsp;

    InputEndpointConfig endpoint;
    endpoint.uri = kRtspUri;
    endpoint.puller_kind = PullerKind::FFmpeg;

    FFmpegPuller puller(std::move(config));

    std::cout << "Opening RTSP stream: " << endpoint.uri << std::endl;
    const PullOpenResult open_result = puller.Open(endpoint);
    if (!open_result.Succeed()) {
        std::cerr << "Open failed: "
                  << (open_result.error.has_value()
                          ? open_result.error->message
                          : "unknown error")
                  << std::endl;
        return 1;
    }

    const MultiStreamInfo stream_info = puller.GetStreamInfo();
    if (!stream_info.HasVideoStream() && !stream_info.HasAudioStream()) {
        std::cerr << "Open succeeded, but no audio or video stream was found"
                  << std::endl;
        puller.Close();
        return 1;
    }

    std::cout << "Stream info: " << stream_info.stream_infos.size()
              << " audio/video stream(s)" << std::endl;

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
            std::cerr << "Read failed at attempt " << (attempt + 1)
                      << ": status="
                      << static_cast<int>(read_result.status);
            if (read_result.error.has_value()) {
                std::cerr << ", error=" << read_result.error->message;
            }
            std::cerr << std::endl;
            puller.Close();
            return 1;
        }

        ++packet_count;
        std::cout << "Packet " << packet_count
                  << ": type="
                  << static_cast<int>(read_result.packet->type)
                  << ", codec="
                  << static_cast<int>(read_result.packet->codec)
                  << ", stream_index=" << read_result.packet->stream_index
                  << ", size=" << read_result.packet->buffer->Size()
                  << std::endl;
    }

    puller.Close();

    if (packet_count != kExpectedPacketCount) {
        std::cerr << "Only received " << packet_count << " packet(s), expected "
                  << kExpectedPacketCount << std::endl;
        return 1;
    }

    std::cout << "FFmpegPuller RTSP test passed" << std::endl;
    return 0;
}

int RunFfmpegPullerDecoderTest() {
    // 本测试只验证一条最小视频解码链路：
    //
    //     RTSP -> FFmpegPuller -> H.264 MediaPacket -> FFmpegDecoder -> MediaFrame
    //
    // 该 RTSP 输入同时包含 H.264 视频和 G.711A 音频。一个 FFmpegDecoder
    // 实例一次只能按一种 codec/轨道工作，所以循环中必须显式筛选视频包，
    // 不能把音频包一并送给 H.264 解码器。
    FFmpegPullerConfig config;
    config.io.connect_timeout = std::chrono::seconds(5);
    config.io.read_timeout = std::chrono::seconds(5);
    config.latency = LatencyMode::Low;

    RtspInputOptions rtsp;
    rtsp.transport = "tcp";
    config.rtsp = rtsp;

    InputEndpointConfig endpoint;
    endpoint.uri = kRtspUri;
    endpoint.puller_kind = PullerKind::FFmpeg;

    FFmpegPuller puller(std::move(config));

    std::cout << "Opening RTSP stream: " << endpoint.uri << std::endl;
    const PullOpenResult open_result = puller.Open(endpoint);
    if (!open_result.Succeed()) {
        std::cerr << "Open failed: "
                  << (open_result.error.has_value()
                          ? open_result.error->message
                          : "unknown error")
                  << std::endl;
        return 1;
    }

    const MultiStreamInfo stream_info = puller.GetStreamInfo();
    // 这是“视频解码”测试，因此音频流存在与否不影响本测试；但必须能够
    // 从 MultiStreamInfo 中找到视频流，否则无法创建 H.264 decoder。
    if (!stream_info.HasVideoStream()) {
        std::cerr << "Open succeeded, but no video stream was found"
                  << std::endl;
        puller.Close();
        return 1;
    }

    std::cout << "Stream info: " << stream_info.stream_infos.size()
              << " audio/video stream(s)" << std::endl;

    // video_stream_idx_ 是 stream_infos 容器中的位置；
    // video_stream_info.stream_index 才是 FFmpeg/MediaPacket 使用的真实流号。
    // 不能直接写 stream_infos[0]，因为其他输入中的音频、字幕等轨道可能
    // 排在视频流之前。
    const MediaStreamInfo& video_stream_info =
        stream_info.stream_infos[stream_info.video_stream_idx_];

    FFmpegDecoder decoder;
    int decoded_frames = 0;
    decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        // Decode() 可能因为 B 帧重排序而一次输出零帧或多帧，因此成功条件
        // 应以真正收到的 MediaFrame 为准，不能只看送入 decoder 的包数量。
        if (!frame || frame->type != MediaType::VIDEO) {
            return;
        }

        ++decoded_frames;
        std::cout << "Decoded frame " << decoded_frames
                  << ": " << frame->Width() << "x" << frame->Height()
                  << ", pixel_format="
                  << static_cast<int>(frame->PixelFormat())
                  << ", pts_us=" << frame->time.pts_us
                  << std::endl;
    });

    // Open() 会根据 H.264 的 codec、time_base 和 SPS/PPS extra_data 创建
    // AVCodecContext。若这一步失败，继续读取包没有意义。
    if (!decoder.Open(video_stream_info)) {
        std::cerr << "Failed to open video decoder" << std::endl;
        puller.Close();
        return 1;
    }

    int input_packet_count = 0;
    int video_packet_count = 0;
    // 本测试的通过条件是解码出 10 帧视频，而不是收到 10 个视频包。
    // 对 H.264 而言，编码包和解码帧并非一一对应：接入直播流后可能要先
    // 等待关键帧，且 B 帧重排序也会影响某个包何时产生输出帧。
    constexpr int kExpectedDecodedFrameCount = 10;

    // 限制的是“读取尝试次数”，防止服务异常或始终没有关键帧时测试无限
    // 等待。循环退出的正常条件是收到了足够的视频解码帧，而不是读到了
    // 固定数量的音视频混合包。
    for (int attempt = 0;
         attempt < kMaxReadAttempts &&
         decoded_frames < kExpectedDecodedFrameCount;
         ++attempt) {
        const PullReadResult read_result = puller.ReadPacket();

        if (read_result.status == PullReadStatus::NoData) {
            // NoData 不代表输入结束；live RTSP 在暂无可交付包时可正常返回它。
            continue;
        }

        if (read_result.status != PullReadStatus::Packet ||
            !read_result.packet ||
            !read_result.packet->buffer ||
            read_result.packet->buffer->Size() == 0) {
            std::cerr << "Read failed at attempt " << (attempt + 1)
                      << ": status="
                      << static_cast<int>(read_result.status);
            if (read_result.error.has_value()) {
                std::cerr << ", error=" << read_result.error->message;
            }
            std::cerr << std::endl;
            puller.Close();
            return 1;
        }

        ++input_packet_count;
        std::cout << "Input packet " << input_packet_count
                  << ": type="
                  << static_cast<int>(read_result.packet->type)
                  << ", codec="
                  << static_cast<int>(read_result.packet->codec)
                  << ", stream_index=" << read_result.packet->stream_index
                  << ", size=" << read_result.packet->buffer->Size()
                  << std::endl;

        // 当前 RTSP 流有 audio(stream_index=1) 和 video(stream_index=0)。
        // 音频 G.711A 数据没有 H.264 NAL 起始码；若送入 H.264 decoder，
        // FFmpeg 必然打印 "No start code is found"。因此这里直接跳过
        // 不属于当前视频轨道的包。
        if (read_result.packet->stream_index != video_stream_info.stream_index) {
            continue;
        }

        // stream_index 是首要依据；type/codec 的检查是测试层防御，能够在
        // Puller 的媒体元数据与实际视频轨道不一致时给出清晰报错。
        if (read_result.packet->type != MediaType::VIDEO ||
            read_result.packet->codec != video_stream_info.codec_type) {
            std::cerr << "Video stream packet metadata does not match decoder"
                      << std::endl;
            decoder.Close();
            puller.Close();
            return 1;
        }

        ++video_packet_count;
        if (!decoder.Decode(read_result.packet)) {
            std::cerr << "Video decode failed at video packet "
                      << video_packet_count << std::endl;
            decoder.Close();
            puller.Close();
            return 1;
        }
    }

    // H.264 可能缓存 B 帧。即使读取循环已经停止，也要先向 decoder 发送
    // 空包进行 drain，才能拿到所有已接收编码包对应的尾部输出帧。
    if (!decoder.Flush()) {
        std::cerr << "Failed to flush video decoder" << std::endl;
        decoder.Close();
        puller.Close();
        return 1;
    }

    // 先 Flush 再 Close：Close 只释放 AVCodecContext，不会主动输出缓存帧。
    decoder.Close();
    puller.Close();

    if (decoded_frames < kExpectedDecodedFrameCount) {
        std::cerr << "Only decoded " << decoded_frames << " video frame(s) from "
                  << video_packet_count << " video packet(s) after "
                  << input_packet_count << " input packet(s); expected "
                  << kExpectedDecodedFrameCount << std::endl;
        return 1;
    }

    // 成功结果只表达测试真正验证的条件：已经解码出目标数量的视频帧。
    // video_packet_count 是过程诊断数据，不能作为“10 帧必须来自 10 包”
    // 的断言依据。
    std::cout << "FFmpegPuller -> FFmpegDecoder test passed: "
              << decoded_frames << " video frame(s) decoded" << std::endl;
    std::cout << "Diagnostic: " << video_packet_count
              << " video packet(s) consumed from " << input_packet_count
              << " input packet(s)" << std::endl;
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

int RunFfmpegPullerDecoderConverterEncoderTest() {
    FFmpegPullerConfig config;
    config.io.connect_timeout = std::chrono::seconds(5);
    config.io.read_timeout = std::chrono::seconds(5);
    config.latency = LatencyMode::Low;

    RtspInputOptions rtsp;
    rtsp.transport = "tcp";
    config.rtsp = rtsp;

    InputEndpointConfig endpoint;
    endpoint.uri = kRtspUri;
    endpoint.puller_kind = PullerKind::FFmpeg;

    FFmpegPuller puller(std::move(config));

    std::cout << "Opening RTSP stream: " << endpoint.uri << std::endl;
    const PullOpenResult open_result = puller.Open(endpoint);
    if (!open_result.Succeed()) {
        std::cerr << "Open failed: "
            << (open_result.error.has_value()
                ? open_result.error->message
                : "unknown error")
            << std::endl;
        return 1;
    }

    const MultiStreamInfo stream_info = puller.GetStreamInfo();
    // 这是“视频解码”测试，因此音频流存在与否不影响本测试；但必须能够
    // 从 MultiStreamInfo 中找到视频流，否则无法创建 H.264 decoder。
    if (!stream_info.HasVideoStream()) {
        std::cerr << "Open succeeded, but no video stream was found"
            << std::endl;
        puller.Close();
        return 1;
    }

    std::cout << "Stream info: " << stream_info.stream_infos.size()
        << " audio/video stream(s)" << std::endl;

    // video_stream_idx_ 是 stream_infos 容器中的位置；
    // video_stream_info.stream_index 才是 FFmpeg/MediaPacket 使用的真实流号。
    // 不能直接写 stream_infos[0]，因为其他输入中的音频、字幕等轨道可能
    // 排在视频流之前。
    const MediaStreamInfo& video_stream_info =
        stream_info.stream_infos[stream_info.video_stream_idx_];

    FFmpegDecoder decoder;
    int decoded_frames = 0;
    int converted_frames = 0;
    int encoded_frames = 0;
    int encoded_packets = 0;
    int64_t total_encoded_bytes = 0;

    MediaFrameConverter converter;
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

    VideoEncoderConfig& video_enc_cfg = std::get<VideoEncoderConfig>(encoder_config.specific);
    video_enc_cfg.width = video_stream_info.Video().width;
    video_enc_cfg.height = video_stream_info.Video().height;
    video_enc_cfg.fps_num = 25;
    video_enc_cfg.fps_den = 1;
    video_enc_cfg.pixel_format = PixelFormat::kI420;
    video_enc_cfg.gop_size = 50;
    video_enc_cfg.max_b_frames = 0;
    video_enc_cfg.preset = "ultrafast";
    video_enc_cfg.tune = "zerolatency";

    FFmpegEncoder encoder;

    decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (!frame || frame->type != MediaType::VIDEO) {
            return;
        }

        ++decoded_frames;
        std::cout << "Decoded frame " << decoded_frames
            << ": " << frame->Width() << "x" << frame->Height()
            << ", pixel_format="
            << static_cast<int>(frame->PixelFormat())
            << ", pts_us=" << frame->time.pts_us
            << std::endl;

        if (!converter.Open(convert_config)) {
            std::cerr << "Failed to open frame converter: "
                      << MediaFrameConverter::LastError() << std::endl;
            return;
        }

        std::shared_ptr<MediaFrame> converted_frame;
        if (!converter.Convert(*frame, converted_frame)) {
            std::cerr << "Failed to convert frame " << decoded_frames << std::endl;
            return;
        }

        ++converted_frames;
        std::cout << "Converted frame " << converted_frames
            << ": " << converted_frame->Width() << "x" << converted_frame->Height()
            << ", pixel_format="
            << static_cast<int>(converted_frame->PixelFormat())
            << std::endl;

        std::vector<PacketPtr> packets;
        if (!encoder.Encode(converted_frame, packets)) {
            std::cerr << "Failed to encode frame " << decoded_frames << std::endl;
            return;
        }

        ++encoded_frames;
        for (const auto& packet : packets) {
            ++encoded_packets;
            total_encoded_bytes += packet->buffer ? packet->buffer->Size() : 0;
            std::cout << "Encoded packet " << encoded_packets
                << ": size=" << (packet->buffer ? packet->buffer->Size() : 0)
                << ", pts=" << packet->pts
                << ", keyframe=" << packet->keyframe
                << std::endl;
        }
    });

    // Open() 会根据 H.264 的 codec、time_base 和 SPS/PPS extra_data 创建
    // AVCodecContext。若这一步失败，继续读取包没有意义。
    if (!decoder.Open(video_stream_info)) {
        std::cerr << "Failed to open video decoder" << std::endl;
        puller.Close();
        return 1;
    }

    if (!encoder.Open(encoder_config)) {
        std::cerr << "Failed to open video encoder" << std::endl;
        decoder.Close();
        puller.Close();
        return 1;
    }

    int input_packet_count = 0;
    int video_packet_count = 0;
    // 本测试的通过条件是解码出 10 帧视频，而不是收到 10 个视频包。
    // 对 H.264 而言，编码包和解码帧并非一一对应：接入直播流后可能要先
    // 等待关键帧，且 B 帧重排序也会影响某个包何时产生输出帧。
    constexpr int kExpectedDecodedFrameCount = 10;

    // 限制的是“读取尝试次数”，防止服务异常或始终没有关键帧时测试无限
    // 等待。循环退出的正常条件是收到了足够的视频解码帧，而不是读到了
    // 固定数量的音视频混合包。
    for (int attempt = 0;
        attempt < kMaxReadAttempts &&
        decoded_frames < kExpectedDecodedFrameCount;
        ++attempt) {
        const PullReadResult read_result = puller.ReadPacket();

        if (read_result.status == PullReadStatus::NoData) {
            // NoData 不代表输入结束；live RTSP 在暂无可交付包时可正常返回它。
            continue;
        }

        if (read_result.status != PullReadStatus::Packet ||
            !read_result.packet ||
            !read_result.packet->buffer ||
            read_result.packet->buffer->Size() == 0) {
            std::cerr << "Read failed at attempt " << (attempt + 1)
                << ": status="
                << static_cast<int>(read_result.status);
            if (read_result.error.has_value()) {
                std::cerr << ", error=" << read_result.error->message;
            }
            std::cerr << std::endl;
            puller.Close();
            return 1;
        }

        ++input_packet_count;
        std::cout << "Input packet " << input_packet_count
            << ": type="
            << static_cast<int>(read_result.packet->type)
            << ", codec="
            << static_cast<int>(read_result.packet->codec)
            << ", stream_index=" << read_result.packet->stream_index
            << ", size=" << read_result.packet->buffer->Size()
            << std::endl;

        // 当前 RTSP 流有 audio(stream_index=1) 和 video(stream_index=0)。
        // 音频 G.711A 数据没有 H.264 NAL 起始码；若送入 H.264 decoder，
        // FFmpeg 必然打印 "No start code is found"。因此这里直接跳过
        // 不属于当前视频轨道的包。
        if (read_result.packet->stream_index != video_stream_info.stream_index) {
            continue;
        }

        // stream_index 是首要依据；type/codec 的检查是测试层防御，能够在
        // Puller 的媒体元数据与实际视频轨道不一致时给出清晰报错。
        if (read_result.packet->type != MediaType::VIDEO ||
            read_result.packet->codec != video_stream_info.codec_type) {
            std::cerr << "Video stream packet metadata does not match decoder"
                << std::endl;
            decoder.Close();
            puller.Close();
            return 1;
        }

        ++video_packet_count;
        if (!decoder.Decode(read_result.packet)) {
            std::cerr << "Video decode failed at video packet "
                << video_packet_count << std::endl;
            decoder.Close();
            encoder.Close();
            puller.Close();
            return 1;
        }
    }

    // H.264 可能缓存 B 帧。即使读取循环已经停止，也要先向 decoder 发送
    // 空包进行 drain，才能拿到所有已接收编码包对应的尾部输出帧。
    if (!decoder.Flush()) {
        std::cerr << "Failed to flush video decoder" << std::endl;
        decoder.Close();
        encoder.Close();
        puller.Close();
        return 1;
    }

    std::vector<PacketPtr> flush_packets;
    if (!encoder.Flush(flush_packets)) {
        std::cerr << "Failed to flush video encoder" << std::endl;
        decoder.Close();
        encoder.Close();
        puller.Close();
        return 1;
    }

    for (const auto& packet : flush_packets) {
        ++encoded_packets;
        total_encoded_bytes += packet->buffer ? packet->buffer->Size() : 0;
        std::cout << "Flush packet " << encoded_packets
            << ": size=" << (packet->buffer ? packet->buffer->Size() : 0)
            << ", pts=" << packet->pts
            << ", keyframe=" << packet->keyframe
            << std::endl;
    }

    // 先 Flush 再 Close：Close 只释放 AVCodecContext，不会主动输出缓存帧。
    decoder.Close();
    encoder.Close();
    puller.Close();

    if (decoded_frames < kExpectedDecodedFrameCount) {
        std::cerr << "Only decoded " << decoded_frames << " video frame(s) from "
            << video_packet_count << " video packet(s) after "
            << input_packet_count << " input packet(s); expected "
            << kExpectedDecodedFrameCount << std::endl;
        return 1;
    }

    // 成功结果只表达测试真正验证的条件：已经解码出目标数量的视频帧。
    // video_packet_count 是过程诊断数据，不能作为“10 帧必须来自 10 包”
    // 的断言依据。
    std::cout << "FFmpegPuller -> FFmpegDecoder -> MediaFrameConverter -> FFmpegEncoder test passed: "
        << decoded_frames << " video frame(s) decoded, "
        << converted_frames << " frame(s) converted, "
        << encoded_frames << " frame(s) encoded, "
        << encoded_packets << " packet(s) output" << std::endl;
    std::cout << "Diagnostic: " << video_packet_count
        << " video packet(s) consumed from " << input_packet_count
        << " input packet(s)" << std::endl;
    std::cout << "Total encoded bytes: " << total_encoded_bytes
        << " (" << (total_encoded_bytes / 1024) << " KB)" << std::endl;
    return 0;
}

}  // namespace

int main() {
    // 当前先运行不依赖网络的 converter 测试。
    // 需要重新验证 RTSP 解码链路时，可改为 RunFfmpegPullerDecoderTest()。
    //return RunFfmpegConverterTest();
    RunFfmpegPullerDecoderConverterEncoderTest();
}