#include "render/video/opengl_video_renderer.h"
#include "common/log/logger.h"
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include "media/ffmpeg_raw_frame_buffer.h"
#include <iostream>
#include <utility>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace render {
namespace {

unsigned int glfw_users = 0;    // GLFW 初始化计数器，支持多次创建 OpenGLVideoRenderer。

// 顶点 shader 只负责把全屏 quad 的位置和纹理坐标传给片元阶段。
constexpr const char* kVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)";

// 片元 shader 同时支持 RGB/灰度 和 YUV 平面纹理。
// YUV 路径把颜色转换放到 GPU，避免高分辨率视频每帧在 CPU 上逐像素转换。
constexpr const char* kFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex0;
uniform sampler2D uTex1;
uniform sampler2D uTex2;
uniform int uPixelFormat;

vec3 yuvToRgb(float y, float u, float v) {
    float luma = (y - 16.0 / 255.0) * (298.0 / 256.0);
    float chromaU = u - 128.0 / 255.0;
    float chromaV = v - 128.0 / 255.0;
    return clamp(vec3(
        luma + (409.0 / 256.0) * chromaV,
        luma - (100.0 / 256.0) * chromaU - (208.0 / 256.0) * chromaV,
        luma + (516.0 / 256.0) * chromaU), 0.0, 1.0);
}

void main() {
    if (uPixelFormat == 4) {
        FragColor = texture(uTex0, vUV);
        return;
    }
    if (uPixelFormat == 5) {
        FragColor = vec4(vec3(texture(uTex0, vUV).r), 1.0);
        return;
    }

    float y = texture(uTex0, vUV).r;
    if (uPixelFormat == 1 || uPixelFormat == 2) {
        vec2 uv = texture(uTex1, vUV).rg;
        if (uPixelFormat == 2) {
            uv = uv.yx;
        }
        FragColor = vec4(yuvToRgb(y, uv.x, uv.y), 1.0);
        return;
    }

    float u = texture(uTex1, vUV).r;
    float v = texture(uTex2, vUV).r;
    FragColor = vec4(yuvToRgb(y, u, v), 1.0);
}
)";

/// @brief 编译单个 shader
/// @param type shader 类型，如 GL_VERTEX_SHADER 或 GL_FRAGMENT_SHADER。
/// @param source shader 源代码指针。
/// @return shader ID。
std::uint32_t CompileShader(unsigned int type, const char* source) {
    const auto shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOG_ERROR("render shader compile failed: {}", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

OpenGLVideoRenderer::~OpenGLVideoRenderer() {
    Shutdown();
}

bool OpenGLVideoRenderer::Init(const RenderConfig& config) {
    if (initialized_) {
        return true;
    }
    if (config.window_width <= 0 || config.window_height <= 0) {
        setError("invalid window size");
        return false;
    }
    config_ = config;
    last_error_.clear();
    if (!createWindow() || !createProgram() || !createQuad()) {
        Shutdown();
        return false;
    }
    initialized_ = true;
    return true;
}

bool OpenGLVideoRenderer::Render(const MediaFrame& frame) {
    if (!initialized_ && !Init(config_)) {
        return false;
    }
    if (!window_ || ShouldClose()) {
        return false;
    }

    // GLFW context 是线程相关状态；每次渲染前显式绑定当前窗口更稳妥。
    glfwMakeContextCurrent(window_);
    if (frame.type != MediaType::VIDEO ||
        !std::holds_alternative<VideoFrameMeta>(frame.meta)) {
        setError("expected video frame metadata");
        return false;
    }
    int shader_format = 0;
    if (!uploadVideoTextures(frame, shader_format)) {
        return false;
    }

    int fb_width = 0;
    int fb_height = 0;
    glfwGetFramebufferSize(window_, &fb_width, &fb_height);
    glViewport(0, 0, fb_width, fb_height);
    glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // 绘制覆盖整个窗口的两个三角形，片元 shader 从视频纹理采样。
    glUseProgram(program_);
    glUniform1i(pixel_format_uniform_, shader_format);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_y_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texture_plane1_);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, texture_plane2_);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);

    glfwSwapBuffers(window_);
    last_error_.clear();
    return true;
}

