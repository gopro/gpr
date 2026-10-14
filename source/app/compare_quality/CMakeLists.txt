set( EXE_NAME compare_quality )

add_executable( ${EXE_NAME} compare_quality.c )
if(NOT WIN32)
  target_link_libraries( ${EXE_NAME} m )
endif()
set_target_properties( ${EXE_NAME} PROPERTIES FOLDER app )
