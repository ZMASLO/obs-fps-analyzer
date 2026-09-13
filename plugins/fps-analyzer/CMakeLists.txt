add_library(fps-analyzer MODULE
    fps-analyzer-filter.cpp
    fps-core.cpp
    fps-analyzer-overlay.cpp
    resolution-detector.cpp
    kissfft/kiss_fft.c
    kissfft/kiss_fftr.c
)

target_link_libraries(fps-analyzer PRIVATE OBS::libobs)
target_compile_features(fps-analyzer PRIVATE cxx_std_20)
set_target_properties(fps-analyzer PROPERTIES PREFIX "")

# Offline test tools (no OBS dependency; they must not include libobs headers
# because the CI SDK is headers-only):
#  - resdet-selftest / resdet-cli: resolution detector (synthetic suite, dump replay)
#  - fps-selftest: characterization tests of the FPS-analysis core against the
#    v0.5.0 reference copy (tools/fps-ref-v050.cpp) with goldens in tests/golden
option(FPS_ANALYZER_BUILD_TOOLS "Build the offline test tools" ON)
if(FPS_ANALYZER_BUILD_TOOLS)
    set(RESDET_CORE resolution-detector.cpp kissfft/kiss_fft.c kissfft/kiss_fftr.c)
    set(FPS_CORE fps-core.cpp)
    set(FPS_REF tools/fps-ref-v050.cpp)
    add_executable(resdet-selftest tools/resdet-selftest.cpp ${RESDET_CORE})
    add_executable(resdet-cli      tools/resdet-cli.cpp      ${RESDET_CORE})
    add_executable(fps-selftest    tools/fps-selftest.cpp    ${FPS_CORE} ${FPS_REF})
    target_compile_definitions(fps-selftest PRIVATE FPS_SELFTEST_HAVE_CORE)
    foreach(tool resdet-selftest resdet-cli fps-selftest)
        target_include_directories(${tool} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_features(${tool} PRIVATE cxx_std_20)
        target_compile_definitions(${tool} PRIVATE _CRT_SECURE_NO_WARNINGS
            FPS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
        # The goldens are byte-exact, so pin the float model rather than relying
        # on the compiler default staying put across toolchain versions.
        if(MSVC)
            target_compile_options(${tool} PRIVATE /fp:precise)
        endif()
    endforeach()

    # `ctest -C Release` runs these; run-tests.ps1 and the CI workflow call it.
    # resdet-selftest writes spectrum_*.bmp next to itself, so keep its working
    # directory inside the build tree.
    add_test(NAME fps-selftest COMMAND fps-selftest)
    add_test(NAME resdet-selftest COMMAND resdet-selftest
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
