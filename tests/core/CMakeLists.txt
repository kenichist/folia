add_executable(core_tests test_core.cpp)
target_link_libraries(core_tests PRIVATE folia_app_core Qt6::Test)
add_test(NAME core_tests COMMAND core_tests)
set_tests_properties(core_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${FOLIA_TEST_QPA}")
