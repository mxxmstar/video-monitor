find_package(glad CONFIG REQUIRED)

add_executable(test_opengl_video_renderer test_opengl_video_renderer.cpp)
target_link_libraries(test_opengl_video_renderer PRIVATE render_lib glad::glad)
set_target_properties(test_opengl_video_renderer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests")
copy_runtime_dependencies(test_opengl_video_renderer)
add_test(NAME test_opengl_video_renderer COMMAND test_opengl_video_renderer)

add_executable(test_osd_text_batch test_osd_text_batch.cpp)
target_include_directories(test_osd_text_batch PRIVATE "${PROJECT_SOURCE_DIR}/include")
if(MSVC)
    target_compile_options(test_osd_text_batch PRIVATE /utf-8)
endif()
add_test(NAME test_osd_text_batch COMMAND test_osd_text_batch)

add_executable(test_osd_renderers test_osd_renderers.cpp)
target_link_libraries(test_osd_renderers PRIVATE render_lib)
copy_runtime_dependencies(test_osd_renderers)
add_test(NAME test_osd_renderers COMMAND test_osd_renderers)
