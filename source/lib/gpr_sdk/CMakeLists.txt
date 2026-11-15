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

if(GPR_BUILD_DECODER)
  include_directories( "../vc5_decoder" )
endif()

if(GPR_BUILD_ENCODER)
  include_directories( "../vc5_encoder" )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/tiny_jpeg")
  include_directories( "../tiny_jpeg" )
  add_definitions("-DGPR_JPEG_AVAILABLE=1")
else(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/tiny_jpeg")
  add_definitions("-DGPR_JPEG_AVAILABLE=0")
endif(EXISTS "${CMAKE_SOURCE_DIR}/source/lib/tiny_jpeg")

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

target_include_directories(${LIB_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/public>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/gpr>)
target_compile_definitions(${LIB_NAME} PUBLIC
  GPR_READING=$<BOOL:${GPR_BUILD_DECODER}> GPR_WRITING=$<BOOL:${GPR_BUILD_ENCODER}> GPR_JPEG_AVAILABLE=1)
target_link_libraries(${LIB_NAME} PUBLIC common vc5_common
  PRIVATE dng_sdk xmp_core md5_lib expat_lib tiny_jpeg)
if(TARGET vc5_encoder)
  target_link_libraries(${LIB_NAME} PRIVATE vc5_encoder)
endif()
if(TARGET vc5_decoder)
  target_link_libraries(${LIB_NAME} PRIVATE vc5_decoder)
endif()

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
