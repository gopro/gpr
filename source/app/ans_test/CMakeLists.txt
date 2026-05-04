set( EXE_NAME ans_test )

include_directories( "../../lib/vc5_common" )

add_executable( ${EXE_NAME} ans_test.c )
if(WIN32)
  target_link_libraries( ${EXE_NAME} vc5_common )
else()
  target_link_libraries( ${EXE_NAME} vc5_common m )
endif()
set_target_properties( ${EXE_NAME} PROPERTIES FOLDER app )
