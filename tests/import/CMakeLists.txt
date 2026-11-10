# Document import: Markdown, text and Word conversion (docs/ATTACHMENTS.md §8).
add_executable(import_tests tst_import.cpp)
target_link_libraries(import_tests PRIVATE folia_app_core Qt6::Test)
add_test(NAME import_tests COMMAND import_tests)
set_tests_properties(import_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${FOLIA_TEST_QPA}")
