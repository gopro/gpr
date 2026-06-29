# GPR SDK conversion test suite
cmake_minimum_required( VERSION 3.5 FATAL_ERROR )

set( EXE_NAME gpr_tools_tests )

file( GLOB TEST_SRC_FILES "*.cpp" )

# Resolve paths relative to this file so they are correct regardless of how the
# project is configured (in-tree or otherwise). ../lib == source/lib.
set( GPR_LIB_DIR  "${CMAKE_CURRENT_LIST_DIR}/../lib" )
set( GPR_DATA_DIR "${CMAKE_CURRENT_LIST_DIR}/../../data/samples" )

add_executable( ${EXE_NAME} ${TEST_SRC_FILES} )

# Include dirs scoped to this target (matches the gpr_tools app's include set).
target_include_directories( ${EXE_NAME} PRIVATE
  "${GPR_LIB_DIR}/common/public"
  "${GPR_LIB_DIR}/vc5_common"
  "${GPR_LIB_DIR}/dng_sdk"
  "${GPR_LIB_DIR}/gpr_sdk/public" )

# Bake in the path to the bundled samples (overridable via argv[1]).
target_compile_definitions( ${EXE_NAME} PRIVATE "GPR_TESTS_DATA_DIR=\"${GPR_DATA_DIR}\"" )

target_link_libraries( ${EXE_NAME} gpr_sdk dng_sdk xmp_core vc5_common common md5_lib expat_lib )

if(EXISTS "${GPR_LIB_DIR}/vc5_decoder")
  target_include_directories( ${EXE_NAME} PRIVATE "${GPR_LIB_DIR}/vc5_decoder" )
  target_compile_definitions( ${EXE_NAME} PRIVATE GPR_READING=1 )
  target_link_libraries( ${EXE_NAME} vc5_decoder )
else()
  target_compile_definitions( ${EXE_NAME} PRIVATE GPR_READING=0 )
endif()

if(EXISTS "${GPR_LIB_DIR}/vc5_encoder")
  target_include_directories( ${EXE_NAME} PRIVATE "${GPR_LIB_DIR}/vc5_encoder" )
  target_compile_definitions( ${EXE_NAME} PRIVATE GPR_WRITING=1 )
  target_link_libraries( ${EXE_NAME} vc5_encoder )
else()
  target_compile_definitions( ${EXE_NAME} PRIVATE GPR_WRITING=0 )
endif()

if(EXISTS "${GPR_LIB_DIR}/tiny_jpeg")
  target_include_directories( ${EXE_NAME} PRIVATE "${GPR_LIB_DIR}/tiny_jpeg" )
  target_compile_definitions( ${EXE_NAME} PRIVATE GPR_JPEG_AVAILABLE=1 )
  target_link_libraries( ${EXE_NAME} tiny_jpeg )
else()
  target_compile_definitions( ${EXE_NAME} PRIVATE GPR_JPEG_AVAILABLE=0 )
endif()

set_target_properties( ${EXE_NAME} PROPERTIES FOLDER test )

add_test( NAME gpr_conversion_tests COMMAND ${EXE_NAME} )
