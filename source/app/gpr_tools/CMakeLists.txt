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

if(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_decoder")
  include_directories( "../../lib/vc5_decoder" )
  add_definitions("-DGPR_READING=1")
else(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_decoder")
  add_definitions("-DGPR_READING=0")
endif(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_decoder")

if(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_encoder")
  include_directories( "../../lib/vc5_encoder" )
  add_definitions("-DGPR_WRITING=1")
else(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_encoder")
  add_definitions("-DGPR_WRITING=0")
endif(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_encoder")

include_directories( "../../lib/md5_lib" )
include_directories( "../../lib/dng_sdk" )
include_directories( "../../lib/gpr_sdk/public" )

if(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/tiny_jpeg")
  include_directories( "../../lib/tiny_jpeg" )
  add_definitions("-DGPR_JPEG_AVAILABLE=1")  
else(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/tiny_jpeg")
  add_definitions("-DGPR_JPEG_AVAILABLE=0")
endif(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/tiny_jpeg")

# add executable
add_executable( ${EXE_NAME} ${GPRTOOLS_SRC_FILES} ${GPRTOOLS_INC_FILES} )

install(TARGETS gpr_tools
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib/static)

# Linked libraries
target_link_libraries( ${EXE_NAME} gpr_sdk )

if(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/tiny_jpeg")
  target_link_libraries( ${EXE_NAME} tiny_jpeg )
endif(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/tiny_jpeg")

target_link_libraries( ${EXE_NAME} dng_sdk xmp_core )

if(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_decoder")
  target_link_libraries( ${EXE_NAME} vc5_decoder )
endif(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_decoder")

if(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_encoder")
  target_link_libraries( ${EXE_NAME} vc5_encoder )
endif(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/vc5_encoder")

target_link_libraries( ${EXE_NAME} vc5_common common md5_lib expat_lib cJSON argument_parser )

# In order to use Carbon API, define qEnableCarbon in gpr_platform.h and uncomment code below 
# if (APPLE)
#	target_link_libraries( ${EXE_NAME} "-framework Carbon" )
# endif (APPLE)

# set the folder where to place the projects
set_target_properties( ${EXE_NAME}  PROPERTIES FOLDER app )
