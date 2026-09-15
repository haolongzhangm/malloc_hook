#!/usr/bin/env bash
set -ex

BUILD_ARCH="arm64-v8a"
if [ -n "$1" ]; then
    BUILD_ARCH=$1
fi

SRC_DIR=$(readlink -f "`dirname $0`")
cd ${SRC_DIR}

# check NDK_ROOT env variable
if [ -z "$NDK_ROOT" ]; then
    echo "Please set NDK_ROOT environment variable"
    exit 1
else
    echo "use $NDK_ROOT to build for android"
fi

# echo "check NDK is invalid or not..."
# if [ ! -f "$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang++" ]; then
#     echo "NDK_ROOT is invalid"
#     exit 1
# fi

CMAKE_PARA_ANDROID="-DCMAKE_TOOLCHAIN_FILE=${NDK_ROOT}/build/cmake/android.toolchain.cmake \
	-DANDROID_NDK=${NDK_ROOT} \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_TOOLCHAIN=clang++ \
    -DANDROID_PLATFORM=android-28"

CMAKE_PARA_ARMV7="-DCMAKE_TOOLCHAIN_FILE=${NDK_ROOT}/build/cmake/android.toolchain.cmake \
    -DANDROID_NDK=${NDK_ROOT} \
    -DANDROID_ABI=armeabi-v7a \
    -DANDROID_TOOLCHAIN=clang++ \
    -DANDROID_PLATFORM=android-28"

CMAKE_PARA=" -DCMAKE_INSTALL_PREFIX=${SRC_DIR} \
    -DCMAKE_BUILD_TYPE=Release -G Ninja"

function cmake_para_gen(){ 
    case ${BUILD_ARCH} in
        "arm64-v8a")
            CMAKE_PARA="${CMAKE_PARA} ${CMAKE_PARA_ANDROID}"
            ;;
        "armeabi-v7a")
            CMAKE_PARA="${CMAKE_PARA} ${CMAKE_PARA_ARMV7}"
            ;;
        *)
            echo "BUILD_ARCH must be either 'arm64-v8a' or 'armeabi-v7a'"
            exit 1
            ;;
    esac
}

function build_sdk(){ 
    rm -rf build
    rm -rf out
    mkdir -p out/lib
    mkdir build && cd build

    cmake ${CMAKE_PARA} .. 
    ninja install -v
}

cmake_para_gen
build_sdk
