# Test tree.
#   testkit/     helpers shared by every suite (temp directories, …)
#   fake_engine/ a scriptable stand-in for llama-server
#   unit/<m>/    one GoogleTest executable per module
add_subdirectory(testkit)
if(RR_BUILD_RUNTIME)
    add_subdirectory(fake_engine)  # before unit/: the supervisor tests use it
endif()
add_subdirectory(unit)
