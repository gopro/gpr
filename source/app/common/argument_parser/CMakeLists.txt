# library
set( LIB_NAME argument_parser )

# for gpr_platform.h (GPR_NEON, reported in the version banner)
include_directories( "../../../lib/common/public" )

# get source files
file( GLOB SRC_FILES "*.c" "*.cpp" )

# get include files
file( GLOB INC_FILES "*.h" )

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

target_link_libraries( ${LIB_NAME} )

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
