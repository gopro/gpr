set( EXE_NAME bench_fused )

include_directories( "../../lib/vc5_encoder" )

add_executable( ${EXE_NAME} bench_fused.c )
if(WIN32)
  target_link_libraries( ${EXE_NAME} vc5_encoder vc5_common common )
else()
  target_link_libraries( ${EXE_NAME} vc5_encoder vc5_common common m )
endif()
set_target_properties( ${EXE_NAME} PROPERTIES FOLDER app )
