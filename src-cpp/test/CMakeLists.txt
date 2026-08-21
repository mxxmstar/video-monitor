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
