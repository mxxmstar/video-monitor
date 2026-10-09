find_package(glad CONFIG REQUIRED)

add_executable(test_opengl_video_renderer test_opengl_video_renderer.cpp)
target_link_libraries(test_opengl_video_renderer PRIVATE render_lib glad::glad)
set_target_properties(test_opengl_video_renderer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests")
copy_runtime_dependencies(test_opengl_video_renderer)
add_test(NAME test_opengl_video_renderer COMMAND test_opengl_video_renderer)
