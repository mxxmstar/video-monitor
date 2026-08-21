# 媒体测试 CMakeLists.txt

file(GLOB MEDIA_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

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
