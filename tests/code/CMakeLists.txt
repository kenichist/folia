# Owned by the code-highlighting owner. Defines code_tests (Qt Test, GUI-less).
add_executable(code_tests code_tests.cpp)
set_target_properties(code_tests PROPERTIES AUTOMOC ON)
target_link_libraries(code_tests PRIVATE folia_code Qt6::Test)
add_test(NAME code_tests COMMAND code_tests)
