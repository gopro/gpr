# library
set( LIB_NAME dng_sdk )

# get source files
file( GLOB SRC_FILES "*.cpp" )

# get include files
file( GLOB INC_FILES "*.h" )

# add include files from other folders
include_directories( "../common/private" )
include_directories( "../common/public" )
include_directories( "../xmp_core/public/include" )

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

target_link_libraries( ${LIB_NAME} )

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
