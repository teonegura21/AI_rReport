# rr::testkit — helpers shared by the test suites (never linked into products).
# Header-only; rr_add_test_suite() links it into every suite automatically.
#   rr/testkit/temp_dir.hpp   private scratch directories (mkdtemp)
#   rr/testkit/result.hpp     unwrap(), EXPECT_OK/ASSERT_OK, EXPECT_ERRC
rr_add_module(testkit
    PUBLIC_DEPS
        rr::core
        GTest::gtest)
