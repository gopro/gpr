# library
set( LIB_NAME vc5_decoder )

# get source files
set(SRC_FILES
  codebooks.c component.c decoder.c dequantize.c fast_decode.c fast_gpr.c
  fused_decode.c fused_stream_decode.c inverse.c parameters.c raw.c
  syntax.c vc5_decoder.c vlc.c wavelet.c)

# get include files
file( GLOB INC_FILES "*.h" )

# add include files from other folders
include_directories( "../vc5_common" )
include_directories( "../common/private" )
include_directories( "../common/public" )
include_directories( "../vc5_encoder" )

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

if(NOT WIN32)
  find_package( Threads REQUIRED )
  target_link_libraries( ${LIB_NAME} Threads::Threads )
endif()

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
