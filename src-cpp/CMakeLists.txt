cmake_minimum_required(VERSION 3.15)
project(video_monitor_cpp VERSION 0.1.0 LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# option(BUILD_TESTS "Build test programs" ON)
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

# 收集所有源文件
file(GLOB_RECURSE SOURCES "src/*.cpp")

# 创建静态库
add_library(common_lib STATIC ${SOURCES})

# 设置头文件包含目录
target_include_directories(common_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 链接 Boost 库
target_link_libraries(common_lib
    PUBLIC
        Boost::system
        Boost::filesystem
        Boost::process
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

# 测试构建选项
option(BUILD_TESTS "Build test programs" ON)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()