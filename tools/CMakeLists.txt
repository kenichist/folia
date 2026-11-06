# Integrator-owned measurement and fixture tools.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/fixturegen.cpp)
  add_executable(folia_fixturegen fixturegen.cpp)
  target_link_libraries(folia_fixturegen PRIVATE folia_app_core)
endif()
if(APPLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/procstat.c)
  add_executable(folia_procstat procstat.c)
endif()
add_executable(folia_baseline MACOSX_BUNDLE baseline.cpp)
target_link_libraries(folia_baseline PRIVATE folia_app_core)
# Decode-cost probe for the image benchmarks (docs/BENCHMARKS.md).
add_executable(folia_mediaprobe mediaprobe.cpp)
target_link_libraries(folia_mediaprobe PRIVATE folia_app_core)
