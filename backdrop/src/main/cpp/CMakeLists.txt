# the minimum version of CMake.
cmake_minimum_required(VERSION 3.5.0)
project(backdrop)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

include_directories(${NATIVERENDER_ROOT_PATH})

# GLES 自渲染需要显式查找 EGL / GLESv3。
find_library(EGL-lib EGL)
find_library(GLES-lib GLESv3)

add_library(backdrop SHARED
    napi_init.cpp
    glass/glass_napi.cpp
    glass/text_layer.cpp
    glass/glass_params.cpp
    glass/gles_renderer.cpp
)

target_link_libraries(backdrop PUBLIC libace_napi.z.so)
target_link_libraries(backdrop PUBLIC libhilog_ndk.z.so)
target_link_libraries(backdrop PUBLIC ${EGL-lib} ${GLES-lib} libc++_shared.so)
# SurfaceHolder / XComponent NDK + 原生窗口 + native_drawing（文字栅格化）
target_link_libraries(backdrop PUBLIC libace_ndk.z.so libnative_window.so libnative_drawing.so)
# OH_NativeImage：BuilderNode 纹理导出（RENDER_TYPE_TEXTURE）的接收端
target_link_libraries(backdrop PUBLIC libnative_image.so)
# OH_NativeVSync：出帧对齐 vsync（每周期最多一帧，见 GlesRenderer::WaitForRenderSlot）
target_link_libraries(backdrop PUBLIC libnative_vsync.so)
