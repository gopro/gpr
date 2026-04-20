set( EXE_NAME compare_quality )

add_executable( ${EXE_NAME} compare_quality.c )
target_link_libraries( ${EXE_NAME} m )
set_target_properties( ${EXE_NAME} PROPERTIES FOLDER app )
