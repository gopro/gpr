# library
set( LIB_NAME vc5_encoder )

# get source files
set(SRC_FILES
  codebooks.c component.c denoise.c encoder.c forward.c
  parameters.c raw.c sections.c syntax.c vc5_encoder.c vlc.c)
# get include files
file( GLOB INC_FILES "*.h" )

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
