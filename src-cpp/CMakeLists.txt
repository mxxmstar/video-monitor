cmake_minimum_required(VERSION 3.15)

if(CMAKE_HOST_WIN32)
    set(DEFAULT_VCPKG_ROOT "C:/vcpkg_env/vcpkg")
    set(DEFAULT_VCPKG_TRIPLET "x64-windows")
    set(DEFAULT_VCPKG_INSTALLED_DIR "C:/vcpkg_env/vcpkg_installed")
else()
    set(DEFAULT_VCPKG_ROOT "$ENV{HOME}/vcpkg")
    set(DEFAULT_VCPKG_TRIPLET "x64-linux")
    set(DEFAULT_VCPKG_INSTALLED_DIR "$ENV{HOME}/vcpkg_env/vcpkg_installed")
endif()
if(DEFINED ENV{VCPKG_ROOT})
    set(DEFAULT_VCPKG_ROOT "$ENV{VCPKG_ROOT}")
endif()
if(DEFINED ENV{VCPKG_INSTALLED_DIR})
    set(DEFAULT_VCPKG_INSTALLED_DIR "$ENV{VCPKG_INSTALLED_DIR}")
endif()
set(CMAKE_TOOLCHAIN_FILE "${DEFAULT_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
    CACHE FILEPATH "Vcpkg toolchain file")
set(VCPKG_TARGET_TRIPLET "${DEFAULT_VCPKG_TRIPLET}" CACHE STRING "Vcpkg target triplet")
set(VCPKG_INSTALLED_DIR "${DEFAULT_VCPKG_INSTALLED_DIR}" CACHE PATH "Vcpkg installed directory")

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

project(video_monitor_cpp VERSION 0.1.0 LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DEFAULT_DEPS_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
    CACHE PATH "Dependency prefix")

# 添加 vcpkg 安装的依赖路径
list(APPEND CMAKE_PREFIX_PATH "${DEFAULT_DEPS_ROOT}")


# 输出目录配置
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# 为每种构建类型（Build Configuration）单独设置输出目录
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/lib")
endforeach()

# 查找 Boost 库
find_package(Boost CONFIG REQUIRED COMPONENTS system filesystem process)
find_package(spdlog CONFIG REQUIRED)

option(BUILD_RENDER "Build the render module" ON)

# OpenVINO 通过 inference_lib 暴露给推理实现；关闭 BUILD_INFERENCE
# 时基础媒体/渲染构建不依赖 OpenVINO。
option(BUILD_INFERENCE "Build the C++ inference module" ON)
option(BUILD_INFERENCE_TESTS "Build inference tests" ON)

set(DEFAULT_OPENVINO_ROOT "")
if(CMAKE_HOST_WIN32)
    set(_local_openvino_root
        "C:/vcpkg_env/openvino_toolkit_windows_2026.2.1.21919.ede283a88e3_x86_64")
    if(EXISTS "${_local_openvino_root}/runtime/cmake/OpenVINOConfig.cmake")
        set(DEFAULT_OPENVINO_ROOT "${_local_openvino_root}")
    endif()
endif()
if(DEFINED ENV{OPENVINO_ROOT})
    set(DEFAULT_OPENVINO_ROOT "$ENV{OPENVINO_ROOT}")
endif()
set(OPENVINO_ROOT "${DEFAULT_OPENVINO_ROOT}" CACHE PATH
    "OpenVINO toolkit root used by the inference module")
# Recover an empty root cached by an earlier Visual Studio configuration.
if(NOT OPENVINO_ROOT AND DEFAULT_OPENVINO_ROOT)
    set(OPENVINO_ROOT "${DEFAULT_OPENVINO_ROOT}" CACHE PATH
        "OpenVINO toolkit root used by the inference module" FORCE)
endif()
set(INFERENCE_MODEL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../models" CACHE PATH
    "Root directory containing inference model assets")

