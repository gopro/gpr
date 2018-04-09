# executable
set( EXE_NAME vc5_encoder_app )

# get source and include files
file( GLOB ENCODER_SRC_FILES "*.c" "*.cpp" )
file( GLOB ENCODER_INC_FILES "*.h" "../common/*.h" )

# add include files from other folders
include_directories( "../common" )
include_directories( "../common/argument_parser" )
include_directories( "../../lib/common/private" )
include_directories( "../../lib/common/public" )
include_directories( "../../lib/vc5_common" )
include_directories( "../../lib/vc5_encoder" )
include_directories( "../../lib/md5_lib" )

add_definitions("-DGPR_WRITING=1")

# add executable
add_executable( ${EXE_NAME} ${ENCODER_SRC_FILES} ${ENCODER_INC_FILES} ${COMMON_SRC_FILES} ${COMMON_INC_FILES} )

target_link_libraries( ${EXE_NAME} vc5_encoder vc5_common common md5_lib argument_parser )

# set the folder where to place the projects
set_target_properties( ${EXE_NAME}  PROPERTIES FOLDER app )
