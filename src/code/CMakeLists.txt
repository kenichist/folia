# Owned by the math-and-code owner (code half). Defines folia_code.
file(GLOB FOLIA_CODE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
add_library(folia_code STATIC ${FOLIA_CODE_SOURCES})
target_include_directories(folia_code PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(folia_code PUBLIC Qt6::Core)
