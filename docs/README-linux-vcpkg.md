# Ubuntu 22.04 vcpkg 环境

虚拟机：`192.168.117.131`，用户：`ub22`。

## 目录

- 工程源码：`/home/ub22/mxxmstar/application`，对应本工程的 `src-cpp` 内容。
- vcpkg 工具：`/home/ub22/vcpkg`，复用虚拟机已有目录。
- 环境目录：`/home/ub22/vcpkg_env`。
- 依赖清单：`/home/ub22/vcpkg_env/vcpkg.json`。
- 安装产物：`/home/ub22/vcpkg_env/vcpkg_installed/x64-linux`。
- 安装日志：`/home/ub22/vcpkg_env/install-x64-linux.log`。

依赖清单来自 `E:/share/project/video-pipeline/vcpkg.json`，与
`docs/vcpkg-windows-setup.md` 引用的清单一致，包括 Boost、spdlog、
ZLMediaKit、libyuv、GLFW、GLAD、gRPC、protobuf、ZeroMQ、yaml-cpp 和 libdatachannel。
vcpkg 源码版本固定为 `cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3`，与 Windows 环境一致。
使用标准 `x64-linux` triplet，依赖库包含 Debug 和 Release 静态库。

## 环境变量与安装命令

`~/.profile` 和 `~/.bashrc` 已加载 `/home/ub22/vcpkg_env/env.sh`。
当前终端可以手动加载：

```bash
source /home/ub22/vcpkg_env/env.sh
vcpkg version
```

环境变量包括 `VCPKG_ROOT`、`VCPKG_DEFAULT_TRIPLET`、
`VCPKG_DEFAULT_HOST_TRIPLET`、`VCPKG_INSTALLED_DIR` 和 `FFMPEG_ROOT`。
脚本还将 FFmpeg 的 `bin` 和 `lib` 加入 `PATH` 和 `LD_LIBRARY_PATH`。

安装或补齐依赖：

```bash
cd /home/ub22/vcpkg_env
VCPKG_MAX_CONCURRENCY=2 vcpkg install \
  --triplet=x64-linux \
  --x-install-root=/home/ub22/vcpkg_env/vcpkg_installed \
  --clean-buildtrees-after-build
```

通过当前工作目录发现 manifest，无需使用旧文档中的 `--manifest-root` 参数。

## 验证

已完成以下检查：

- 文档清单中的 21 个直接依赖及其传递依赖，共 98 个包，全部安装成功。
- 固定 manifest 的 `builtin-baseline` 后再次检查，无需补装或重建依赖。
- 工程 `build-linux` 的 CMake 配置通过，自动找到 Boost 1.91.0 和新依赖目录。
- 独立 Linux 进程测试通过，覆盖启动、参数传递、退出码和启动失败处理。
- 综合依赖验证通过，CTest 结果为 `1/1` 通过。
- `protoc --version` 输出 `libprotoc 33.4`，MediaServer 无缺失的动态库依赖。

系统编译器为 GCC 12.3.0，系统 CMake 为 3.22.1；vcpkg 构建依赖时
自动下载并使用 CMake 4.3.3。

独立进程测试位于 `/home/ub22/vcpkg_env/verify-core-src`，可重新运行：

```bash
ctest --test-dir /home/ub22/vcpkg_env/verify-core-build --output-on-failure
```

环境验证工程位于 `/home/ub22/vcpkg_env/verify-src`。它检查主要依赖的
CMake 配置、头文件和链接，并执行 JSON/YAML、protobuf、libyuv、ZeroMQ
以及本工程 Linux 进程模块的基本运行验证，不需要 FFmpeg。

```bash
cmake -S /home/ub22/vcpkg_env/verify-src \
  -B /home/ub22/vcpkg_env/verify-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/home/ub22/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DVCPKG_INSTALLED_DIR=/home/ub22/vcpkg_env/vcpkg_installed \
  -DVCPKG_MANIFEST_MODE=OFF
cmake --build /home/ub22/vcpkg_env/verify-build --parallel 2
ctest --test-dir /home/ub22/vcpkg_env/verify-build --output-on-failure
```

## FFmpeg 与完整工程构建

用户提供的归档已解压到：
`/home/ub22/vcpkg_env/ffmpeg-n8.1-latest-linux64-gpl-shared-8.1`。
该目录包含 `bin`、`include`、`lib` 和可重定位的 pkg-config 文件。
实际 FFmpeg 版本为 `n8.1.3-14-g330caae0c1-20261008`。

CMake 支持通过 `FFMPEG_ROOT` 环境变量或同名缓存参数选择开发包，
在 Linux 查找 `.so`，在 Windows 查找 `.lib`。
Linux 构建产物含 FFmpeg 库目录的 RPATH，清除 `LD_LIBRARY_PATH` 后
仍可运行媒体测试，动态依赖检查没有缺失库。
为通过 GCC 编译，修正了 `MediaFrame::PixelFormat()` 返回类型的限定，
并为推流配置补齐 `<optional>` 包含。

```bash
source /home/ub22/vcpkg_env/env.sh
cmake -S /home/ub22/mxxmstar/application \
  -B /home/ub22/mxxmstar/application/build-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DFFMPEG_ROOT="$FFMPEG_ROOT" \
  -DBUILD_TESTS=ON -DBUILD_MEDIA_TESTS=ON
cmake --build /home/ub22/mxxmstar/application/build-linux --parallel 2
env -u LD_LIBRARY_PATH ctest \
  --test-dir /home/ub22/mxxmstar/application/build-linux \
  -R '^test_media_' \
  -E '^test_media_test_(ffmpeg_puller_decoder_converter_encoder|media_stream_session)$' \
  --output-on-failure --timeout 60
```

2026-10-09 验证结果：

- 完整工程和全部测试目标构建成功，使用默认 packed 帧模式。
- FFmpeg 命令生成 1 秒 160x120 H.264 / 48 kHz AAC 视频，ffprobe 信息正确，解码无错误。
- 本地媒体测试 7/8 通过，包含 FFmpegPusher、真实 H.264 编码及双 MP4
  时间戳验证、raw frame 契约、Publisher、异步推流和脚本流会话/重连测试。
- `test_media_test_pusher_session` 重复失败，输出
  `Async event did not interrupt blocked Push`；该用例使用模拟推流器，不调用 FFmpeg。
- 两项依赖外部 RTSP 摄像头/服务的测试未运行；Windows 路径的进程测试未运行。
  Linux 进程模块已通过前述独立验证。可选 raw 帧模式的完整构建未运行。

构建和测试日志分别位于 `/home/ub22/vcpkg_env/build-ffmpeg-linux.log`
和 `/home/ub22/vcpkg_env/test-ffmpeg-linux.log`。
命令行测试文件为 `/home/ub22/vcpkg_env/ffmpeg-smoke.mp4`。
