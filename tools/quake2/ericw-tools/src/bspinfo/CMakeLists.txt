add_executable(vmt-bspinfo main.cc)
target_link_libraries(vmt-bspinfo common fmt::fmt)

# HACK: copy .dll dependencies
add_custom_command(TARGET vmt-bspinfo POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:TBB::tbb>" "$<TARGET_FILE_DIR:vmt-bspinfo>"
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:TBB::tbbmalloc>" "$<TARGET_FILE_DIR:vmt-bspinfo>"
                   )
copy_mingw_dlls(vmt-bspinfo)
add_loader_path_to_rpath(vmt-bspinfo)

install(TARGETS vmt-bspinfo RUNTIME DESTINATION .)
