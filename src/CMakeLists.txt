# Integrator-owned: application core library + the app bundle.
file(GLOB FOLIA_CORE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/storage/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/editor/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/shell/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/import/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp)
list(FILTER FOLIA_CORE_SOURCES EXCLUDE REGEX ".*/app/main\\.cpp$")
file(GLOB FOLIA_CORE_HEADERS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/core/*.h
     ${CMAKE_CURRENT_SOURCE_DIR}/storage/*.h
     ${CMAKE_CURRENT_SOURCE_DIR}/editor/*.h
     ${CMAKE_CURRENT_SOURCE_DIR}/shell/*.h
     ${CMAKE_CURRENT_SOURCE_DIR}/import/*.h
     ${CMAKE_CURRENT_SOURCE_DIR}/app/*.h)
set(FOLIA_PLATFORM_SOURCES)
if(APPLE)
  file(GLOB FOLIA_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/mac/*.mm)
endif()

add_library(folia_app_core STATIC ${FOLIA_CORE_SOURCES} ${FOLIA_CORE_HEADERS} ${FOLIA_PLATFORM_SOURCES})
target_include_directories(folia_app_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(folia_app_core PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets folia_lua folia_sqlite folia_code)
if(FOLIA_WITH_MATH)
  target_link_libraries(folia_app_core PUBLIC folia_math)
  target_compile_definitions(folia_app_core PUBLIC FOLIA_WITH_MATH=1)
endif()
if(APPLE)
  target_link_libraries(folia_app_core PUBLIC "-framework AppKit" "-framework Foundation"
                        "-framework ImageIO" "-framework CoreGraphics" "-framework CoreText"
                        "-framework UniformTypeIdentifiers")
endif()
target_compile_definitions(folia_app_core PUBLIC
  FOLIA_VERSION="${PROJECT_VERSION}"
  FOLIA_PRESET_SOURCE_DIR="${CMAKE_SOURCE_DIR}/presets")

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cpp)
  add_executable(folia MACOSX_BUNDLE WIN32 app/main.cpp)
  target_link_libraries(folia PRIVATE folia_app_core)
  set_target_properties(folia PROPERTIES
    OUTPUT_NAME Folia
    MACOSX_BUNDLE_GUI_IDENTIFIER org.folia.notes
    MACOSX_BUNDLE_BUNDLE_NAME Folia
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  if(APPLE)
    # App icon; regenerate with `swift packaging/brand/make_brand.swift`.
    set(FOLIA_ICON ${CMAKE_SOURCE_DIR}/packaging/brand/Folia.icns)
    target_sources(folia PRIVATE ${FOLIA_ICON})
    set_source_files_properties(${FOLIA_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(folia PROPERTIES MACOSX_BUNDLE_ICON_FILE Folia.icns)
  elseif(WIN32)
    target_sources(folia PRIVATE ${CMAKE_SOURCE_DIR}/packaging/brand/folia.rc)
  endif()
  # Ship presets inside the bundle / next to the executable so first run can
  # copy them into the user's editable shell folder.
  if(APPLE)
    add_custom_command(TARGET folia POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/presets
              $<TARGET_BUNDLE_CONTENT_DIR:folia>/Resources/presets)
  else()
    add_custom_command(TARGET folia POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/presets
              $<TARGET_FILE_DIR:folia>/presets)
  endif()
endif()
