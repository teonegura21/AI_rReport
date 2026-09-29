# One GoogleTest executable per module; see rr_add_test_suite() in cmake/RrModule.cmake.
# The conditions mirror src/CMakeLists.txt: a suite exists only if its module does.
add_subdirectory(core)
add_subdirectory(log)
add_subdirectory(config)
add_subdirectory(json)
add_subdirectory(blob)
add_subdirectory(proc)
add_subdirectory(net)

if(RR_BUILD_HUB)
    add_subdirectory(db)
endif()
