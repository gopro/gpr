# library
set( LIB_NAME vc5_encoder )

# get source files
set(SRC_FILES
  codebooks.c component.c denoise.c encoder.c forward.c fused_encode.c
  parameters.c raw.c sections.c syntax.c vc5_encoder.c vlc.c)
# get include files
file( GLOB INC_FILES "*.h" )

# The assembly file preprocesses to an empty unit on non-ARM targets.
list(APPEND SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/fused_encode_arm64.S")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/fused_encode_arm64.S"
    PROPERTIES LANGUAGE C)

# add include files from other folders
include_directories( "../vc5_common" )
include_directories( "../common/private" )
include_directories( "../common/public" )

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

if(NOT WIN32)
  find_package( Threads REQUIRED )
  target_link_libraries( ${LIB_NAME} Threads::Threads m )
endif()

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
