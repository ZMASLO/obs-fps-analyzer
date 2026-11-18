add_library(fps-analyzer MODULE
    fps-analyzer-filter.cpp
    fps-core.cpp
    plot-layout.cpp
    fps-analyzer-overlay.cpp
    resolution-detector.cpp
    source-rate.cpp
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
    set(FPS_CORE fps-core.cpp plot-layout.cpp)
    set(FPS_REF tools/fps-ref-v050.cpp)
    add_executable(resdet-selftest tools/resdet-selftest.cpp ${RESDET_CORE})
    add_executable(resdet-cli      tools/resdet-cli.cpp      ${RESDET_CORE})
    add_executable(fps-selftest    tools/fps-selftest.cpp    ${FPS_CORE} ${FPS_REF})
    add_executable(fps-cli         tools/fps-cli.cpp         ${FPS_CORE})
    target_compile_definitions(fps-selftest PRIVATE FPS_SELFTEST_HAVE_CORE)
    foreach(tool resdet-selftest resdet-cli fps-selftest fps-cli)
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

    # Source refresh rate estimator suite
    add_executable(srate-selftest tools/srate-selftest.cpp source-rate.cpp)
    target_include_directories(srate-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(srate-selftest PRIVATE cxx_std_20)
    target_compile_definitions(srate-selftest PRIVATE _CRT_SECURE_NO_WARNINGS)
    find_package(Threads REQUIRED)
    target_link_libraries(srate-selftest PRIVATE Threads::Threads)

    # Performance: per-frame and per-render cost of the OBS-free code (fps-bench)
    add_executable(fps-bench tools/fps-bench.cpp ${FPS_CORE} source-rate.cpp)
    target_include_directories(fps-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(fps-bench PRIVATE cxx_std_20)
    target_compile_definitions(fps-bench PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(fps-bench PRIVATE Threads::Threads)

    # `ctest -C Release` runs these; run-tests.ps1 and the CI workflow call it.
    # resdet-selftest writes spectrum_*.bmp next to itself, so keep its working
    # directory inside the build tree.
    add_test(NAME fps-selftest COMMAND fps-selftest)
    add_test(NAME resdet-selftest COMMAND resdet-selftest
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    # Replays the recorded traces listed in tests/cases.txt. Recordings that are
    # not present are reported as skipped, so this passes before any are added.
    add_test(NAME fps-fixtures COMMAND fps-cli --manifest
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases.txt)
    add_test(NAME srate-selftest COMMAND srate-selftest)
    # Generous per-call budgets: catches only a gross slowdown, on any machine
    # (run-perf.ps1 compares with a local baseline for the fine-grained check)
    add_test(NAME fps-perf-budget COMMAND fps-bench --check)
endif()
