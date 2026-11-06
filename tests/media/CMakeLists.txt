# Attachments, media budget/scheduler, image decoding and the image block.
add_executable(media_tests tst_media.cpp)
target_link_libraries(media_tests PRIVATE folia_app_core Qt6::Test)
add_test(NAME media_tests COMMAND media_tests)
set_tests_properties(media_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
