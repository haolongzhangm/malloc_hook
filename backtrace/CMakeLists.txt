aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src SRC_LIST)
add_library(helper STATIC ${SRC_LIST})
target_include_directories(helper PUBLIC 
                            ${CMAKE_CURRENT_SOURCE_DIR}
                            ${CMAKE_CURRENT_SOURCE_DIR}/driver
                            ${CMAKE_CURRENT_SOURCE_DIR}/include
                        )
target_link_libraries(helper unwindstack)
if (CMAKE_CXX_COMPILER_ID STREQUAL Clang)
    target_compile_options(helper PRIVATE -fno-c++-static-destructors)
endif ()