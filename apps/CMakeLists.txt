# Executables. Apps are thin: they parse arguments and wire modules together;
# all logic lives in src/ where it is unit-tested.

if(RR_BUILD_RUNTIME)
    add_subdirectory(rreport-runtime)
endif()
