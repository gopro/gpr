# library
set( LIB_NAME vc5_common )

# get source files
set(SRC_FILES
  ans_joint.c bitstream.c codec.c companding.c image.c logcurve.c
  noise_model.c stream.c syntax.c utilities.c wavelet.c)

# get include files
file( GLOB INC_FILES "*.h" )

# add include files from other folders
include_directories( "../common/private" )
include_directories( "../common/public" )

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

if(NOT WIN32)
  target_link_libraries( ${LIB_NAME} m )
endif()

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
