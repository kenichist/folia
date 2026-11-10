# Settings window tests (integrator-owned): a real AppController on a temp data folder.
add_executable(app_tests tst_settings.cpp)
target_link_libraries(app_tests PRIVATE folia_app_core Qt6::Test)
add_test(NAME app_tests COMMAND app_tests)
set_tests_properties(app_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${FOLIA_TEST_QPA}")
# Startup read-ahead and the idle window's buffers.
add_executable(launch_tests tst_launch.cpp)
target_link_libraries(launch_tests PRIVATE folia_app_core Qt6::Test)
add_test(NAME launch_tests COMMAND launch_tests)
set_tests_properties(launch_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${FOLIA_TEST_QPA}")
