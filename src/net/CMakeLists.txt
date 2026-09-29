# rr_net — HTTP for rReport: a libcurl client (streaming, instant cancellation,
# error mapping), SSE parsing, retry with backoff, and a server wrapper over
# cpp-httplib. Shared by the hub and the runtime.
rr_add_module(net
    SOURCES
        http.cpp
        http_client.cpp
        http_server.cpp
        retry.cpp
        sse.cpp
    PUBLIC_DEPS
        rr::core
    PRIVATE_DEPS
        rr::json
        CURL::libcurl
        httplib::httplib)

# A client that disconnects mid-stream (the hub cancelling a generation) must
# produce a failed write, not a SIGPIPE that kills the server. cpp-httplib
# sends with flags 0 unless told otherwise. (Its Server constructor also sets
# SIGPIPE to SIG_IGN process-wide; we do not rely on that side effect, and
# rr_proc resets signal dispositions for children because of it.) Defined for
# every translation unit of rr_net, so the header-only library is compiled one
# way only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(rr_net PRIVATE CPPHTTPLIB_SEND_FLAGS=MSG_NOSIGNAL)
endif()
