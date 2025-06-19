project(fps-analyzer)

add_library(fps-analyzer MODULE fps-analyzer.cpp)
target_link_libraries(fps-analyzer libobs)

add_custom_command(TARGET fps-analyzer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:fps-analyzer>
        ${CMAKE_BINARY_DIR}/rundir/$<CONFIG>/obs-plugins/64bit/
)