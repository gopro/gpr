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
target_include_directories(${LIB_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/gpr>)
target_link_libraries(${LIB_NAME} PUBLIC vc5_common common)

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(${LIB_NAME} PUBLIC Threads::Threads)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$" AND NEON AND NOT WIN32)
  enable_language(ASM)
  target_sources(${LIB_NAME} PRIVATE stock_unpack_arm64.S)
  target_compile_definitions(${LIB_NAME} PRIVATE GPR_STOCK_UNPACK_ASM=1)
endif()

if(GPR_ENABLE_PARALLEL_CHANNELS AND NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(${LIB_NAME} PUBLIC Threads::Threads)
  target_compile_definitions(${LIB_NAME} PRIVATE GPR_ENABLE_PARALLEL_CHANNELS=1)
else()
  target_compile_definitions(${LIB_NAME} PRIVATE GPR_ENABLE_PARALLEL_CHANNELS=0)
endif()
if(GPR_ENABLE_PARALLEL_UNPACK AND NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(${LIB_NAME} PUBLIC Threads::Threads)
  target_compile_definitions(${LIB_NAME} PRIVATE GPR_ENABLE_PARALLEL_UNPACK=1)
else()
  target_compile_definitions(${LIB_NAME} PRIVATE GPR_ENABLE_PARALLEL_UNPACK=0)
endif()

# set the folder where to place the projects
set_target_properties( ${LIB_NAME} PROPERTIES FOLDER lib )
