# library
set( LIB_NAME xmp_core )

# get source files
file( GLOB BASE_SRC_FILES "*.cpp" )

# get include files
file( GLOB BASE_INC_FILES "*.h" "public/include/*.h" "public/include/*.hpp" "public/include/client-glue/*.hpp" )

# get all source files
set( SRC_FILES ${BASE_SRC_FILES} )

# get all include files
set( INC_FILES ${BASE_INC_FILES} )

# add include files from other folders
include_directories( "../md5_lib" )
include_directories( "../expat_lib" )
include_directories( "public/include" )

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

# define compile time definitions
target_compile_definitions( ${LIB_NAME} PUBLIC XML_STATIC=1 )

SET(CMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libstdc++")

target_link_libraries( ${LIB_NAME} )

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
