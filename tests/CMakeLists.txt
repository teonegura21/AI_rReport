# Test tree.
#   testkit/     helpers shared by every suite (temp directories, …)
#   fake_engine/ a scriptable stand-in for llama-server
#   fakes/       in-process stand-ins for outside services (RunPod's API)
#   oracle/      a model that answers every mission right, and the faults
#                real models make, on demand (hub only)
#   unit/<m>/    one GoogleTest executable per module
#   fuzz/        fuzz targets: their corpora replayed by CTest; libFuzzer
#                binaries with RR_FUZZ (hub only)
#   component/   the real executables, driven from the outside
#   live/        the runtime against a real llama-server (L4, nightly); skipped
#                without one
add_subdirectory(testkit)
add_subdirectory(fakes)
if(RR_BUILD_HUB)
    add_subdirectory(oracle)
endif()
if(RR_BUILD_RUNTIME)
    add_subdirectory(fake_engine)  # before unit/: the supervisor tests use it
endif()
add_subdirectory(unit)
if(RR_BUILD_HUB)
    add_subdirectory(fuzz)
endif()
add_subdirectory(component)
add_subdirectory(live)
