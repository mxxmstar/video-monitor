/// @file test_ffmpeg_puller.cpp
/// @brief FFmpegPuller 的 RTSP 集成测试。

#include "media/puller/ffmpeg_puller.h"
#include "media/decoder/ffmpeg_decoder.h"
#include "media/converter/ffmpeg_audio_converter.h"
#include "media/converter/ffmpeg_video_converter.h"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <utility>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace {

constexpr const char* kRtspUri = "rtsp://192.168.66.83/live/mainstream";
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
                  << static_cast<int>(frame->GetPixelFormat())
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
    // 本测试不连接 RTSP，而是在内存中构造两种简单的 AVFrame，分别验证：
    //
    //   1. FFmpegVideoConverter 是否可以调用 sws_scale 完成视频转换；
    //   2. FFmpegAudioConverter 是否可以调用 swr_convert 完成音频转换。
    //
    // 使用合成帧的好处是输入尺寸、格式、采样率和样本数完全确定。这样
    // 测试失败时，可以先排除网络、解码器和媒体流内容带来的干扰。
    std::cout << "Running FFmpeg converter test" << std::endl;

    // --------------------------- 视频转换测试 ---------------------------
    // 构造一帧 4x4 的 YUV420P 视频。4x4 只是为了让测试数据简单：
    // Y 平面是 4x4，U/V 平面各是 2x2。
    AVFrame* video_input = av_frame_alloc();
    if (!video_input) {
        std::cerr << "Failed to allocate video input frame" << std::endl;
        return 1;
    }

    video_input->format = AV_PIX_FMT_YUV420P;
    video_input->width = 4;
    video_input->height = 4;
    video_input->pts = 123;
    int ret = av_frame_get_buffer(video_input, 0);
    if (ret < 0) {
        std::cerr << "Failed to allocate video input buffer" << std::endl;
        av_frame_free(&video_input);
        return 1;
    }

    // 给三个平面填充可识别的值。这里不验证具体颜色，只验证转换后有
    // 合法输出；转换算法的具体数值属于 swscale 的实现细节。
    for (int row = 0; row < video_input->height; ++row) {
        for (int col = 0; col < video_input->width; ++col) {
            video_input->data[0][row * video_input->linesize[0] + col] =
                static_cast<uint8_t>(16 + row * video_input->width + col);
        }
    }
    for (int row = 0; row < video_input->height / 2; ++row) {
        for (int col = 0; col < video_input->width / 2; ++col) {
            video_input->data[1][row * video_input->linesize[1] + col] = 90;
            video_input->data[2][row * video_input->linesize[2] + col] = 160;
        }
    }

    FFmpegVideoConverter video_converter;
    if (!video_converter.Open(2, 2, AV_PIX_FMT_NV12, SWS_BILINEAR)) {
        std::cerr << "Failed to open video converter: "
                  << video_converter.LastError() << std::endl;
        av_frame_free(&video_input);
        return 1;
    }

    AVFrame* video_output = video_converter.Convert(video_input);
    if (!video_output) {
        std::cerr << "Video conversion failed: "
                  << video_converter.LastError() << std::endl;
        video_converter.Close();
        av_frame_free(&video_input);
        return 1;
    }

    // NV12 是两个平面：Y 平面和交错的 UV 平面。这里只检查转换接口
    // 的输出契约，不把 swscale 的颜色计算结果写死在测试中。
    const bool video_ok =
        video_output->width == 2 &&
        video_output->height == 2 &&
        video_output->format == AV_PIX_FMT_NV12 &&
        video_output->data[0] != nullptr &&
        video_output->data[1] != nullptr &&
        video_output->pts == video_input->pts;

    if (!video_ok) {
        std::cerr << "Video conversion output is invalid" << std::endl;
        av_frame_free(&video_output);
        video_converter.Close();
        av_frame_free(&video_input);
        return 1;
    }

    std::cout << "Video conversion passed: YUV420P 4x4 -> NV12 2x2"
              << std::endl;
    av_frame_free(&video_output);
    video_converter.Close();
    av_frame_free(&video_input);

    // --------------------------- 音频转换测试 ---------------------------
    // 构造 160 个 8000Hz 单声道 S16P 样本，约对应 20ms 音频。
    // S16P 的每个声道拥有独立平面；单声道时只有 extended_data[0]。
    AVFrame* audio_input = av_frame_alloc();
    if (!audio_input) {
        std::cerr << "Failed to allocate audio input frame" << std::endl;
        return 1;
    }

    audio_input->format = AV_SAMPLE_FMT_S16P;
    audio_input->sample_rate = 8000;
    audio_input->nb_samples = 160;
    audio_input->pts = 456;
    ret = av_channel_layout_from_mask(&audio_input->ch_layout,
                                      AV_CH_LAYOUT_MONO);
    if (ret < 0) {
        std::cerr << "Failed to set audio input channel layout" << std::endl;
        av_frame_free(&audio_input);
        return 1;
    }
    ret = av_frame_get_buffer(audio_input, 0);
    if (ret < 0) {
        std::cerr << "Failed to allocate audio input buffer" << std::endl;
        av_frame_free(&audio_input);
        return 1;
    }

    auto* audio_samples = reinterpret_cast<int16_t*>(audio_input->data[0]);
    for (int i = 0; i < audio_input->nb_samples; ++i) {
        // 使用递增波形即可，测试重点是格式转换链路，不是音频内容质量。
        audio_samples[i] = static_cast<int16_t>((i % 32) * 500);
    }

    FFmpegAudioConverter audio_converter;
    if (!audio_converter.Open(AV_CH_LAYOUT_STEREO,
                              16000,
                              AV_SAMPLE_FMT_S16)) {
        std::cerr << "Failed to open audio converter: "
                  << audio_converter.LastError() << std::endl;
        av_frame_free(&audio_input);
        return 1;
    }

    AVFrame* audio_output = audio_converter.Convert(audio_input);
    if (!audio_output) {
        std::cerr << "Audio conversion failed: "
                  << audio_converter.LastError() << std::endl;
        audio_converter.Close();
        av_frame_free(&audio_input);
        return 1;
    }

    // 8000Hz -> 16000Hz 后，样本数应大致翻倍；具体边界可能受 swr 延迟
    // 和重采样实现影响，所以这里只要求输出为正数，而不写死精确数量。
    const bool audio_ok =
        audio_output->format == AV_SAMPLE_FMT_S16 &&
        audio_output->sample_rate == 16000 &&
        audio_output->ch_layout.nb_channels == 2 &&
        audio_output->nb_samples > 0 &&
        audio_output->data[0] != nullptr &&
        audio_output->pts == audio_input->pts;

    if (!audio_ok) {
        std::cerr << "Audio conversion output is invalid" << std::endl;
        av_frame_free(&audio_output);
        audio_converter.Close();
        av_frame_free(&audio_input);
        return 1;
    }

    std::cout << "Audio conversion passed: S16P 8000Hz mono -> "
              << "S16 16000Hz stereo, " << audio_output->nb_samples
              << " output sample(s)" << std::endl;

    av_frame_free(&audio_output);
    audio_converter.Close();
    av_frame_free(&audio_input);

    std::cout << "FFmpeg converter test passed" << std::endl;
    return 0;
}

}  // namespace

int main() {
    // 当前先运行不依赖网络的 converter 测试。
    // 需要重新验证 RTSP 解码链路时，可改为 RunFfmpegPullerDecoderTest()。
    return RunFfmpegConverterTest();
}
