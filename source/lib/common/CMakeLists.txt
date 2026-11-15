# library
set( LIB_NAME common )

# get source files
file( GLOB SRC_FILES "private/*.cpp" "private/*.c" )

# get include files
file( GLOB INC_FILES "private/*.h" "public/*.h" )

include_directories( "./public" )

# library
add_library( ${LIB_NAME} STATIC ${SRC_FILES} ${INC_FILES} )

target_include_directories(${LIB_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/public>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/gpr>)
target_compile_definitions(${LIB_NAME} PUBLIC
  GPR_READING=$<BOOL:${GPR_BUILD_DECODER}>
  GPR_WRITING=$<BOOL:${GPR_BUILD_ENCODER}>
  GPR_JPEG_AVAILABLE=$<BOOL:${GPR_BUILD_SDK}>)

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
