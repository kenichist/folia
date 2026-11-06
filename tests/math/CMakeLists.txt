# Owned by the math renderer owner.
#   math_tests   - unit/geometry/error/pathological/concurrency/perf tests (ctest)
#   math_gallery - renders the corpus + inline paragraphs to a PNG for review
find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(math_tests tst_math.cpp MathCorpus.h)
target_link_libraries(math_tests PRIVATE folia_math Qt6::Test)
set_target_properties(math_tests PROPERTIES AUTOMOC ON)
add_test(NAME math_tests COMMAND math_tests)
# Fonts need the platform font database; "offscreen" keeps CI headless where
# the default platform is unavailable (macOS uses CoreText either way).
if(NOT APPLE)
  set_tests_properties(math_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${FOLIA_TEST_QPA}")
endif()

# First calls of a process race on 8 threads (one-time font init + cold cache).
add_executable(math_init_race tst_init_race.cpp MathCorpus.h)
target_link_libraries(math_init_race PRIVATE folia_math)
add_test(NAME math_init_race COMMAND math_init_race)
if(NOT APPLE)
  set_tests_properties(math_init_race PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${FOLIA_TEST_QPA}")
endif()

add_executable(math_gallery math_gallery.cpp MathCorpus.h)
target_link_libraries(math_gallery PRIVATE folia_math)
target_compile_definitions(math_gallery PRIVATE
  FOLIA_MATH_GALLERY_DEFAULT="${CMAKE_BINARY_DIR}/math-gallery.png")
