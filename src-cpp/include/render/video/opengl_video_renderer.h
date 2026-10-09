#pragma once

#include <cstdint>
#include <string>

#include <vector>
#include "render/video/i_video_renderer.h"
struct GLFWwindow;

namespace render {

/// @brief 基于 GLFW + GLAD + OpenGL 3.3 的本地视频窗口渲染器。
///
/// 渲染路径：
/// 1. 从连续缓冲区或 raw AVFrame 读取视频平面；
/// 2. 上传 RGB/BGR/灰度或 YUV 纹理，YUV 在 shader 中转换；
/// 3. 一个全屏四边形采样该纹理并显示到窗口。
///
/// 注意：GLFW 窗口和 OpenGL context 通常应该在同一线程创建和使用。
/// 如果未来把它接到异步解码回调，建议单独建立 render 线程并通过队列投递帧。
class OpenGLVideoRenderer final : public IVideoRenderer {
public:
    OpenGLVideoRenderer() = default;
    ~OpenGLVideoRenderer() override;
    OpenGLVideoRenderer(const OpenGLVideoRenderer&) = delete;
    OpenGLVideoRenderer& operator=(const OpenGLVideoRenderer&) = delete;

    /// @brief 初始化渲染器，创建窗口、OpenGL context、shader 和纹理。
    bool Init(const RenderConfig& config) override;
    /// @brief 渲染一帧视频。
    bool Render(const MediaFrame& frame) override;
    /// @brief 轮询 GLFW 事件，处理窗口关闭和键盘事件。
    void PollEvents() override;
    /// @brief 判断窗口是否已经请求关闭。
    bool ShouldClose() const override;
    /// @brief 关闭渲染器，删除窗口、OpenGL context、shader 和纹理。
    void Shutdown() override;
    const std::string& LastError() const { return last_error_; }

private:
    /// @brief 初始化 GLFW、创建窗口并加载 OpenGL 函数指针。
    bool createWindow();

    /// @brief 创建最小 shader program，用于把纹理绘制到全屏 quad。
    bool createProgram();

    /// @brief 创建全屏矩形的 VAO/VBO/EBO。
    bool createQuad();

    /// @brief 将 RGB/BGR/灰度或 NV12/NV21/I420 平面直接上传为纹理。
    ///
    /// 该路径保留解码器提供的 stride 和 plane offset，不先生成 RGBA临时帧；
    ///fragment shader 会在采样时完成颜色空间转换。
    bool uploadVideoTextures(const MediaFrame& frame, int& shader_format);

    /// @brief 上传一个带 stride 的单通道或双通道视频平面。
    bool uploadPlane(std::uint32_t& texture,
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
                     const char* plane_name);

    /// @brief 删除 OpenGL 对象。调用前需要确保当前线程拥有有效 context。
    void destroyGlResources();

    /// @brief 记录并输出最近一次渲染错误。
    void setError(std::string message);

    RenderConfig config_;   ///< 渲染配置
    std::vector<std::uint8_t> upload_buffer_;
    GLFWwindow* window_{nullptr};   ///< GLFW 窗口
    std::uint32_t program_{0};   ///< OpenGL shader program
    std::uint32_t vao_{0};   ///< OpenGL VAO
    std::uint32_t vbo_{0};   ///< OpenGL VBO
    std::uint32_t ebo_{0};   ///< OpenGL EBO
    std::uint32_t texture_y_{0};   ///< OpenGL Y平面 纹理
    std::uint32_t texture_plane1_{0};   ///< OpenGL 平面纹理1
    std::uint32_t texture_plane2_{0};   ///< OpenGL 平面纹理2
    int texture_y_width_{0};   ///< OpenGL Y平面 纹理宽度
    int texture_y_height_{0};   ///< OpenGL Y平面 纹理高度
    unsigned int texture_y_internal_format_{0};   ///< OpenGL Y平面 纹理内部格式（是否为GL_R8）
    int texture_plane1_width_{0};   ///< OpenGL 平面纹理1宽度
    int texture_plane1_height_{0};   ///< OpenGL 平面纹理1高度
    unsigned int texture_plane1_internal_format_{0};   ///< OpenGL 平面纹理1内部格式（是否为GL_R8）
    int texture_plane2_width_{0};   ///< OpenGL 平面纹理2宽度
    int texture_plane2_height_{0};   ///< OpenGL 平面纹理2高度
    unsigned int texture_plane2_internal_format_{0};    ///< OpenGL 平面纹理2内部格式（是否为GL_R8）
    int pixel_format_uniform_{-1};  ///< Shader 中 uPixelFormat 的位置
    bool glfw_initialized_{false};   ///< GLFW 是否初始化
    bool initialized_{false};   ///< renderer 是否初始化
    std::string last_error_;     ///< 最近一次渲染错误信息
};

} // namespace render
