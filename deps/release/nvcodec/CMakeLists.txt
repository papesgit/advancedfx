cmake_minimum_required (VERSION 3.24)

project ("nvcodec")

#
# NVIDIA Video Codec SDK
#

# Create an interface library for nvcodec
add_library(nvcodec INTERFACE)

# Set include directories
set(NVCODEC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(NVCODEC_SAMPLES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/samples)
target_include_directories(nvcodec INTERFACE
    ${NVCODEC_INCLUDE_DIR}
    ${NVCODEC_SAMPLES_DIR}/NvEncoder
    ${NVCODEC_SAMPLES_DIR}/Utils
)

# Set library directories based on platform
if(CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
    set(NVCODEC_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/Win32)
elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
    set(NVCODEC_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/x64)
endif()

# Link libraries
target_link_libraries(nvcodec INTERFACE
    ${NVCODEC_LIB_DIR}/nvencodeapi.lib
    ${NVCODEC_LIB_DIR}/nvcuvid.lib
)

# Sample source files
set(NVCODEC_SAMPLE_SOURCES
    ${NVCODEC_SAMPLES_DIR}/NvEncoder/NvEncoder.cpp
    ${NVCODEC_SAMPLES_DIR}/NvEncoder/NvEncoder.h
    ${NVCODEC_SAMPLES_DIR}/NvEncoder/NvEncoderD3D11.cpp
    ${NVCODEC_SAMPLES_DIR}/NvEncoder/NvEncoderD3D11.h
    ${NVCODEC_SAMPLES_DIR}/Utils/Logger.h
    ${NVCODEC_SAMPLES_DIR}/Utils/NvCodecUtils.h
)

# Export variables for parent scope
set(AFX_NVCODEC_INCLUDE_DIRECTORIES
    ${NVCODEC_INCLUDE_DIR}
    ${NVCODEC_SAMPLES_DIR}/NvEncoder
    ${NVCODEC_SAMPLES_DIR}/Utils
    PARENT_SCOPE
)
set(AFX_NVCODEC_LIB_DIR ${NVCODEC_LIB_DIR} PARENT_SCOPE)
set(AFX_NVCODEC_LINK_LIBRARIES
    ${NVCODEC_LIB_DIR}/nvencodeapi.lib
    ${NVCODEC_LIB_DIR}/nvcuvid.lib
    PARENT_SCOPE
)
set(AFX_NVCODEC_SAMPLE_SOURCES ${NVCODEC_SAMPLE_SOURCES} PARENT_SCOPE)
