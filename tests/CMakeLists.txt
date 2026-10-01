# Test tree.
#   testkit/     helpers shared by every suite (temp directories, …)
#   fake_engine/ a scriptable stand-in for llama-server
#   fakes/       in-process stand-ins for outside services (RunPod's API)
#   unit/<m>/    one GoogleTest executable per module
#   component/   the real executables, driven from the outside
#   live/        the runtime against a real llama-server (L4, nightly); skipped
#                without one
add_subdirectory(testkit)
add_subdirectory(fakes)
if(RR_BUILD_RUNTIME)
    add_subdirectory(fake_engine)  # before unit/: the supervisor tests use it
endif()
add_subdirectory(unit)
add_subdirectory(component)
add_subdirectory(live)
