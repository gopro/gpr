# library
set( LIB_NAME expat_lib )

# get source files
file( GLOB SRC_FILES "*.c" )

# get include files
file( GLOB INC_FILES "*.h" )

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

# define compile time definitions
target_compile_definitions( ${LIB_NAME} PUBLIC XML_STATIC=1 )

SET(CMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libstdc++")

target_link_libraries( ${LIB_NAME} )

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
