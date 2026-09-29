# Test tree.
#   testkit/     helpers shared by every suite (temp directories, …)
#   unit/<m>/    one GoogleTest executable per module
add_subdirectory(testkit)
add_subdirectory(unit)
