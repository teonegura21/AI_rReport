# rr::fakes — stand-ins for services the tests cannot use for real (header-only).
#   rr/fakes/fake_runpod.hpp   RunPod's pod API: REST v2 and GraphQL's podStop
rr_add_module(fakes
    PUBLIC_DEPS
        rr::json
        rr::net)
