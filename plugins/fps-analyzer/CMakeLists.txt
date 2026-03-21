add_library(fps-analyzer MODULE
    fps-analyzer-filter.cpp
    fps-analyzer-overlay.cpp
)

target_link_libraries(fps-analyzer PRIVATE OBS::libobs)
set_target_properties(fps-analyzer PROPERTIES PREFIX "")
