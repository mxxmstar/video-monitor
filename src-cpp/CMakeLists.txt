cmake_minimum_required(VERSION 3.15)
project(video_monitor_cpp VERSION 0.1.0 LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DEFAULT_DEPS_ROOT "C:/vcpkg_env/vcpkg_installed/x64-windows")

# 设置 vcpkg 工具链文件
set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg_env/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "Vcpkg target triplet")

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
find_package(Boost REQUIRED COMPONENTS system filesystem process)
find_package(spdlog CONFIG REQUIRED)

# FFmpeg 配置
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-8.1")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")

# FFmpeg 库文件
set(FFMPEG_LIBS
    "${FFMPEG_LIB_DIR}/avcodec.lib"
    "${FFMPEG_LIB_DIR}/avformat.lib"
    "${FFMPEG_LIB_DIR}/avutil.lib"
    "${FFMPEG_LIB_DIR}/avdevice.lib"
    "${FFMPEG_LIB_DIR}/avfilter.lib"
    "${FFMPEG_LIB_DIR}/swscale.lib"
    "${FFMPEG_LIB_DIR}/swresample.lib"
)
file(GLOB FFMPEG_RUNTIME_DLLS CONFIGURE_DEPENDS
    "${FFMPEG_ROOT}/bin/*.dll"
)

# 收集公共库源文件；main.cpp 由下方主程序目标单独编译。
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(FILTER SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
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

# 主程序由主 CMake 管理。
add_executable(video_monitor "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
target_link_libraries(video_monitor PRIVATE common_lib)
set_target_properties(video_monitor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
copy_runtime_dependencies(video_monitor)

# 测试构建选项
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_MEDIA_TESTS "Build media test programs" ON)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
