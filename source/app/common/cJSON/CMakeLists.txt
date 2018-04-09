# library
set( LIB_NAME cJSON )

# get source files
file( GLOB SRC_FILES "*.c" "*.cpp" )

# get include files
file( GLOB INC_FILES "*.h" )

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

target_link_libraries( ${LIB_NAME} )

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
