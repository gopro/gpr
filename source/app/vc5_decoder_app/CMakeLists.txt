# executable
set( EXE_NAME vc5_decoder_app )

# get source and include files
file( GLOB DECODER_SRC_FILES "*.c" "*.cpp" )
file( GLOB DECODER_INC_FILES "*.h" "../common/*.h" )

# add include files from other folders
include_directories( "../common" )
include_directories( "../common/argument_parser" )
include_directories( "../../lib/common/private" )
include_directories( "../../lib/common/public" )
include_directories( "../../lib/vc5_common" )
include_directories( "../../lib/vc5_decoder" )
include_directories( "../../lib/md5_lib" )

add_definitions("-DGPR_READING=1")

# add executable
add_executable( ${EXE_NAME} ${DECODER_SRC_FILES} ${DECODER_INC_FILES} ${COMMON_SRC_FILES} ${COMMON_INC_FILES} )

target_link_libraries( ${EXE_NAME} vc5_decoder vc5_common common md5_lib argument_parser )

# set the folder where to place the projects
set_target_properties( ${EXE_NAME}  PROPERTIES FOLDER app )
