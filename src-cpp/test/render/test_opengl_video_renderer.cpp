#include "render/video/opengl_video_renderer.h"
#include "media/simple_buffer.h"
#include "media/ffmpeg_raw_frame_buffer.h"

#include <glad/glad.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#include "media/puller/ffmpeg_puller.h"
#include "media/decoder/ffmpeg_decoder.h"
#include "media/converter/media_frame_converter.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/publisher/publisher.h"
#include "media/ffmpeg_raw_frame_buffer.h"
#include "media/simple_buffer.h"
#include "common/log/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <utility>
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

constexpr int kMaxTestSeconds = 180;
constexpr const char* kRtspPushOutputUrl = "rtsp://127.0.0.1/live/test_zlm_rtsp";

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

bool Check(bool result, const char* message) {
    if (!result) std::cerr << message << '\n';
    return result;
}

MediaFrame MakeFrame(PixelFormat format) {
    MediaFrame frame;
    VideoFrameMeta meta;
    meta.width = 3;
    meta.height = 3;
    meta.pixel_format = format;
    const bool rgb = format == PixelFormat::kRGB24 || format == PixelFormat::kBGR24;
    const bool gray = format == PixelFormat::kGRAY8;
    const bool planar = format == PixelFormat::kI420;
    meta.plane_count = rgb || gray ? 1 : (planar ? 3 : 2);
    meta.plane_info[0] = {5, rgb ? 11 : 5, 0};
    meta.plane_info[1] = {24, planar ? 3 : 5, 0};
    meta.plane_info[2] = {33, 3, 0};
    std::vector<std::uint8_t> bytes(48, 0);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            auto* pixel = bytes.data() + 5 + y * meta.plane_info[0].stride + x * (rgb ? 3 : 1);
            if (rgb) pixel[format == PixelFormat::kRGB24 ? 0 : 2] = 255;
            else pixel[0] = gray ? 128 : 81;
        }
    }
    if (!rgb && !gray) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                if (planar) {
                    bytes[24 + y * 3 + x] = 90;
                    bytes[33 + y * 3 + x] = 240;
                } else {
                    auto* uv = bytes.data() + 24 + y * 5 + x * 2;
                    uv[format == PixelFormat::kNV12 ? 0 : 1] = 90;
                    uv[format == PixelFormat::kNV12 ? 1 : 0] = 240;
                }
            }
        }
    }
    frame.meta = meta;
    frame.buffer = std::make_shared<SimpleBuffer>(std::move(bytes));
    return frame;
}

bool CheckPixel(render::OpenGLVideoRenderer& renderer, const MediaFrame& frame,
                std::array<int, 3> expected) {
    // Use an FBO so hidden-window pixel ownership does not affect readback.
    if (!Check(renderer.Render(frame), renderer.LastError().c_str())) return false;
    GLuint fbo = 0, texture = 0;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (!Check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
               "incomplete test framebuffer")) return false;
    const bool rendered = renderer.Render(frame);
    std::array<unsigned char, 4> pixel{};
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
    if (!Check(rendered, renderer.LastError().c_str())) return false;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(static_cast<int>(pixel[i]) - expected[i]) > 3) {
            std::cerr << "format=" << static_cast<int>(frame.PixelFormat())
                      << " actual=" << static_cast<int>(pixel[0]) << ','
                      << static_cast<int>(pixel[1]) << ',' << static_cast<int>(pixel[2])
                      << " expected=" << expected[0] << ',' << expected[1] << ','
                      << expected[2] << '\n';
            return false;
        }
    }
    return Check(glGetError() == GL_NO_ERROR, "OpenGL error");
}


