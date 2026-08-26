set(BSPUTIL_SOURCES
	bsputil.cc
	../include/bsputil/bsputil.hh
)

add_library(libbsputil STATIC ${BSPUTIL_SOURCES})
target_link_libraries(libbsputil common TBB::tbb TBB::tbbmalloc fmt::fmt)

add_executable(vmt-bsputil main.cc)
target_link_libraries(vmt-bsputil libbsputil)

# HACK: copy .dll dependencies
add_custom_command(TARGET vmt-bsputil POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:TBB::tbb>" "$<TARGET_FILE_DIR:vmt-bsputil>"
				   COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:TBB::tbbmalloc>" "$<TARGET_FILE_DIR:vmt-bsputil>"
				   )
copy_mingw_dlls(vmt-bsputil)
add_loader_path_to_rpath(vmt-bsputil)

install(TARGETS vmt-bsputil RUNTIME DESTINATION .)