void OpenGLVideoRenderer::PollEvents() {
    if (!window_) {
        return;
    }

    // GLFW 需要在事件循环中轮询，否则窗口关闭和键盘事件不会被处理。
    glfwPollEvents();
    if (config_.close_on_escape &&
        glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
}

bool OpenGLVideoRenderer::ShouldClose() const {
    return window_ && glfwWindowShouldClose(window_);
}

void OpenGLVideoRenderer::Shutdown() {
    if (window_) {
        // 删除 GL 对象前绑定 context，避免在没有当前 context 的线程上调用 glDelete*。
        glfwMakeContextCurrent(window_);
        destroyGlResources();
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }

    if (glfw_initialized_) {
        if (--glfw_users == 0) {
            glfwTerminate();
        }
        glfw_initialized_ = false;
    }

    initialized_ = false;
    texture_y_width_ = 0;
    texture_y_height_ = 0;
    texture_y_internal_format_ = 0;
    texture_plane1_width_ = 0;
    texture_plane1_height_ = 0;
    texture_plane1_internal_format_ = 0;
    texture_plane2_width_ = 0;
    texture_plane2_height_ = 0;
    texture_plane2_internal_format_ = 0;
    pixel_format_uniform_ = -1;
}

bool OpenGLVideoRenderer::createWindow() {
    if (glfw_users == 0 && !glfwInit()) {
        setError("failed to initialize GLFW");
        return false;
    }
    glfw_initialized_ = true;
    ++glfw_users;

    // 使用 OpenGL 3.3 core profile，覆盖 Windows/Linux/macOS 上较常见的能力基线。
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, config_.visible ? GLFW_TRUE : GLFW_FALSE);

    window_ = glfwCreateWindow(config_.window_width,
                               config_.window_height,
                               config_.title.c_str(),
                               nullptr,
                               nullptr);
    if (!window_) {
        setError("failed to create GLFW window");
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(config_.vsync ? 1 : 0);

    // glad 必须在拥有当前 context 之后加载，否则无法解析 OpenGL 函数指针。
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        setError("failed to initialize GLAD");
        return false;
    }

    return true;
}

bool OpenGLVideoRenderer::createProgram() {
    // 编译着色器，顶点和片元。
    const auto vertex_shader = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    const auto fragment_shader = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vertex_shader == 0 || fragment_shader == 0) {
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        setError("failed to compile video shaders");
        return false;
    }
    // 创建并链接程序
    program_ = glCreateProgram();
    glAttachShader(program_, vertex_shader);
    glAttachShader(program_, fragment_shader);
    glLinkProgram(program_);

    // 删除临时 shader 对象，程序已经链接完成。
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    int ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]{};
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        setError(std::string("render program link failed: ") + log);
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    // 固定绑定三个纹理单元：RGBA/Y、UV 或 U、V。Render() 只切换格式
    // 标志和实际存在的纹理对象，不需要重新查询 uniform。
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "uTex0"), 0);
    glUniform1i(glGetUniformLocation(program_, "uTex1"), 1);
    glUniform1i(glGetUniformLocation(program_, "uTex2"), 2);
    pixel_format_uniform_ = glGetUniformLocation(program_, "uPixelFormat");
    if (pixel_format_uniform_ < 0) {
        setError("failed to locate YUV format uniform");
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }
    return true;
}

bool OpenGLVideoRenderer::createQuad() {
    // 顶点格式为 position.xy + uv.xy。
    // UV 的 y 方向按图像内存从上到下的布局翻转，避免画面上下颠倒。
    const std::array<float, 16> vertices{
        -1.0f, -1.0f, 0.0f, 1.0f,   // 左下角
         1.0f, -1.0f, 1.0f, 1.0f,   // 右下角
         1.0f,  1.0f, 1.0f, 0.0f,   // 右上角
        -1.0f,  1.0f, 0.0f, 0.0f,   // 左上角
    };
    const std::array<unsigned int, 6> indices{0, 1, 2, 0, 2, 3};     // 两个三角形

    // 创建 VAO、VBO 和 EBO
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    glBindVertexArray(vao_);

    // 绑定 VBO 并上传顶点数据
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(),
                 GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned int)),
                 indices.data(),
                 GL_STATIC_DRAW);

    // 配置顶点属性指针
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    return true;
}

