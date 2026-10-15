#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#include <array>
#include <climits>
#include <cmath>

#include "media/ffmpeg_raw_frame_buffer.h"
#include "render/osd/osd_render_impl.h"
#include "engine/openvino_engine.h"
#include "media/media_frame.h"
#include "media/simple_buffer.h"
#include "model/yolo_model.h"
#include "media/puller/ffmpeg_puller.h"
#include "media/decoder/ffmpeg_decoder.h"
#include "render/video/opengl_video_renderer.h"
#include "common/log/logger.h"
#include "render/osd/osd_render.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

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

template <typename View>
void DrawDetectionBoxes(View& view, const FrameResult& result) {
    constexpr int thickness = 2;
    for (const auto& object : result.objects) {
        const auto& rect = object.object.rect;
        const double right = static_cast<double>(rect.x) + rect.width;
        const double bottom = static_cast<double>(rect.y) + rect.height;
        if (!std::isfinite(rect.x) || !std::isfinite(rect.y) ||
            !std::isfinite(right) || !std::isfinite(bottom) || rect.width <= 0 || rect.height <= 0) {
            continue;
        }
        const int x1 = static_cast<int>(std::floor(std::clamp<double>(rect.x, 0, view.width)));
        const int y1 = static_cast<int>(std::floor(std::clamp<double>(rect.y, 0, view.height)));
        const int x2 = static_cast<int>(std::ceil(std::clamp<double>(right, 0, view.width)));
        const int y2 = static_cast<int>(std::ceil(std::clamp<double>(bottom, 0, view.height)));
        if (x1 >= x2 || y1 >= y2) {
            continue;
        }
        const int horizontal = std::min(thickness, y2 - y1);
        const int vertical = std::min(thickness, x2 - x1);
        const auto color = OsdColors::Yuv::kGreen;
        FillTextRect(view, x1, y1, x2 - x1, horizontal, color);
        FillTextRect(view, x1, y2 - horizontal, x2 - x1, horizontal, color);
        FillTextRect(view, x1, y1, vertical, y2 - y1, color);
        FillTextRect(view, x2 - vertical, y1, vertical, y2 - y1, color);
    }
}

bool DrawDetectionBoxes(MediaFrame& frame, const FrameResult& result) {
    if (result.objects.empty()) {
        return true;
    }
    const auto* meta = frame.VideoMeta();
    if (!meta || !frame.buffer || meta->width <= 0 || meta->height <= 0 || meta->width == INT_MAX || meta->height == INT_MAX) {
        return false;
    }
    const bool planar = meta->pixel_format == PixelFormat::kI420;
    if (!planar && meta->pixel_format != PixelFormat::kNV12 && meta->pixel_format != PixelFormat::kNV21) {
        return false;
    }
    const int count = planar ? 3 : 2;
    const int chroma_width = (meta->width + 1) / 2;
    if (!planar && chroma_width > INT_MAX / 2) {
        return false;
    }
    std::array<std::uint8_t*, 3> planes{};
    std::array<std::size_t, 3> strides{};
    auto* raw = dynamic_cast<FFmpegRawFrameBuffer*>(frame.buffer.get());
    // Decoded raw frames may still share storage with the decoder's reference frames.
    if (raw && (!raw->IsValid() || av_frame_make_writable(raw->GetFrame()) < 0)) {
        return false;
    }
    for (int index = 0; index < count; ++index) {
        const int row_bytes = index == 0 ? meta->width : chroma_width * (planar ? 1 : 2);
        const int rows = index == 0 ? meta->height : (meta->height + 1) / 2;
        const int stride = raw ? raw->PlaneStride(index) : frame.Stride(index);
        if (stride < row_bytes) {
            return false;
        }
        strides[index] = static_cast<std::size_t>(stride);
        if (raw) {
            planes[index] = raw->PlaneData(index);
        } else {
            if (!frame.buffer->IsContiguous() || !frame.buffer->Data() ||
                meta->plane_count < count || frame.PlaneOffset(index) < 0) {
                return false;
            }
            const auto offset = static_cast<std::size_t>(frame.PlaneOffset(index));
            const auto required = strides[index] * (rows - 1) + row_bytes;
            if (offset > frame.buffer->Size() || required > frame.buffer->Size() - offset) {
                return false;
            }
            planes[index] = frame.buffer->Data() + offset;
        }
        if (!planes[index]) {
            return false;
        }
    }
    if (planar) {
        I420View view{planes[0], planes[1], planes[2], meta->width, meta->height,
                      strides[0], strides[1], strides[2]};
        DrawDetectionBoxes(view, result);
    } else {
        Nv12View view{planes[0], planes[1], meta->width, meta->height,
                      strides[0], strides[1], meta->pixel_format == PixelFormat::kNV21};
        DrawDetectionBoxes(view, result);
    }
    return true;
}

