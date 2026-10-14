#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>

#include "engine/openvino_engine.h"
#include "media/media_frame.h"
#include "media/simple_buffer.h"
#include "model/yolo_model.h"
#include "media/puller/ffmpeg_puller.h"
#include "media/decoder/ffmpeg_decoder.h"
#include "render/video/opengl_video_renderer.h"
#include "common/log/logger.h"

#ifndef VIDEO_MONITOR_MODEL_ROOT
#define VIDEO_MONITOR_MODEL_ROOT "."
#endif

namespace {

constexpr const char* kRtspUri = "rtsp://192.168.66.83/live/mainstream";
constexpr int kMaxReadAttempts = INT_MAX;

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

MediaFrame MakeI420Frame(int width, int height) {
    const auto y_size = static_cast<size_t>(width) * static_cast<size_t>(height);
    const auto uv_size = y_size / 4;
    std::vector<uint8_t> data(y_size + uv_size * 2, 128);

    VideoFrameMeta meta;
    meta.pixel_format = PixelFormat::kI420;
    meta.width = width;
    meta.height = height;
    meta.plane_count = 3;
    meta.plane_info[0] = {0, width, static_cast<int32_t>(y_size)};
    meta.plane_info[1] = {static_cast<int32_t>(y_size), width / 2,
                           static_cast<int32_t>(uv_size)};
    meta.plane_info[2] = {static_cast<int32_t>(y_size + uv_size), width / 2,
                           static_cast<int32_t>(uv_size)};

    MediaFrame frame;
    frame.type = MediaType::VIDEO;
    frame.time.pts_us = 1'000;
    frame.meta = meta;
    frame.buffer = std::make_shared<SimpleBuffer>(std::move(data));
    return frame;
}

int RunFfmpegPullerDecoderInferenceRendererTest() {
    const auto model_path = std::filesystem::path(VIDEO_MONITOR_MODEL_ROOT) / "yolov5/yolov5s.xml";
    if (!std::filesystem::exists(model_path)) {
        LOG_ERROR("Missing YOLOv5 model: {}", model_path.string());
        return 1;
    }

    auto puller_config = testPullerConfig();
    auto endpoint = testInputEndpoint();

    FFmpegPuller puller(std::move(puller_config));

    LOG_INFO("Opening RTSP stream: {}", endpoint.uri);
    const PullOpenResult open_result = puller.Open(endpoint);
    if (!open_result.Succeed()) {
        LOG_ERROR("Open failed: {}", (open_result.error.has_value()
                          ? open_result.error->message
                          : "unknown error"));
        return 1;
    }

    const MultiStreamInfo stream_info = puller.GetStreamInfo();
    if (!stream_info.HasVideoStream()) {
        LOG_ERROR("Open succeeded, but no video stream was found");
        puller.Close();
        return 1;
    }

    LOG_INFO("Stream info: video stream idx={}, {}x{}",
             stream_info.video_stream_idx_,
             stream_info.stream_infos[stream_info.video_stream_idx_].video().width,
             stream_info.stream_infos[stream_info.video_stream_idx_].video().height);

    const MediaStreamInfo& video_stream_info =
        stream_info.stream_infos[stream_info.video_stream_idx_];

    render::RenderConfig render_config;
    render_config.window_width = video_stream_info.video().width > 0 ? video_stream_info.video().width : 640;
    render_config.window_height = video_stream_info.video().height > 0 ? video_stream_info.video().height : 480;
    render_config.visible = true;
    render_config.vsync = false;
    render_config.title = "RTSP Stream Inference Renderer";

    render::OpenGLVideoRenderer renderer;
    if (!renderer.Init(render_config)) {
        LOG_ERROR("Failed to initialize renderer: {}", renderer.LastError());
        puller.Close();
        return 1;
    }

    ModelConfig model_config;
    model_config.name = "yolov5s";
    model_config.class_count = 80;
    model_config.options["input_width"] = "640";
    model_config.options["input_height"] = "640";

    YoloModel model;
    if (!model.Initialize(model_config)) {
        LOG_ERROR("YoloModel initialization failed");
        renderer.Shutdown();
        puller.Close();
        return 1;
    }

    OpenVinoEngine engine;
    EngineLoadConfig engine_config;
    engine_config.engine.model_path = model_path.string();
    engine_config.engine.device = "CPU";
    engine_config.engine.request_count = 1;

    OpenVinoPreprocessConfig preprocess;
    preprocess.enabled = true;
    preprocess.input_pixel_format = PixelFormat::kI420;
    preprocess.model_pixel_format = PixelFormat::kRGB24;
    preprocess.model_input_layout = "NCHW";
    preprocess.scale = 255.0f;
    engine_config.preprocess = preprocess;

    if (!engine.LoadModel(engine_config)) {
        LOG_ERROR("OpenVINO model loading failed");
        renderer.Shutdown();
        puller.Close();
        return 1;
    }

    const auto model_status = model.ValidateModelDesc(engine.GetModelDesc());
    if (!model_status.Ok()) {
        LOG_ERROR("Model/output validation failed: {}", model_status.message);
        engine.Release();
        renderer.Shutdown();
        puller.Close();
        return 1;
    }

    FFmpegDecoder decoder;
    int decoded_frames = 0;
    int inferred_frames = 0;
    int rendered_frames = 0;
    int packet_count = 0;
    std::atomic<bool> window_close_requested{false};

    decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (!frame || frame->type != MediaType::VIDEO) {
            return;
        }

        ++decoded_frames;
        LOG_INFO("Decoded frame {}: {}x{}, pixel_format={}, pts_us={}",
                  decoded_frames, frame->Width(), frame->Height(),
                  static_cast<int>(frame->PixelFormat()), frame->time.pts_us);

        TensorFrame input;
        const auto preprocess_status = model.PreprocessWithStatus(*frame, input);
        if (!preprocess_status.Ok()) {
            LOG_ERROR("Preprocess failed for frame {}: {}", decoded_frames, preprocess_status.message);
            return;
        }

        TensorFrame output;
        const auto infer_status = engine.InferWithStatus(input, output);
        if (!infer_status.Ok()) {
            LOG_ERROR("Inference failed for frame {}: {}", decoded_frames, infer_status.message);
            return;
        }

        FrameResult result;
        const auto postprocess_status = model.PostprocessWithStatus(output, result);
        if (!postprocess_status.Ok()) {
            LOG_ERROR("Postprocess failed for frame {}: {}", decoded_frames, postprocess_status.message);
            return;
        }

        ++inferred_frames;
        LOG_INFO("Frame {} inference completed, objects={}", decoded_frames, result.objects.size());

        if (renderer.Render(*frame)) {
            ++rendered_frames;
        } else {
            LOG_ERROR("Render failed for frame {}", decoded_frames);
        }

        renderer.PollEvents();
        if (renderer.ShouldClose()) {
            window_close_requested = true;
        }
    });

    if (!decoder.Open(video_stream_info)) {
        LOG_ERROR("Failed to open video decoder");
        engine.Release();
        renderer.Shutdown();
        puller.Close();
        return 1;
    }

    for (int attempt = 0; !window_close_requested; ++attempt) {
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
                      attempt, (read_result.error.has_value() ? read_result.error->message : "unknown error"));
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
    }

    if (!decoder.Flush()) {
        LOG_ERROR("Failed to flush video decoder");
    }

    decoder.Close();
    engine.Release();
    puller.Close();

    LOG_INFO("Decoded {} frame(s), inferred {} frame(s), rendered {} frame(s)",
             decoded_frames, inferred_frames, rendered_frames);

    if (decoded_frames == 0) {
        LOG_ERROR("No frames were decoded");
        renderer.Shutdown();
        return 1;
    }

    if (inferred_frames == 0) {
        LOG_ERROR("No frames were inferred");
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
    LOG_INFO("FFmpegPuller Decoder Inference Renderer test passed");
    return 0;
}

}  // namespace

int main() {
    return RunFfmpegPullerDecoderInferenceRendererTest();
}