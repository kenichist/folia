# Settings window tests (integrator-owned): a real AppController on a temp data folder.
add_executable(app_tests tst_settings.cpp)
target_link_libraries(app_tests PRIVATE folia_app_core Qt6::Test)
add_test(NAME app_tests COMMAND app_tests)
set_tests_properties(app_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
