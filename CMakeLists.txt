cmake_minimum_required(VERSION 3.23)

set(VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/version_script.ld)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O3 -fPIC")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(ANDROID OR OHOS)
  # now old ANDROID have libc++_shared.so, new ANDROID have libc++.so so we use
  # libc++_static by default, by set ANDROID_STL=c++_static only take effect on old NDK,
  # so we force config by use xxx_LINKER_FLAGS
  message(STATUS "config static-libstdc++ for ANDROID or OHOS")
  set(CMAKE_EXE_LINKER_FLAGS "-fuse-ld=lld")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static-libstdc++")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -static-libstdc++")
endif()
# Check if using Clang
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--version-script=${VERSION_SCRIPT}")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

add_subdirectory(aosp_file)

if (ANDROID_ABI MATCHES "arm64-v8a")
    set(VNDK_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/aosp_file/com.android.vndk.v30/lib64")
    set(APEX_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/aosp_file/com.android.runtime/lib64")
else ()
    set(VNDK_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/aosp_file/com.android.vndk.v30/lib")
    set(APEX_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/aosp_file/com.android.runtime/lib")
endif ()

add_library(dma_alloc_hook SHARED ${CMAKE_SOURCE_DIR}/src/dma_alloc_hook.cpp)
target_link_libraries(dma_alloc_hook PRIVATE backtrace_header log)
install(TARGETS dma_alloc_hook DESTINATION ${CMAKE_INSTALL_PREFIX}/out/lib)

# copy database compile_commands.json to PROJECT_SOURCE_DIR
add_custom_target(
  copy_database_compile_commands_mem_trace ALL
  COMMAND
    ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_BINARY_DIR}/compile_commands.json
    ${PROJECT_SOURCE_DIR}/compile_commands.json)
