# Owned by the math-and-code owner (math half). Defines folia_math: the native
# TeX-subset renderer (no browser engine, no external process). Links only
# Qt6::Core and Qt6::Gui. See docs/MATH_SUBSET.md.
file(GLOB FOLIA_MATH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
add_library(folia_math STATIC ${FOLIA_MATH_SOURCES})
target_include_directories(folia_math PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(folia_math PUBLIC Qt6::Core Qt6::Gui)
if(NOT MSVC)
  target_compile_options(folia_math PRIVATE -Wall -Wextra)
endif()
