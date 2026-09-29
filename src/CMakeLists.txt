# C++ modules. Order matters only for readability: dependencies come first.
# Each subdirectory declares one module with rr_add_module() (cmake/RrModule.cmake).

# --- Shared by the hub and the runtime ---------------------------------------
add_subdirectory(core)
add_subdirectory(log)
add_subdirectory(config)
add_subdirectory(json)
add_subdirectory(blob)

# --- Hub only ------------------------------------------------------------------
# The runtime keeps no durable state of its own (the hub is the source of
# truth, ADR-0001), so it never links SQLite.
if(RR_BUILD_HUB)
    add_subdirectory(db)
endif()