namespace {

/// @brief 统一上传平面视图结构体
struct UploadPlaneView {
    const std::uint8_t* data{nullptr};
    int stride{0};
};

/// @brief 从媒体帧中解析指定平面数据和步长，上传到平面视图中
/// @param frame 输入媒体帧
/// @param plane 平面索引
/// @param fallback_offset 回退偏移量
/// @param row_bytes 每行字节数
/// @param rows 行数
/// @param out 输出平面视图
/// @param error 输出错误信息
/// @return 是否成功解析
bool ResolveUploadPlane(const MediaFrame& frame,
                        int plane,
                        std::size_t fallback_offset,
                        int row_bytes,
                        int rows,
                        UploadPlaneView& out,
                        std::string& error) {
    // 尝试从 ffmpeg 原始视频帧中解析平面数据
    if (const auto* raw = dynamic_cast<const FFmpegRawFrameBuffer*>(frame.buffer.get())) {
        const int stride = raw->PlaneStride(plane);
        const auto magnitude = stride < 0 ? -static_cast<std::int64_t>(stride) : stride;
        if (!raw->IsValid() || !raw->PlaneData(plane) ||
            row_bytes <= 0 || rows <= 0 || magnitude < row_bytes) {
            error = "invalid raw video plane";
            return false;
        }
        out.data = raw->PlaneData(plane);
        out.stride = stride;
        return true;
    }
    const auto* base = frame.buffer ? frame.buffer->Data() : nullptr;
    const std::size_t size = frame.buffer ? frame.buffer->Size() : 0;
    if (!base || size == 0) {
        error = "video frame buffer is empty";
        return false;
    }
    // 连续缓冲区的普通帧，使用 frame 的 stride 和 plane offset 解析平面数据。
    const auto* meta = frame.VideoMeta();
    if (meta->plane_count < 0 || meta->plane_count > 8 || (meta->plane_count > 0 && plane >= meta->plane_count) ||
        frame.Stride(plane) < 0 || frame.PlaneOffset(plane) < 0) {
        error = "invalid video plane metadata";
        return false;
    }
    // 优先使用 frame 的 stride，否则使用 row_bytes 作为 stride。
    const int stride = frame.Stride(plane) > 0 ? frame.Stride(plane) : row_bytes;
    if (row_bytes <= 0 || rows <= 0 || stride < row_bytes) {
        error = "invalid YUV plane stride";
        return false;
    }

    // 优先使用 frame 的 plane offset，否则使用 fallback_offset。
    const std::size_t offset = frame.PlaneOffset(plane) > 0 ? static_cast<std::size_t>(frame.PlaneOffset(plane)) : fallback_offset;
    if (offset > size) {
        error = "YUV plane offset exceeds frame buffer";
        return false;
    }

    // 边界检查
    const std::size_t last_row_offset = static_cast<std::size_t>(stride) * static_cast<std::size_t>(rows - 1);
    const std::size_t remaining = size - offset;
    if (last_row_offset > remaining || static_cast<std::size_t>(row_bytes) > remaining - last_row_offset) {
        error = "YUV plane exceeds frame buffer";
        return false;
    }

    out.data = base + offset;
    out.stride = stride;
    return true;
}

} // namespace

bool OpenGLVideoRenderer::uploadPlane(std::uint32_t& texture,
                                      int& texture_width,
                                      int& texture_height,
                                      unsigned int& texture_internal_format,
                                      int width,
                                      int height,
                                      int row_bytes,
                                      int stride,
                                      const std::uint8_t* data,
                                      unsigned int pixel_format,
                                      unsigned int internal_format,
                                      const char* plane_name) {
    const int bytes_per_pixel = internal_format == GL_RGB8 ? 3 : (internal_format == GL_RG8 ? 2 : 1);
    // 处理非标准 stride 的情况，复制到连续缓冲区再上传。
    if (data && width > 0 && height > 0 && row_bytes > 0 && (stride < 0 || stride % bytes_per_pixel != 0)) {
        const auto magnitude = stride < 0 ? -static_cast<std::int64_t>(stride) : stride;
        if (magnitude < row_bytes) {
            setError("invalid video plane stride");
            return false;
        }

        // 复制到连续缓冲区，避免 GPU 读取非标准 stride。
        upload_buffer_.resize(static_cast<std::size_t>(row_bytes) * height);
        for (int row = 0; row < height; ++row) {
            std::memcpy(upload_buffer_.data() + static_cast<std::size_t>(row) * row_bytes,
                        data + static_cast<std::ptrdiff_t>(row) * stride, row_bytes);
        }
        data = upload_buffer_.data();
        stride = row_bytes;
    }
    if (width <= 0 || height <= 0 || row_bytes <= 0 || stride < row_bytes || stride % bytes_per_pixel != 0 || !data) {
        setError(std::string("invalid ") + plane_name + " upload layout");
        return false;
    }

    if (texture == 0) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else {
        glBindTexture(GL_TEXTURE_2D, texture);
    }

    // GL_UNPACK_ROW_LENGTH 让 GPU 直接读取解码器的 stride，避免为了去除 padding 再复制一份 Y/UV 平面。
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / bytes_per_pixel);
    if (width != texture_width || height != texture_height || texture_internal_format != internal_format) {
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     static_cast<GLint>(internal_format),
                     width,
                     height,
                     0,
                     pixel_format,
                     GL_UNSIGNED_BYTE,
                     data);
        texture_width = width;
        texture_height = height;
        texture_internal_format = internal_format;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        width,
                        height,
                        pixel_format,
                        GL_UNSIGNED_BYTE,
                        data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return true;
}

