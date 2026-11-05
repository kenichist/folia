# Independent robustness suite for the programmable Lua shell (performance-and-QA owner).
add_executable(shell_tests tst_shell.cpp)
target_link_libraries(shell_tests PRIVATE folia_app_core Qt6::Test)
add_test(NAME shell_tests COMMAND shell_tests)
# Some cases re-run this binary in a child process (hang/crash probes) with a
# few seconds of timeout each, so allow more than the ctest default per test.
set_tests_properties(shell_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 300)