int RunFfmpegPullerDecoderRendererTest() {
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

    const MediaStreamInfo& video_stream_info =
        stream_info.stream_infos[stream_info.video_stream_idx_];

    render::RenderConfig render_config;
    render_config.window_width = video_stream_info.video().width > 0 ? video_stream_info.video().width : 640;
    render_config.window_height = video_stream_info.video().height > 0 ? video_stream_info.video().height : 480;
    render_config.visible = true;
    render_config.vsync = false;
    render_config.title = "RTSP Stream Renderer";

    render::OpenGLVideoRenderer renderer;
    if (!renderer.Init(render_config)) {
        LOG_ERROR("Failed to initialize renderer: {}", renderer.LastError());
        puller.Close();
        return 1;
    }

    FFmpegDecoder decoder;
    int decoded_frames = 0;
    int rendered_frames = 0;
    std::atomic<bool> window_close_requested{false};

    decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (!frame || frame->type != MediaType::VIDEO) {
            return;
        }

        ++decoded_frames;
        LOG_INFO("Decoded frame {}: {}x{}, pixel_format={}, pts_us={}",
                  decoded_frames, frame->Width(), frame->Height(),
                  static_cast<int>(frame->PixelFormat()), frame->time.pts_us);

        if (renderer.Render(*frame)) {
            ++rendered_frames;
        } else {
            LOG_ERROR("Render failed for frame {}: {}", decoded_frames, renderer.LastError());
        }

        renderer.PollEvents();
        if (renderer.ShouldClose()) {
            window_close_requested = true;
        }
    });

    if (!decoder.Open(video_stream_info)) {
        LOG_ERROR("Failed to open video decoder");
        renderer.Shutdown();
        puller.Close();
        return 1;
    }
    

    int packet_count = 0;
    for (int attempt = 0;
         attempt < kMaxReadAttempts/* && decoded_frames < kExpectedFrameCount*/ && !window_close_requested;
         ++attempt) {
        const PullReadResult read_result = puller.ReadPacket();

        if (read_result.status == PullReadStatus::NoData) {
            renderer.PollEvents();
            if (renderer.ShouldClose()) {
                window_close_requested = true;
            }
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
            break;
        }

        if (read_result.packet->stream_index != video_stream_info.stream_index) {
            continue;
        }

        if (read_result.packet->type != MediaType::VIDEO ||
            read_result.packet->codec != video_stream_info.codec_type) {
            LOG_ERROR("Video stream packet metadata does not match decoder");
            break;
        }

        ++packet_count;
        if (!decoder.Decode(read_result.packet)) {
            LOG_ERROR("Video decode failed at video packet {}", packet_count);
            break;
        }
        
        LOG_INFO("Packet {}: type={}, codec={}, stream_index={}, size={}",
                 packet_count, static_cast<int>(read_result.packet->type),
                 static_cast<int>(read_result.packet->codec), read_result.packet->stream_index,
                 read_result.packet->buffer->Size());
    }

    if (!decoder.Flush()) {
        LOG_ERROR("Failed to flush video decoder");
    }

    decoder.Close();
    puller.Close();

    LOG_INFO("Decoded {} frame(s), rendered {} frame(s)", decoded_frames, rendered_frames);
    
    if (decoded_frames == 0) {
        LOG_ERROR("No frames were decoded");
        renderer.Shutdown();
        return 1;
    }

    if (rendered_frames == 0) {
        LOG_ERROR("No frames were rendered");
        renderer.Shutdown();
        return 1;
    }

    LOG_INFO("Keeping window open for manual inspection. Close window to exit...");
    while (!renderer.ShouldClose()) {
        renderer.PollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    renderer.Shutdown();
    LOG_INFO("FFmpegPuller RTSP test passed");
    return 0;
}

}

int main() {
#if 0    
    render::RenderConfig config;
    config.window_width = config.window_height = 480;
    config.visible = config.vsync = true;
    render::OpenGLVideoRenderer renderer;
    if (!Check(renderer.Init(config), renderer.LastError().c_str())) return 1;
    for (const auto format : {PixelFormat::kRGB24, PixelFormat::kBGR24, PixelFormat::kGRAY8,
                              PixelFormat::kNV12, PixelFormat::kNV21, PixelFormat::kI420}) {
        const auto expected = format == PixelFormat::kGRAY8 ?
            std::array<int, 3>{128, 128, 128} : std::array<int, 3>{255, 0, 0};
        if (!CheckPixel(renderer, MakeFrame(format), expected)) return 1;
    }
    AVFrame* raw = av_frame_alloc();
    if (!Check(raw != nullptr, "AVFrame allocation failed")) return 1;
    raw->format = AV_PIX_FMT_RGB24;
    raw->width = 5;
    raw->height = 3;
    if (av_frame_get_buffer(raw, 32) < 0) { av_frame_free(&raw); return 1; }
    for (int y = 0; y < raw->height; ++y) {
        for (int x = 0; x < raw->width; ++x) {
            auto* pixel = raw->data[0] + y * raw->linesize[0] + x * 3;
            pixel[0] = 0; pixel[1] = 255; pixel[2] = 0;
        }
    }
    MediaFrame raw_frame;
    VideoFrameMeta meta;
    meta.width = raw->width; meta.height = raw->height;
    meta.pixel_format = PixelFormat::kRGB24; meta.plane_count = 1;
    raw_frame.meta = meta;
    raw_frame.buffer = std::make_shared<FFmpegRawFrameBuffer>(raw);
    if (!CheckPixel(renderer, raw_frame, {0, 255, 0})) return 1;
    raw->data[0] += (raw->height - 1) * raw->linesize[0];
    raw->linesize[0] = -raw->linesize[0];
    if (!CheckPixel(renderer, raw_frame, {0, 255, 0})) return 1;

    auto invalid = MakeFrame(PixelFormat::kRGB24);
    invalid.buffer = std::make_shared<SimpleBuffer>();
    if (!Check(!renderer.Render(invalid), "empty buffer accepted")) return 1;
    invalid = MakeFrame(PixelFormat::kRGB24);
    std::get<VideoFrameMeta>(invalid.meta).plane_count = 9;
    if (!Check(!renderer.Render(invalid), "invalid plane count accepted")) return 1;
    invalid.type = MediaType::AUDIO;
    if (!Check(!renderer.Render(invalid), "audio frame accepted")) return 1;
    render::OpenGLVideoRenderer second;
    if (!Check(second.Init(config), "second renderer initialization failed")) return 1;
    second.Shutdown();
    if (!CheckPixel(renderer, MakeFrame(PixelFormat::kRGB24), {255, 0, 0})) return 1;
    renderer.PollEvents();
    renderer.Shutdown();
    renderer.Shutdown();
    if (!Check(renderer.Init(config), "renderer reinitialization failed")) return 1;
    if (!CheckPixel(renderer, MakeFrame(PixelFormat::kRGB24), {255, 0, 0})) return 1;
    std::cout << "OpenGL renderer pixel and lifecycle checks passed\n";
    return 0;
#endif
    return RunFfmpegPullerDecoderRendererTest();    
}