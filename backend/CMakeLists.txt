cmake_minimum_required(VERSION 3.15)
project(sfc_visualization_backend CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 查找依赖
find_package(Drogon REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(spdlog REQUIRED)

# ONNX Runtime
if(DEFINED ENV{ONNXRUNTIME_DIR})
    set(ONNXRUNTIME_ROOT_DIR $ENV{ONNXRUNTIME_DIR})
elseif(EXISTS "/opt/homebrew/opt/onnxruntime")
    set(ONNXRUNTIME_ROOT_DIR "/opt/homebrew/opt/onnxruntime")
else()
    set(ONNXRUNTIME_ROOT_DIR "/usr/local/onnxruntime")
endif()

find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    PATHS ${ONNXRUNTIME_ROOT_DIR}/include
    PATH_SUFFIXES onnxruntime core/session)

find_library(ONNXRUNTIME_LIB onnxruntime
    PATHS ${ONNXRUNTIME_ROOT_DIR}/lib)

if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIB)
    message(FATAL_ERROR "ONNX Runtime not found. Set ONNXRUNTIME_DIR environment variable")
endif()

message(STATUS "ONNX Runtime found:")
message(STATUS "  Include: ${ONNXRUNTIME_INCLUDE_DIR}")
message(STATUS "  Library: ${ONNXRUNTIME_LIB}")

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${ONNXRUNTIME_INCLUDE_DIR}
)

# 源文件
file(GLOB_RECURSE SOURCES "src/*.cpp")

# 可执行文件
add_executable(sfc_server ${SOURCES})

target_link_libraries(sfc_server
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    ${ONNXRUNTIME_LIB}
)

# 编译选项
target_compile_options(sfc_server PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O3 -march=native>
    $<$<CONFIG:Debug>:-g -O0>
)

# 安装
install(TARGETS sfc_server DESTINATION bin)
