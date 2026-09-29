# rr_config — layered TOML configuration (defaults < file < secrets < env < CLI)
# where the built-in defaults double as the schema.
rr_add_module(config
    SOURCES
        config.cpp
    PUBLIC_DEPS
        rr::core
    PRIVATE_DEPS
        tomlplusplus::tomlplusplus)
