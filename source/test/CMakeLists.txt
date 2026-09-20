# GPR SDK conversion test suite
cmake_minimum_required( VERSION 3.5 FATAL_ERROR )

set( EXE_NAME gpr_tools_tests )

file( GLOB TEST_SRC_FILES "*.cpp" )

# Resolve paths relative to this file so they are correct regardless of how the
# project is configured (in-tree or otherwise). ../lib == source/lib.
set( GPR_LIB_DIR   "${CMAKE_CURRENT_LIST_DIR}/../lib" )
set( GPR_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../app/gpr_tools" )
set( GPR_DATA_DIR  "${CMAKE_CURRENT_LIST_DIR}/../../data/samples" )

# The suite also drives gpr_tools' CLI conversion layer (dng_convert_main, e.g.
# the --preview argument), so the app sources implementing it are compiled in.
list( APPEND TEST_SRC_FILES
  "${GPR_TOOLS_DIR}/main_c.c"
  "${GPR_TOOLS_DIR}/gpr_parse_utils.cpp"
  "${GPR_TOOLS_DIR}/gpr_print_utils.cpp" )

add_executable( ${EXE_NAME} ${TEST_SRC_FILES} )

# Include dirs scoped to this target (matches the gpr_tools app's include set).
target_include_directories( ${EXE_NAME} PRIVATE
  "${GPR_TOOLS_DIR}"
  "${CMAKE_CURRENT_LIST_DIR}/../app/common/cJSON"
  "${CMAKE_CURRENT_LIST_DIR}/../app/common/argument_parser"
  "${GPR_LIB_DIR}/common/public"
  "${GPR_LIB_DIR}/common/private"
  "${GPR_LIB_DIR}/vc5_common"
  "${GPR_LIB_DIR}/dng_sdk"
  "${GPR_LIB_DIR}/gpr_sdk/public"
  "${GPR_LIB_DIR}/gpr_sdk/private" )

# Bake in the path to the bundled samples (overridable via argv[1]).
target_compile_definitions( ${EXE_NAME} PRIVATE "GPR_TESTS_DATA_DIR=\"${GPR_DATA_DIR}\"" )

# Base link set; the vc5/tiny_jpeg libraries are appended conditionally below.
# GNU ld resolves symbols left to right, so libraries must appear after the
# libraries that use them: vc5_common/common/md5_lib/expat_lib are linked last
# again after the conditional libraries (repeating a static lib is harmless).
target_link_libraries( ${EXE_NAME} gpr_sdk dng_sdk xmp_core vc5_common common md5_lib expat_lib cJSON argument_parser )

# GPR_*_AVAILABLE variables come from the top-level CMakeLists.txt (feature
# option combined with the presence of the corresponding source directory).
# This suite is only added when reading and writing are both available, but
# check anyway so the defines stay correct if that ever changes.
if(GPR_READING_AVAILABLE)
  target_include_directories( ${EXE_NAME} PRIVATE "${GPR_LIB_DIR}/vc5_decoder" )
  target_compile_definitions( ${EXE_NAME} PRIVATE GPR_READING=1 )
  target_link_libraries( ${EXE_NAME} vc5_decoder )
else()
  target_compile_definitions( ${EXE_NAME} PRIVATE GPR_READING=0 )
endif()

if(GPR_WRITING_AVAILABLE)
  target_include_directories( ${EXE_NAME} PRIVATE "${GPR_LIB_DIR}/vc5_encoder" )
  target_compile_definitions( ${EXE_NAME} PRIVATE GPR_WRITING=1 )
  target_link_libraries( ${EXE_NAME} vc5_encoder )
else()
  target_compile_definitions( ${EXE_NAME} PRIVATE GPR_WRITING=0 )
endif()

if(GPR_JPEG_WRITER_AVAILABLE)
  target_include_directories( ${EXE_NAME} PRIVATE "${GPR_LIB_DIR}/tiny_jpeg" )
  target_compile_definitions( ${EXE_NAME} PRIVATE GPR_JPEG_AVAILABLE=1 )
  target_link_libraries( ${EXE_NAME} tiny_jpeg )
else()
  target_compile_definitions( ${EXE_NAME} PRIVATE GPR_JPEG_AVAILABLE=0 )
endif()

# Linked again so they follow vc5_decoder/vc5_encoder on the link line (see
# the comment on the base link set above).
target_link_libraries( ${EXE_NAME} vc5_common common md5_lib expat_lib )

set_target_properties( ${EXE_NAME} PROPERTIES FOLDER test )

add_test( NAME gpr_conversion_tests COMMAND ${EXE_NAME} )
