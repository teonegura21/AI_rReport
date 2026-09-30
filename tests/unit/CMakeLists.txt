# One GoogleTest executable per module; see rr_add_test_suite() in cmake/RrModule.cmake.
# The conditions mirror src/CMakeLists.txt: a suite exists only if its module does.
add_subdirectory(core)
add_subdirectory(log)
add_subdirectory(config)
add_subdirectory(json)
add_subdirectory(blob)
add_subdirectory(proc)
add_subdirectory(net)
add_subdirectory(runtime_api)
add_subdirectory(runpod)

if(RR_BUILD_RUNTIME)
    add_subdirectory(supervisor)
endif()

if(RR_BUILD_HUB)
    add_subdirectory(db)
    add_subdirectory(llm)
    add_subdirectory(library)
endif()

# The runtime client is tested against the real runtime: it needs both halves.
if(RR_BUILD_HUB AND RR_BUILD_RUNTIME)
    add_subdirectory(runtime_client)
endif()
