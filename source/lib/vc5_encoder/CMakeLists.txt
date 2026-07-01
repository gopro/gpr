# library
set( LIB_NAME vc5_encoder )

# get source files
file( GLOB SRC_FILES "*.c" )

# get include files
file( GLOB INC_FILES "*.h" )

# add include files from other folders
include_directories( "../vc5_common" )
include_directories( "../common/private" )
include_directories( "../common/public" )

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

# Enable the NEON SIMD encoder kernels on ARM targets (Apple Silicon / aarch64).
# Everywhere else NEON defaults to 0 (see gpr_platform.h) and the scalar path is used.
# Can be forced off with -DGPR_ENABLE_NEON=OFF (e.g. to compare against the scalar path).
option( GPR_ENABLE_NEON "Enable NEON SIMD encoder kernels on ARM" ON )
if( GPR_ENABLE_NEON AND ( CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64" ) )
  message( STATUS "${LIB_NAME}: enabling NEON SIMD path (NEON=1)" )
  target_compile_definitions( ${LIB_NAME} PRIVATE NEON=1 )
endif()

target_link_libraries( ${LIB_NAME} )

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
