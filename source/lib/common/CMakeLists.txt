# library
set( LIB_NAME common )

# get source files
file( GLOB SRC_FILES "private/*.cpp" "private/*.c" )

# get include files
file( GLOB INC_FILES "private/*.h" "public/*.h" )

include_directories( "./public" )

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

target_link_libraries( ${LIB_NAME} )

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