bool OpenGLVideoRenderer::uploadVideoTextures(const MediaFrame& frame, int& shader_format) {
    const int width = frame.Width();
    const int height = frame.Height();
    if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() / 3 || height == std::numeric_limits<int>::max()) {
        setError("invalid YUV frame size");
        return false;
    }

    const int chroma_width = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;
    const auto format = frame.PixelFormat();
    std::string error;
    if (format == PixelFormat::kRGB24 || format == PixelFormat::kBGR24 || format == PixelFormat::kGRAY8) {
        const bool gray = format == PixelFormat::kGRAY8;
        const int row_bytes = width * (gray ? 1 : 3);
        UploadPlaneView packed;
        if (!ResolveUploadPlane(frame, 0, 0, row_bytes, height, packed, error)) {
            setError(std::move(error));
            return false;
        }
        shader_format = gray ? 5 : 4;
        return uploadPlane(texture_y_, texture_y_width_, texture_y_height_,
                           texture_y_internal_format_, width, height, row_bytes,
                           packed.stride, packed.data,
                           gray ? GL_RED : (format == PixelFormat::kBGR24 ? GL_BGR : GL_RGB),
                           gray ? GL_R8 : GL_RGB8, "packed");
    }
    if (format != PixelFormat::kNV12 && format != PixelFormat::kNV21 && format != PixelFormat::kI420) {
        setError("unsupported video pixel format");
        return false;
    }
    UploadPlaneView y_plane;
    if (!ResolveUploadPlane(frame, 0, 0, width, height, y_plane, error)) {
        setError(std::move(error));
        return false;
    }

    const std::size_t y_size = static_cast<std::size_t>(y_plane.stride) * static_cast<std::size_t>(height);
    if (!uploadPlane(texture_y_,
                     texture_y_width_,
                     texture_y_height_,
                     texture_y_internal_format_,
                     width,
                     height,
                     width,
                     y_plane.stride,
                     y_plane.data,
                     GL_RED,
                     GL_R8,
                     "Y")) {
        return false;
    }

    if (format == PixelFormat::kNV12 || format == PixelFormat::kNV21) {
        UploadPlaneView uv_plane;
        if (!ResolveUploadPlane(frame,
                                1,
                                y_size,
                                chroma_width * 2,
                                chroma_height,
                                uv_plane,
                                error)) {
            setError(std::move(error));
            return false;
        }
        if (!uploadPlane(texture_plane1_,
                         texture_plane1_width_,
                         texture_plane1_height_,
                         texture_plane1_internal_format_,
                         chroma_width,
                         chroma_height,
                         chroma_width * 2,
                         uv_plane.stride,
                         uv_plane.data,
                         GL_RG,
                         GL_RG8,
                         "UV")) {
            return false;
        }
        shader_format = format == PixelFormat::kNV21 ? 2 : 1;
        return true;
    }

    if (format == PixelFormat::kI420) {
        UploadPlaneView u_plane;
        if (!ResolveUploadPlane(frame,
                                1,
                                y_size,
                                chroma_width,
                                chroma_height,
                                u_plane,
                                error)) {
            setError(std::move(error));
            return false;
        }
        const std::size_t u_size = static_cast<std::size_t>(u_plane.stride) * static_cast<std::size_t>(chroma_height);
        UploadPlaneView v_plane;
        if (!ResolveUploadPlane(frame,
                                2,
                                y_size + u_size,
                                chroma_width,
                                chroma_height,
                                v_plane,
                                error)) {
            setError(std::move(error));
            return false;
        }

        if (!uploadPlane(texture_plane1_,
                         texture_plane1_width_,
                         texture_plane1_height_,
                         texture_plane1_internal_format_,
                         chroma_width,
                         chroma_height,
                         chroma_width,
                         u_plane.stride,
                         u_plane.data,
                         GL_RED,
                         GL_R8,
                         "U") ||
            !uploadPlane(texture_plane2_,
                         texture_plane2_width_,
                         texture_plane2_height_,
                         texture_plane2_internal_format_,
                         chroma_width,
                         chroma_height,
                         chroma_width,
                         v_plane.stride,
                         v_plane.data,
                         GL_RED,
                         GL_R8,
                         "V")) {
            return false;
        }
        shader_format = 3;
        return true;
    }

    setError("unsupported direct YUV format");
    return false;
}

void OpenGLVideoRenderer::destroyGlResources() {
    if (texture_y_ != 0) {
        glDeleteTextures(1, &texture_y_);
        texture_y_ = 0;
    }
    if (texture_plane1_ != 0) {
        glDeleteTextures(1, &texture_plane1_);
        texture_plane1_ = 0;
    }
    if (texture_plane2_ != 0) {
        glDeleteTextures(1, &texture_plane2_);
        texture_plane2_ = 0;
    }
    if (ebo_ != 0) {
        glDeleteBuffers(1, &ebo_);
        ebo_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

void OpenGLVideoRenderer::setError(std::string message) {
    last_error_ = std::move(message);
    if (!last_error_.empty()) {
        std::cerr << "OpenGLVideoRenderer: " << last_error_ << '\n';
    }
}

} // namespace render
