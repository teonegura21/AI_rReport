# C++ modules. Order matters only for readability: dependencies come first.
# Each subdirectory declares one module with rr_add_module() (cmake/RrModule.cmake).

# --- Shared by the hub and the runtime ---------------------------------------
add_subdirectory(core)
add_subdirectory(log)
add_subdirectory(config)
add_subdirectory(json)
add_subdirectory(blob)
add_subdirectory(proc)
add_subdirectory(net)
add_subdirectory(runtime_api)
add_subdirectory(runpod)       # RunPod's API: the hub starts and stops the pod, the pod stops itself

# --- Runtime only (the GPU pod) -------------------------------------------------
if(RR_BUILD_RUNTIME)
    add_subdirectory(supervisor)
endif()

# --- Hub only ------------------------------------------------------------------
# The runtime keeps no durable state of its own (the hub is the source of
# truth, ADR-0001), so it never links SQLite.
if(RR_BUILD_HUB)
    add_subdirectory(text)      # text made comparable: tokens, sentences, numbers, names, alignment
    add_subdirectory(db)
    add_subdirectory(llm)       # calling models
    add_subdirectory(prompt)    # the prompts: templates, versions, rendering, token estimates
    add_subdirectory(agents)    # the army: missions on the engines, escalation, ceilings
    add_subdirectory(reason)    # many answers, one decision: votes, waves, judgements; the tiers
    add_subdirectory(library)   # a library's database: schema and repositories
    add_subdirectory(runtime_client)  # the pod, seen from the hub
    add_subdirectory(hub)             # the hub's configuration, doctor and bench
endif()
