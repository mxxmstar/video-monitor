# 测试 CMakeLists.txt

# 收集所有测试源文件
file(GLOB_RECURSE TEST_SOURCES "*.cpp")

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
    
    # 复制 Boost DLL 到输出目录（Windows）
    if(WIN32)
        add_custom_command(TARGET ${test_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DEFAULT_DEPS_ROOT}/debug/bin/boost_process-vc145-mt-gd-x64-1_91.dll"
                $<TARGET_FILE_DIR:${test_target}>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DEFAULT_DEPS_ROOT}/debug/bin/boost_filesystem-vc145-mt-gd-x64-1_91.dll"
                $<TARGET_FILE_DIR:${test_target}>
        )
    endif()
    
    # 注册测试
    add_test(NAME ${test_target}
        COMMAND ${test_target}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endforeach()