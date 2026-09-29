rr_add_test_suite(net
    SOURCES
        http_test.cpp
        sse_test.cpp
        retry_test.cpp
        client_server_test.cpp
    DEPS
        rr::net)
