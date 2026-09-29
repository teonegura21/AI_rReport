# One GoogleTest executable per module; see rr_add_test_suite() in cmake/RrModule.cmake.
add_subdirectory(core)
add_subdirectory(log)
add_subdirectory(config)
add_subdirectory(json)
