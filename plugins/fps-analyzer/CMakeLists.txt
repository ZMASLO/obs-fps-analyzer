add_library(fps-analyzer MODULE
    fps-analyzer-filter.cpp
    fps-analyzer-overlay.cpp
    resolution-detector.cpp
    kissfft/kiss_fft.c
    kissfft/kiss_fftr.c
)

target_link_libraries(fps-analyzer PRIVATE OBS::libobs)
target_compile_features(fps-analyzer PRIVATE cxx_std_20)
set_target_properties(fps-analyzer PROPERTIES PREFIX "")

# Offline test tools for the resolution detector (no OBS dependency):
#  - resdet-selftest: synthetic regression suite
#  - resdet-cli: replays frame dumps from the filter's "Debug options"
option(FPS_ANALYZER_BUILD_TOOLS "Build the resdet test tools" ON)
if(FPS_ANALYZER_BUILD_TOOLS)
    set(RESDET_CORE resolution-detector.cpp kissfft/kiss_fft.c kissfft/kiss_fftr.c)
    foreach(tool resdet-selftest resdet-cli)
        add_executable(${tool} tools/${tool}.cpp ${RESDET_CORE})
        target_include_directories(${tool} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_features(${tool} PRIVATE cxx_std_20)
        target_compile_definitions(${tool} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endforeach()
endif()
