# 媒体测试 CMakeLists.txt

file(GLOB MEDIA_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# 这个测试只依赖 FFmpeg 和媒体适配代码，单独构建可以在旧版 MinGW
# 无法编译 common/process 时仍验证 raw AVFrame 契约。
list(FILTER MEDIA_TEST_SOURCES EXCLUDE REGEX ".*/test_raw_frame_contract\\.cpp$")

foreach(test_file ${MEDIA_TEST_SOURCES})
    get_filename_component(test_name ${test_file} NAME_WE)
    set(test_target "test_media_${test_name}")

    add_executable(${test_target} ${test_file})

    target_link_libraries(${test_target}
        PRIVATE
            common_lib
    )

    set_target_properties(${test_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
    )

    copy_runtime_dependencies(${test_target})

    add_test(NAME ${test_target}
        COMMAND ${test_target}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endforeach()

set(RAW_FRAME_CONTRACT_TEST "${CMAKE_CURRENT_SOURCE_DIR}/test_raw_frame_contract.cpp")
if(EXISTS "${RAW_FRAME_CONTRACT_TEST}")
    add_executable(test_media_raw_frame_contract
        "${RAW_FRAME_CONTRACT_TEST}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src/media/ffmpeg_raw_frame_buffer.cpp"
    )
    target_include_directories(test_media_raw_frame_contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../include
            ${FFMPEG_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    )
    target_link_libraries(test_media_raw_frame_contract
        PRIVATE
            "${FFMPEG_LIB_DIR}/avutil.lib"
    )
    set_target_properties(test_media_raw_frame_contract PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
    )
    add_custom_command(TARGET test_media_raw_frame_contract POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_ROOT}/bin/avutil-60.dll"
            "$<TARGET_FILE_DIR:test_media_raw_frame_contract>"
    )
    add_test(NAME test_media_raw_frame_contract
        COMMAND test_media_raw_frame_contract
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
