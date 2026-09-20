# executable
set( EXE_NAME gpr_tools )

# get source and include files
file( GLOB GPRTOOLS_SRC_FILES "*.c" "*.cpp" )
file( GLOB GPRTOOLS_INC_FILES "*.h" "../common/*.h" )

# add include files from other folders
include_directories( "../common" )
include_directories( "../common/cJSON" )
include_directories( "../common/argument_parser" )
include_directories( "../common/TinyJPEG" )
include_directories( "../../lib/common/public" )
include_directories( "../../lib/vc5_common" )

# GPR_*_AVAILABLE variables come from the top-level CMakeLists.txt (feature
# option combined with the presence of the corresponding source directory)
if(GPR_READING_AVAILABLE)
  include_directories( "../../lib/vc5_decoder" )
  add_definitions("-DGPR_READING=1")
else()
  add_definitions("-DGPR_READING=0")
endif()

if(GPR_WRITING_AVAILABLE)
  include_directories( "../../lib/vc5_encoder" )
  add_definitions("-DGPR_WRITING=1")
else()
  add_definitions("-DGPR_WRITING=0")
endif()

include_directories( "../../lib/md5_lib" )
include_directories( "../../lib/dng_sdk" )
include_directories( "../../lib/gpr_sdk/public" )

if(GPR_JPEG_WRITER_AVAILABLE)
  include_directories( "../../lib/tiny_jpeg" )
  add_definitions("-DGPR_JPEG_AVAILABLE=1")
else()
  add_definitions("-DGPR_JPEG_AVAILABLE=0")
endif()

# add executable
add_executable( ${EXE_NAME} ${GPRTOOLS_SRC_FILES} ${GPRTOOLS_INC_FILES} )

# Linked libraries
target_link_libraries( ${EXE_NAME} gpr_sdk )

if(GPR_JPEG_WRITER_AVAILABLE)
  target_link_libraries( ${EXE_NAME} tiny_jpeg )
endif()

target_link_libraries( ${EXE_NAME} dng_sdk xmp_core )

if(GPR_READING_AVAILABLE)
  target_link_libraries( ${EXE_NAME} vc5_decoder )
endif()

if(GPR_WRITING_AVAILABLE)
  target_link_libraries( ${EXE_NAME} vc5_encoder )
endif()

target_link_libraries( ${EXE_NAME} vc5_common common md5_lib expat_lib cJSON argument_parser )

# In order to use Carbon API, define qEnableCarbon in gpr_platform.h and uncomment code below 
# if (APPLE)
#	target_link_libraries( ${EXE_NAME} "-framework Carbon" )
# endif (APPLE)

# set the folder where to place the projects
set_target_properties( ${EXE_NAME}  PROPERTIES FOLDER app )
