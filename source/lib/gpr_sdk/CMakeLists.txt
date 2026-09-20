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

# GPR_*_AVAILABLE variables come from the top-level CMakeLists.txt (feature
# option combined with the presence of the corresponding source directory)
if(GPR_READING_AVAILABLE)
  include_directories( "../vc5_decoder" )
  add_definitions("-DGPR_READING=1")
else()
  add_definitions("-DGPR_READING=0")
endif()

if(GPR_WRITING_AVAILABLE)
  include_directories( "../vc5_encoder" )
  add_definitions("-DGPR_WRITING=1")
else()
  add_definitions("-DGPR_WRITING=0")
endif()

if(GPR_JPEG_WRITER_AVAILABLE)
  include_directories( "../tiny_jpeg" )
  add_definitions("-DGPR_JPEG_AVAILABLE=1")
else()
  add_definitions("-DGPR_JPEG_AVAILABLE=0")
endif()

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

target_link_libraries( ${LIB_NAME} )

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
