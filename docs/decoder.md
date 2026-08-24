# decoder 模块

本文档介绍 decoder 模块 的职责、依赖、配置和生命周期。


## 1、职责

**decoder 模块 只负责解码视频/音频流，输入 MediaPacket，输出 MediaFrame。**

## 2、接口

### IDecoder 接口
- **Open()** 根据输入的 MediaStreamInfo 初始化解码器。
- **Decode()** 解码输入的 MediaPacket，输出 MediaFrame 由回调函数进行处理。
- **Flush()** 清空解码器中的缓存。
- **Close()** 关闭解码器。
- **SetFrameCallback()** 设置解码器的回调函数，用于接收解码后的 MediaFrame。

### FFmpegDecoder 软解码器
- **receiveFrames()** 将解码后的 AVFrame 数据提取并填充到 MediaFrame 的元数据（meta）中，分别处理视频帧和音频帧两种情况。


## 3、拓展
- AVFrame内存池优化
- 硬件解码