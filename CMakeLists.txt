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
# on the NEON preprocessor symbol which defaults to 0 in gpr_platform.h. Detect
# ARM here and define NEON=1 for the whole project so every subdirectory added
# below (the encoder today, the decoder if kernels are added later) inherits the
# flag. Force the scalar path with -DGPR_ENABLE_NEON=OFF.
# ---------------------------------------------------------------------------
option( GPR_ENABLE_NEON "Enable NEON SIMD kernels on ARM" ON )

if( GPR_ENABLE_NEON AND ( CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64" ) )
  set( GPR_NEON_ENABLED ON )
  add_definitions( "-DNEON=1" )
  message( STATUS "NEON SIMD: available (arm64) - enabling (NEON=1)" )
else()
  set( GPR_NEON_ENABLED OFF )
  message( STATUS "NEON SIMD: not enabled - using scalar path (set GPR_ENABLE_NEON=ON on an arm64 target to enable)" )
endif()

# add needed subdirectories
add_subdirectory( "source/lib/common" )
add_subdirectory( "source/lib/vc5_common" )

if(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_decoder")
  MESSAGE( STATUS "Found source/lib/vc5_decoder - enabling vc5 decoder and setting GPR_READING=1" )
  add_subdirectory( "source/lib/vc5_decoder" )
else(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_decoder")
  MESSAGE( STATUS "Could not find source/lib/vc5_decoder - disabling vc5 decoder and setting GPR_READING=0" )
endif(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_decoder")

if(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_encoder")
  MESSAGE( STATUS "Found source/lib/vc5_encoder - enabling vc5 encoder and setting GPR_WRITING=1" )
  add_subdirectory( "source/lib/vc5_encoder" )
else(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_encoder")
  MESSAGE( STATUS "Could not find source/lib/vc5_encoder - disabling vc5 encoder and setting GPR_WRITING=0" )
endif(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_encoder")

add_subdirectory( "source/lib/dng_sdk" )
add_subdirectory( "source/lib/gpr_sdk" )
add_subdirectory( "source/lib/xmp_core" )
add_subdirectory( "source/lib/expat_lib" )
add_subdirectory( "source/lib/md5_lib" )

if(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/tiny_jpeg")
  MESSAGE( STATUS "Found source/lib/tiny_jpeg - enabling jpeg writing and setting GPR_JPEG_AVAILABLE=1" )
  add_subdirectory( "source/lib/tiny_jpeg" )
else(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/tiny_jpeg")
  MESSAGE( STATUS "Could not find source/lib/tiny_jpeg - disabling jpeg writing and setting GPR_JPEG_AVAILABLE=0" )
endif(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/tiny_jpeg")

add_subdirectory( "source/app/common/cJSON" )
add_subdirectory( "source/app/common/argument_parser" )
add_subdirectory( "source/app/gpr_tools" )

# Conversion test suite (needs both the vc5 encoder and decoder available)
if(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_decoder" AND EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_encoder")
  enable_testing()
  add_subdirectory( "source/test" )
endif()
