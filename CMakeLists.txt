# minimum required cmake version
cmake_minimum_required( VERSION 3.5 FATAL_ERROR )

set(CMAKE_SUPPRESS_REGENERATION true)

set(CMAKE_C_FLAGS "-std=c99")

# project name
project( gpr )

if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )

  execute_process(
    COMMAND git log -1 --format=%h
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
else(EXISTS "${CMAKE_SOURCE_DIR}/.git")
  set(GIT_BRANCH "")
  set(GIT_COMMIT_HASH "")
endif(EXISTS "${CMAKE_SOURCE_DIR}/.git")

message(STATUS "Git current branch: ${GIT_BRANCH}")
message(STATUS "Git commit hash: ${GIT_COMMIT_HASH}")

add_definitions("-DGIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\"")
add_definitions("-DGIT_BRANCH=\"${GIT_BRANCH}\"")

# ---------------------------------------------------------------------------
# NEON SIMD support
#
# The VC5 codec has hand-written NEON kernels (currently in the encoder), gated
# on the GPR_NEON preprocessor symbol which defaults to 0 in gpr_platform.h. Detect
# ARM here and define GPR_NEON=1 for the whole project so every subdirectory added
# below (the encoder today, the decoder if kernels are added later) inherits the
# flag. Force the scalar path with -DGPR_NEON=OFF.
# ---------------------------------------------------------------------------
option( GPR_NEON "Enable NEON SIMD kernels on ARM" ON )

if( GPR_NEON AND ( CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64" ) )
  add_definitions( "-DGPR_NEON=1" )
  message( STATUS "NEON SIMD: available (arm64) - enabling (GPR_NEON=1)" )
else()
  message( STATUS "NEON SIMD: not enabled - using scalar path (set GPR_NEON=ON on an arm64 target to enable)" )
endif()

# ---------------------------------------------------------------------------
# Optional codec features
#
# Each feature is available only when its source directory is present, and can
# additionally be switched off with the matching CMake option (e.g.
# -DGPR_READING=OFF). The GPR_*_AVAILABLE variables below hold the combined
# result and are consumed by the subdirectory CMakeLists to set the
# GPR_READING / GPR_WRITING / GPR_JPEG_AVAILABLE preprocessor symbols.
# ---------------------------------------------------------------------------
option( GPR_READING "Enable GPR/DNG reading (VC5 decoder)" ON )
option( GPR_WRITING "Enable GPR writing (VC5 encoder)" ON )
option( GPR_JPEG_AVAILABLE "Enable jpeg thumbnail writing (tiny_jpeg)" ON )
option( GPR_TIMING "Enable timing output" ON )

if( NOT GPR_TIMING )
  # gpr_platform.h defaults GPR_TIMING to 1; force all timing code off
  add_definitions( "-DGPR_TIMING=0" )
  message( STATUS "Timing: disabled (GPR_TIMING=0)" )
endif()

# add needed subdirectories
add_subdirectory( "source/lib/common" )
add_subdirectory( "source/lib/vc5_common" )

if( GPR_READING AND EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_decoder" )
  set( GPR_READING_AVAILABLE ON )
  MESSAGE( STATUS "Found source/lib/vc5_decoder - enabling vc5 decoder and setting GPR_READING=1" )
  add_subdirectory( "source/lib/vc5_decoder" )
  add_subdirectory( "source/app/vc5_decoder_app" )
else()
  set( GPR_READING_AVAILABLE OFF )
  MESSAGE( STATUS "vc5 decoder disabled (missing source/lib/vc5_decoder or GPR_READING=OFF) - setting GPR_READING=0" )
endif()

if( GPR_WRITING AND EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_encoder" )
  set( GPR_WRITING_AVAILABLE ON )
  MESSAGE( STATUS "Found source/lib/vc5_encoder - enabling vc5 encoder and setting GPR_WRITING=1" )
  add_subdirectory( "source/lib/vc5_encoder" )
  add_subdirectory( "source/app/vc5_encoder_app" )
else()
  set( GPR_WRITING_AVAILABLE OFF )
  MESSAGE( STATUS "vc5 encoder disabled (missing source/lib/vc5_encoder or GPR_WRITING=OFF) - setting GPR_WRITING=0" )
endif()

# GPR_JPEG_WRITER_AVAILABLE must be decided before gpr_sdk is added: the
# gpr_sdk CMakeLists reads it to set the GPR_JPEG_AVAILABLE preprocessor
# symbol that compiles the body of gpr_convert_gpr_to_jpg into gpr.cpp.
if( GPR_JPEG_AVAILABLE AND EXISTS "${CMAKE_SOURCE_DIR}/source/lib/tiny_jpeg" )
  set( GPR_JPEG_WRITER_AVAILABLE ON )
  MESSAGE( STATUS "Found source/lib/tiny_jpeg - enabling jpeg writing and setting GPR_JPEG_AVAILABLE=1" )
  add_subdirectory( "source/lib/tiny_jpeg" )
else()
  set( GPR_JPEG_WRITER_AVAILABLE OFF )
  MESSAGE( STATUS "jpeg writing disabled (missing source/lib/tiny_jpeg or GPR_JPEG_AVAILABLE=OFF) - setting GPR_JPEG_AVAILABLE=0" )
endif()

add_subdirectory( "source/lib/dng_sdk" )
add_subdirectory( "source/lib/gpr_sdk" )
add_subdirectory( "source/lib/xmp_core" )
add_subdirectory( "source/lib/expat_lib" )
add_subdirectory( "source/lib/md5_lib" )

add_subdirectory( "source/app/common/cJSON" )
add_subdirectory( "source/app/common/argument_parser" )
add_subdirectory( "source/app/gpr_tools" )

# Conversion test suite (needs both the vc5 encoder and decoder available)
if( GPR_READING_AVAILABLE AND GPR_WRITING_AVAILABLE AND EXISTS "${CMAKE_SOURCE_DIR}/source/test/CMakeLists.txt" )
  enable_testing()
  add_subdirectory( "source/test" )
endif()
