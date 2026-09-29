# C++ modules. Order matters only for readability: dependencies come first.
# Each subdirectory declares one module with rr_add_module() (cmake/RrModule.cmake).

# --- Shared by the hub and the runtime ---------------------------------------
add_subdirectory(core)
add_subdirectory(log)
add_subdirectory(config)
add_subdirectory(json)