bool DrawDetectionBoxes2(MediaFrame& frame, const FrameResult& result) {

    osd::OverlayBatch batch;
    auto textoverlay = std::make_shared<OverlayText>();
    textoverlay->x = 100;
    textoverlay->y = 100;
    textoverlay->text = "Hello, World!";
    textoverlay->scale = 5;
    textoverlay->color = OsdColors::Yuv::kRed;
    batch.Add(std::move(textoverlay));



    const auto* meta = frame.VideoMeta();
    if (!meta || !frame.buffer || meta->width <= 0 || meta->height <= 0 || meta->width == INT_MAX || meta->height == INT_MAX) {
        return false;
    }
    if (meta->pixel_format != PixelFormat::kI420 &&
        meta->pixel_format != PixelFormat::kNV12 &&
        meta->pixel_format != PixelFormat::kNV21) {
        return false;
    }
    
    constexpr int thickness = 2;
    for (const auto& object : result.objects) {
        const auto& rect = object.object.rect;
        const double right = static_cast<double>(rect.x) + rect.width;
        const double bottom = static_cast<double>(rect.y) + rect.height;
        if (!std::isfinite(rect.x) || !std::isfinite(rect.y) ||
            !std::isfinite(right) || !std::isfinite(bottom) || rect.width <= 0 || rect.height <= 0) {
            continue;
        }
        const int x1 = static_cast<int>(std::floor(std::clamp<double>(rect.x, 0, meta->width)));
        const int y1 = static_cast<int>(std::floor(std::clamp<double>(rect.y, 0, meta->height)));
        const int x2 = static_cast<int>(std::ceil(std::clamp<double>(right, 0, meta->width)));
        const int y2 = static_cast<int>(std::ceil(std::clamp<double>(bottom, 0, meta->height)));
        if (x1 >= x2 || y1 >= y2) {
            continue;
        }

        auto overlay = std::make_shared<OverlayRect>();
        overlay->x = x1;
        overlay->y = y1;
        overlay->width = x2 - x1;
        overlay->height = y2 - y1;
        overlay->color = OsdColors::Yuv::kGreen;
        overlay->thickness = thickness;
        batch.Add(std::move(overlay));
    }





    if (batch.Empty()) {
        return true;
    }

    auto* raw = dynamic_cast<FFmpegRawFrameBuffer*>(frame.buffer.get());
    if (raw && (!raw->IsValid() || av_frame_make_writable(raw->GetFrame()) < 0)) {
        return false;
    }

    switch (meta->pixel_format) {
    case PixelFormat::kI420:
        return DrawOverlays<osd::I420OsdRenderer>(frame, batch) == OsdStatus::kOk;
    case PixelFormat::kNV12:
    case PixelFormat::kNV21:
        return DrawOverlays<osd::Nv12OsdRenderer>(frame, batch) == OsdStatus::kOk;
    default:
        return false;
    }
}

int TestDetectionBoxes() {
    FrameResult result;
    ObjectResult object;
    object.object.rect = {4, 4, 12, 12};
    result.objects.push_back(object);
    auto packed = MakeI420Frame(24, 24);
    auto* pixels = packed.buffer->Data();
    if (!DrawDetectionBoxes(packed, result) ||
        pixels[4 * 24 + 4] != OsdColors::Yuv::kGreen.y ||
        pixels[15 * 24 + 15] != OsdColors::Yuv::kGreen.y ||
        pixels[8 * 24 + 8] != 128 || pixels[0] != 128 ||
        pixels[24 * 24 + 2 * 12 + 2] != OsdColors::Yuv::kGreen.u ||
        pixels[24 * 24 + 12 * 12 + 2 * 12 + 2] != OsdColors::Yuv::kGreen.v) {
        return 1;
    }
    result.objects[0].object.rect = {-4, -4, 12, 12};
    if (!DrawDetectionBoxes(packed, result) || pixels[0] != OsdColors::Yuv::kGreen.y) {
        return 1;
    }
    packed.buffer = std::make_shared<SimpleBuffer>(std::vector<std::uint8_t>(1));
    if (DrawDetectionBoxes(packed, result)) {
        return 1;
    }

    for (bool nv21 : {false, true}) {
        AVFrame* av_frame = av_frame_alloc();
        if (!av_frame) {
            return 1;
        }
        auto buffer = std::make_shared<FFmpegRawFrameBuffer>(av_frame);
        av_frame->format = nv21 ? AV_PIX_FMT_NV21 : AV_PIX_FMT_NV12;
        av_frame->width = 24;
        av_frame->height = 24;
        if (av_frame_get_buffer(av_frame, 32) < 0) {
            return 1;
        }
        for (int plane = 0; plane < 2; ++plane) {
            std::memset(av_frame->data[plane], 128, av_frame->linesize[plane] * (plane == 0 ? 24 : 12));
        }
        AVFrame* reference = av_frame_clone(av_frame);
        if (!reference) {
            return 1;
        }
        auto raw = MakeI420Frame(24, 24);
        std::get<VideoFrameMeta>(raw.meta).pixel_format = nv21 ? PixelFormat::kNV21 : PixelFormat::kNV12;
        raw.buffer = buffer;
        const bool drawn = DrawDetectionBoxes(raw, result);
        const bool correct = drawn && buffer->PlaneData(0)[0] == OsdColors::Yuv::kGreen.y &&
            buffer->PlaneData(1)[0] == (nv21 ? OsdColors::Yuv::kGreen.v : OsdColors::Yuv::kGreen.u) &&
            buffer->PlaneData(1)[1] == (nv21 ? OsdColors::Yuv::kGreen.u : OsdColors::Yuv::kGreen.v) &&
            reference->data[0][0] == 128 && reference->data[1][0] == 128;
        av_frame_free(&reference);
        if (!correct) {
            return 1;
        }
    }
    std::cout << "Detection box tests passed\n";
    return 0;
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

        if (!DrawDetectionBoxes2(*frame, result)) {
            LOG_ERROR("Drawing detection boxes failed for frame {}: invalid or unsupported YUV plane layout",
                      decoded_frames);
            return;
        }

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

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--test-detection-boxes") {
        return TestDetectionBoxes();
    }
    return RunFfmpegPullerDecoderInferenceRendererTest();
}
