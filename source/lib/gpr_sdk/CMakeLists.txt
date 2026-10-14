# library
set( LIB_NAME gpr_sdk )

# get source files
file( GLOB SRC_FILES "private/*.cpp" "public/*.cpp" )

# get include files
file( GLOB INC_FILES "private/*.h" "public/*.h" )

# add include files from other folders
include_directories( "./public" )
include_directories( "../common/private" )
include_directories( "../common/public" )
include_directories( "../dng_sdk" )
include_directories( "../vc5_common" )

include_directories( "../vc5_decoder" "../vc5_encoder" "../tiny_jpeg" )

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

target_compile_definitions(${LIB_NAME} PUBLIC
  GPR_READING=1 GPR_WRITING=1 GPR_JPEG_AVAILABLE=1)
target_link_libraries(${LIB_NAME} PUBLIC
  dng_sdk xmp_core expat_lib md5_lib tiny_jpeg
  vc5_decoder vc5_encoder vc5_common common)

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
