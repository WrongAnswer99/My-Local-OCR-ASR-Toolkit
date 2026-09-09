cmake_minimum_required(VERSION 3.16)
project(ppocr_onnx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 本工程基于 PaddleOCR(PP-OCRv4/v6) 推理模型 + ONNX Runtime，
# 用 g++(MinGW) + CMake 构建，算法参照 RapidOCR(python) 移植。
#
# 可执行目标：
#   ppocr_onnx  : 单张图片 OCR
#   ocrwatch    : 持续截图某屏幕区域并 OCR，命中关键字后执行动作
#   ocrmon      : 框选区域后持续打印识别文字
#   region_picker : 全屏框选并输出 rect
#   screen_point_picker : 点击穿透的屏幕选点与坐标记录工具
#   api_demo    : SDK(sdk/ocr.hpp) 用法示例
#   ocr_sdk     : 对外分发动态库 ocr.dll

# ================= 第三方依赖(手动放置, 不联网自动下载) =================
# 与 My-Game-Demo/CMakeLists.txt 相同的策略: 启动时检查 third_party/ 里
# 是否已放好依赖, 缺失就直接 FATAL_ERROR 并给出获取方式。
set(THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# ---- ONNX Runtime(CPU 预编译包) ----
# 编译期只需 include/ 下的 C API 头; 运行期由 onnx_session.cpp
# LoadLibrary(onnxruntime.dll) 延迟加载, 因此还要 lib/onnxruntime.dll。
set(ONNXRUNTIME_DIR "${THIRD_PARTY}/onnxruntime")
if(NOT EXISTS "${ONNXRUNTIME_DIR}/include/onnxruntime_c_api.h")
    message(FATAL_ERROR
        "onnxruntime not found in third_party/onnxruntime.\n"
        "Please download the ONNX Runtime (windows x64 CPU) release zip from:\n"
        "  https://github.com/microsoft/onnxruntime/releases\n"
        "and extract it so that include/ and lib/ land under:\n"
        "  ${ONNXRUNTIME_DIR}")
endif()
if(NOT EXISTS "${ONNXRUNTIME_DIR}/lib/onnxruntime.dll")
    message(FATAL_ERROR
        "onnxruntime.dll not found in ${ONNXRUNTIME_DIR}/lib.\n"
        "It is loaded at runtime (LoadLibrary), please include the full lib/"
        "folder of the ONNX Runtime release zip (see above).")
endif()
message(STATUS "Found ONNX Runtime headers: ${ONNXRUNTIME_DIR}/include")

# ---- stb(header-only 图像编解码) ----
set(STB_DIR "${THIRD_PARTY}/stb")
if(NOT EXISTS "${STB_DIR}/stb_image.h")
    message(FATAL_ERROR
        "stb not found in ${STB_DIR}.\n"
        "Please get the two single-file headers (stb_image.h / "
        "stb_image_write.h) from:\n"
        "  https://github.com/nothings/stb\n"
        "or clone it with:\n"
        "  git clone --depth 1 https://github.com/nothings/stb.git ${STB_DIR}")
endif()
message(STATUS "Found stb headers: ${STB_DIR}")

# onnxruntime / stb 都没有本地 CMakeLists, 无法 add_subdirectory;
# 用 INTERFACE 目标登记头文件路径, 之后 target_link_libraries 即附带 include。
add_library(onnxruntime INTERFACE)
target_include_directories(onnxruntime INTERFACE ${ONNXRUNTIME_DIR}/include)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${STB_DIR})

# 通用 include(工程自身头文件)
include_directories(
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/sdk
)

# ================= 目标 =================

# ---- OCR 核心静态库(image/onnx_session 需要 stb 与 onnxruntime 头) ----
add_library(ocr STATIC
  src/image.cpp
  src/onnx_session.cpp
  src/ocr.cpp
)
if(WIN32)
  target_compile_definitions(ocr PRIVATE NOMINMAX)
endif()
target_link_libraries(ocr PRIVATE onnxruntime stb)

# ---- 命令行 OCR ----
add_executable(ppocr_onnx src/main.cpp)
target_link_libraries(ppocr_onnx PRIVATE ocr)

# ---- 屏幕区域监听 OCR ----
add_executable(ocrwatch src/ocrwatch.cpp src/screen.cpp)
target_link_libraries(ocrwatch PRIVATE ocr user32 gdi32)

# ---- 框选区域工具(纯 Win32) ----
add_executable(region_picker src/region_picker.cpp src/region_select.cpp)
target_link_libraries(region_picker PRIVATE user32 gdi32)

# ---- 屏幕选点工具(点击穿透，只观察并记录点击) ----
add_executable(screen_point_picker src/screen_point_picker.cpp)
target_link_libraries(screen_point_picker PRIVATE user32 gdi32)

# ---- 演示: 框选后持续 OCR ----
add_executable(ocrmon src/ocrmon.cpp src/region_select.cpp src/screen.cpp)
target_link_libraries(ocrmon PRIVATE ocr user32 gdi32)

# ---- SDK 动态库(对外分发: ocr.dll + sdk/ocr.hpp) ----
# C ABI 导出(见 src/ocr_api.hpp); 静态链接 gcc 运行库, 便于对方免装运行库。
add_library(ocr_sdk SHARED
  src/ocr_c_api.cpp
  src/onnx_session.cpp
  src/ocr.cpp
  src/image.cpp
  src/screen.cpp
  src/region_select.cpp
)
target_compile_definitions(ocr_sdk PRIVATE OCR_BUILD_DLL NOMINMAX)
target_link_libraries(ocr_sdk PRIVATE onnxruntime stb user32 gdi32)
set_target_properties(ocr_sdk PROPERTIES OUTPUT_NAME ocr PREFIX "")

# ---- SDK 用法示例(不链接 dll, 运行时 LoadLibrary) ----
add_executable(api_demo sdk/examples/api_demo.cpp)

target_compile_options(ocr PRIVATE -O2)
target_compile_options(ppocr_onnx PRIVATE -O2)
target_compile_options(ocrwatch PRIVATE -O2)
target_compile_options(region_picker PRIVATE -O2)
target_compile_options(screen_point_picker PRIVATE -O2)
target_compile_options(ocrmon PRIVATE -O2)
target_compile_options(ocr_sdk PRIVATE -O2 -static -static-libgcc -static-libstdc++)
target_compile_options(api_demo PRIVATE -O2)

# 运行目录放一份 onnxruntime.dll，程序启动时用 LoadLibrary 加载
foreach(target ppocr_onnx ocrwatch ocrmon api_demo)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ONNXRUNTIME_DIR}/lib/onnxruntime.dll
            $<TARGET_FILE_DIR:${target}>/onnxruntime.dll
  )
endforeach()