if(BUILD_INFERENCE)
    if(NOT OpenVINO_DIR AND OPENVINO_ROOT)
        set(_openvino_cmake_dir "${OPENVINO_ROOT}/runtime/cmake")
        if(EXISTS "${_openvino_cmake_dir}/OpenVINOConfig.cmake")
            set(OpenVINO_DIR "${_openvino_cmake_dir}" CACHE PATH
                "OpenVINO CMake package directory" FORCE)
        endif()
    endif()
    find_package(OpenVINO REQUIRED COMPONENTS Runtime)
endif()

# 保留 packed 路径作为性能对照组；切换为 ON 时启用 raw AVFrame 路径。
option(MEDIA_RAW_FRAME_BUFFER "Use non-copying raw AVFrame media buffers" OFF)

# FFmpeg 配置
set(DEFAULT_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-8.1")
if(DEFINED ENV{FFMPEG_ROOT})
    set(DEFAULT_FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
endif()
set(FFMPEG_ROOT "${DEFAULT_FFMPEG_ROOT}"
    CACHE PATH "FFmpeg development package root")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")

# FFmpeg 库文件
set(FFMPEG_LIBS)
foreach(component avcodec avformat avutil avdevice avfilter swscale swresample)
    find_library(FFMPEG_${component}_LIBRARY NAMES ${component}
        PATHS "${FFMPEG_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    list(APPEND FFMPEG_LIBS "${FFMPEG_${component}_LIBRARY}")
endforeach()
if(UNIX AND NOT APPLE)
    # The downloaded shared libraries also need this path for transitive dependencies.
    list(APPEND CMAKE_BUILD_RPATH "${FFMPEG_LIB_DIR}")
    add_link_options("LINKER:--disable-new-dtags")
endif()
file(GLOB FFMPEG_RUNTIME_DLLS CONFIGURE_DEPENDS
    "${FFMPEG_ROOT}/bin/*.dll"
)

# 收集公共库源文件；main.cpp 由下方主程序目标单独编译。
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(FILTER SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*/render/.*\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*/inference/.*\\.cpp$")
# MediaStreamSource 仍处于骨架阶段，先只接入已经实现的 Session。
list(FILTER SOURCES EXCLUDE REGEX ".*/media/stream/stream_source\\.cpp$")

# 创建静态库
add_library(common_lib STATIC ${SOURCES})

# 设置头文件包含目录
target_include_directories(common_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${FFMPEG_INCLUDE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 链接 Boost 库
target_link_libraries(common_lib
    PUBLIC
        Boost::system
        Boost::filesystem
        Boost::process
        spdlog::spdlog
)

if(MEDIA_RAW_FRAME_BUFFER)
    target_compile_definitions(common_lib PUBLIC RAW_FRAME_BUFFER=1)
else()
    target_compile_definitions(common_lib PUBLIC RAW_FRAME_BUFFER=0)
endif()

# 链接 FFmpeg 库
target_link_libraries(common_lib
    PUBLIC
        ${FFMPEG_LIBS}
)

# Windows 特定设置
if(WIN32)
    target_link_libraries(common_lib
        PRIVATE
            advapi32
            user32
            shell32
    )
endif()

# 编译选项
target_compile_options(common_lib PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

if(MSVC)
    target_compile_options(common_lib PUBLIC /utf-8 /Zc:__cplusplus)
endif()

# 将目标的运行时依赖复制到可执行文件目录，避免运行时找不到 DLL。
function(copy_runtime_dependencies target_name)
    if(NOT WIN32)
        return()
    endif()

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:${target_name}>"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copying linked runtime DLLs for ${target_name}"
    )

    foreach(ffmpeg_dll IN LISTS FFMPEG_RUNTIME_DLLS)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ffmpeg_dll}"
                "$<TARGET_FILE_DIR:${target_name}>"
            COMMENT "Copying FFmpeg runtime DLL: ${ffmpeg_dll}"
        )
    endforeach()
endfunction()

if(BUILD_INFERENCE)
    # Stage 1 只建立独立 target 和依赖边界；具体 inference 源码在后续阶段逐步
    # 放入 src/inference，空目录时仍允许先验证 CMake/OpenVINO 配置。
    file(GLOB_RECURSE INFERENCE_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/inference/*.cpp")
    if(INFERENCE_SOURCES)
        add_library(inference_lib STATIC ${INFERENCE_SOURCES})
        target_include_directories(inference_lib
            PUBLIC
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/include/inference
                ${FFMPEG_INCLUDE_DIR}
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_link_libraries(inference_lib
            PUBLIC
                common_lib
                openvino::runtime
        )
    else()
        add_library(inference_lib INTERFACE)
        target_link_libraries(inference_lib
            INTERFACE
                common_lib
                openvino::runtime
        )
    endif()

endif()

if(BUILD_RENDER)
    add_subdirectory(src/render)
endif()

# 主程序由主 CMake 管理。
add_executable(video_monitor "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
target_link_libraries(video_monitor PRIVATE common_lib)
if(BUILD_INFERENCE)
    target_link_libraries(video_monitor PRIVATE inference_lib)
endif()
if(BUILD_RENDER)
    target_link_libraries(video_monitor PRIVATE render_lib)
endif()
set_target_properties(video_monitor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
copy_runtime_dependencies(video_monitor)

if(BUILD_INFERENCE AND WIN32 AND OPENVINO_ROOT)
    # OpenVINO plugin/frontend 通过动态加载发现，不能只依赖 TARGET_RUNTIME_DLLS。
    # 使用 CONFIG 选择 Debug/Release 目录，兼容 Visual Studio 多配置生成器。
    set(_openvino_release_dir "${OPENVINO_ROOT}/runtime/bin/intel64/Release")
    set(_openvino_debug_dir "${OPENVINO_ROOT}/runtime/bin/intel64/Debug")
    set(_openvino_runtime_names
        openvino.dll
        openvino_c.dll
        openvino_intel_cpu_plugin.dll
        openvino_auto_plugin.dll
        openvino_ir_frontend.dll)
    foreach(_runtime_name IN LISTS _openvino_runtime_names)
        set(_release_file "${_openvino_release_dir}/${_runtime_name}")
        if(_runtime_name STREQUAL "openvino.dll")
            set(_debug_name "openvinod.dll")
        elseif(_runtime_name STREQUAL "openvino_c.dll")
            set(_debug_name "openvino_cd.dll")
        else()
            string(REPLACE ".dll" "d.dll" _debug_name "${_runtime_name}")
        endif()
        set(_debug_file "${_openvino_debug_dir}/${_debug_name}")
        if(EXISTS "${_release_file}" OR EXISTS "${_debug_file}")
            add_custom_command(TARGET video_monitor POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<IF:$<CONFIG:Debug>,${_debug_file},${_release_file}>"
                    "$<TARGET_FILE_DIR:video_monitor>"
                COMMENT "Copying OpenVINO runtime: ${_runtime_name}")
        endif()
    endforeach()
    set(_openvino_tbb_release "${OPENVINO_ROOT}/runtime/3rdparty/tbb/bin/tbb12.dll")
    set(_openvino_tbb_debug "${OPENVINO_ROOT}/runtime/3rdparty/tbb/bin/tbb12_debug.dll")
    if(EXISTS "${_openvino_tbb_release}" OR EXISTS "${_openvino_tbb_debug}")
        add_custom_command(TARGET video_monitor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<IF:$<CONFIG:Debug>,${_openvino_tbb_debug},${_openvino_tbb_release}>"
                "$<TARGET_FILE_DIR:video_monitor>"
            COMMENT "Copying OpenVINO TBB runtime")
    endif()
endif()

# 测试构建选项
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_MEDIA_TESTS "Build media test programs" ON)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
