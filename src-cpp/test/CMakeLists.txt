# 通用测试 CMakeLists.txt

# 媒体测试由 test/media/CMakeLists.txt 独立管理。
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp")

# 为每个测试文件创建可执行文件
foreach(test_file ${TEST_SOURCES})
    # 获取相对路径并去除扩展名
    file(RELATIVE_PATH rel_path ${CMAKE_CURRENT_SOURCE_DIR} ${test_file})
    get_filename_component(test_name ${rel_path} NAME_WE)
    
    # 获取目录结构作为测试名称
    string(REPLACE "/" "_" test_target "test_${test_name}")
    
    # 创建可执行文件
    add_executable(${test_target} ${test_file})
    
    # 链接 common_lib
    target_link_libraries(${test_target}
        PRIVATE
            common_lib
    )
    
    # 设置测试输出目录
    set_target_properties(${test_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
    )
    
    copy_runtime_dependencies(${test_target})
    
    # 注册测试
    add_test(NAME ${test_target}
        COMMAND ${test_target}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endforeach()

if(BUILD_MEDIA_TESTS)
    add_subdirectory(media)
endif()

if(BUILD_RENDER)
    add_subdirectory(render)
endif()

if(BUILD_INFERENCE AND BUILD_INFERENCE_TESTS)
    find_package(glad CONFIG REQUIRED)
    
    add_executable(test_sync_inference inference/test_sync_inference.cpp)
    target_link_libraries(test_sync_inference PRIVATE 
        inference_lib 
        render_lib 
        common_lib
        glad::glad
    )
    target_compile_definitions(test_sync_inference
        PRIVATE
            VIDEO_MONITOR_MODEL_ROOT="${INFERENCE_MODEL_ROOT}"
    )
    set_target_properties(test_sync_inference PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
    )
    foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(test_sync_inference PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/bin/tests")
    endforeach()
    copy_runtime_dependencies(test_sync_inference)
    if(WIN32 AND OPENVINO_ROOT)
        set(_test_openvino_release_dir "${OPENVINO_ROOT}/runtime/bin/intel64/Release")
        set(_test_openvino_debug_dir "${OPENVINO_ROOT}/runtime/bin/intel64/Debug")
        foreach(_test_runtime_name
                openvino.dll
                openvino_c.dll
                openvino_intel_cpu_plugin.dll
                openvino_auto_plugin.dll
                openvino_ir_frontend.dll)
            if(_test_runtime_name STREQUAL "openvino.dll")
                set(_test_debug_name "openvinod.dll")
            elseif(_test_runtime_name STREQUAL "openvino_c.dll")
                set(_test_debug_name "openvino_cd.dll")
            else()
                string(REPLACE ".dll" "d.dll" _test_debug_name "${_test_runtime_name}")
            endif()
            add_custom_command(TARGET test_sync_inference POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<IF:$<CONFIG:Debug>,${_test_openvino_debug_dir}/${_test_debug_name},${_test_openvino_release_dir}/${_test_runtime_name}>"
                    "$<TARGET_FILE_DIR:test_sync_inference>"
            )
        endforeach()
        add_custom_command(TARGET test_sync_inference POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<IF:$<CONFIG:Debug>,${OPENVINO_ROOT}/runtime/3rdparty/tbb/bin/tbb12_debug.dll,${OPENVINO_ROOT}/runtime/3rdparty/tbb/bin/tbb12.dll>"
                "$<TARGET_FILE_DIR:test_sync_inference>"
        )
    endif()
    add_test(NAME test_sync_inference
        COMMAND test_sync_inference
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    add_test(NAME test_sync_inference_detection_boxes
        COMMAND test_sync_inference --test-detection-boxes)
endif()
