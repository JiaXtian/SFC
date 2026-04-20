cmake_minimum_required(VERSION 3.15)
project(SFC_Orchestration VERSION 1.0 LANGUAGES CXX)

# C++17标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -DNDEBUG")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g -Wall -Wextra")
endif()

# 查找依赖
find_package(Threads REQUIRED)

# ONNX Runtime
# 优先级:
# 1) -DONNXRUNTIME_DIR=...
# 2) 环境变量 ONNXRUNTIME_DIR
# 3) Homebrew / 手动安装常见路径
if(NOT ONNXRUNTIME_DIR)
    if(DEFINED ENV{ONNXRUNTIME_DIR})
        set(ONNXRUNTIME_DIR $ENV{ONNXRUNTIME_DIR})
    elseif(EXISTS "/opt/homebrew/opt/onnxruntime")
        set(ONNXRUNTIME_DIR "/opt/homebrew/opt/onnxruntime")
    elseif(EXISTS "/usr/local/opt/onnxruntime")
        set(ONNXRUNTIME_DIR "/usr/local/opt/onnxruntime")
    else()
        set(ONNXRUNTIME_DIR "")
    endif()
endif()

find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    PATHS
        ${ONNXRUNTIME_DIR}/include
        /opt/homebrew/include
        /usr/local/include
    PATH_SUFFIXES
        onnxruntime
        onnxruntime/core/session
)

find_library(ONNXRUNTIME_LIB onnxruntime
    PATHS
        ${ONNXRUNTIME_DIR}/lib
        /opt/homebrew/lib
        /usr/local/lib
)

if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIB)
    message(FATAL_ERROR
        "ONNX Runtime not found.\n"
        "Please install it or set ONNXRUNTIME_DIR.\n"
        "Expected header: onnxruntime_cxx_api.h\n"
        "Detected include: ${ONNXRUNTIME_INCLUDE_DIR}\n"
        "Detected lib: ${ONNXRUNTIME_LIB}\n"
    )
endif()

message(STATUS "ONNX Runtime include: ${ONNXRUNTIME_INCLUDE_DIR}")
message(STATUS "ONNX Runtime lib: ${ONNXRUNTIME_LIB}")

# nlohmann/json (header-only)
find_package(nlohmann_json REQUIRED)

# 源文件
set(SOURCES
    src/network_graph.cpp
    src/heuristic_pruner.cpp
    src/drl_inference.cpp
    src/sfc_orchestrator.cpp
    src/main.cpp
)

# 头文件目录
include_directories(
    include
    ${ONNXRUNTIME_INCLUDE_DIR}
)

# 可执行文件
add_executable(sfc_inference ${SOURCES})
add_executable(model_benchmark ${SOURCES})

# Ensure macOS can find libonnxruntime at runtime without DYLD_LIBRARY_PATH.
if(APPLE)
    get_filename_component(ONNXRUNTIME_LIB_DIR ${ONNXRUNTIME_LIB} DIRECTORY)
    set_target_properties(sfc_inference PROPERTIES
        BUILD_RPATH "${ONNXRUNTIME_LIB_DIR}"
        INSTALL_RPATH "${ONNXRUNTIME_LIB_DIR}"
    )
    set_target_properties(model_benchmark PROPERTIES
        BUILD_RPATH "${ONNXRUNTIME_LIB_DIR}"
        INSTALL_RPATH "${ONNXRUNTIME_LIB_DIR}"
    )
endif()

# 链接库
target_link_libraries(sfc_inference
    PRIVATE
        Threads::Threads
        ${ONNXRUNTIME_LIB}
        nlohmann_json::nlohmann_json
)
target_link_libraries(model_benchmark
    PRIVATE
        Threads::Threads
        ${ONNXRUNTIME_LIB}
        nlohmann_json::nlohmann_json
)

# 安装
install(TARGETS sfc_inference model_benchmark DESTINATION bin)

# 打印配置信息
message(STATUS "")
message(STATUS "======================================")
message(STATUS "  SFC Orchestration System Build")
message(STATUS "======================================")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "C++ flags: ${CMAKE_CXX_FLAGS}")
message(STATUS "ONNX Runtime dir: ${ONNXRUNTIME_DIR}")
message(STATUS "ONNX Runtime include: ${ONNXRUNTIME_INCLUDE_DIR}")
message(STATUS "ONNX Runtime lib: ${ONNXRUNTIME_LIB}")
message(STATUS "======================================")
message(STATUS "")
