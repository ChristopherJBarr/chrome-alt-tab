enable_language(RC)
configure_file(resources/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
add_executable(chrome-alt-tab-switcher src/switcher_main.cpp src/switcher.cpp resources/app.rc "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
target_include_directories(chrome-alt-tab-switcher PRIVATE resources)
target_compile_features(chrome-alt-tab-switcher PRIVATE cxx_std_20)
set_target_properties(chrome-alt-tab-switcher PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(chrome-alt-tab-switcher PRIVATE $<$<CONFIG:Release>:WINRT_NO_SOURCE_LOCATION> UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX CHROME_ALT_TAB_VERSION="${PROJECT_VERSION}")
target_compile_options(chrome-alt-tab-switcher PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(chrome-alt-tab-switcher PRIVATE d3d11 d2d1 coremessaging windowsapp user32 gdi32 shcore shlwapi crypt32 windowscodecs ole32 dwmapi advapi32 shell32)

add_executable(switcher-model-tests ../tests/switcher_model.cpp)
target_include_directories(switcher-model-tests PRIVATE src)
target_compile_features(switcher-model-tests PRIVATE cxx_std_20)
target_compile_options(switcher-model-tests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_test(NAME switcher-model COMMAND switcher-model-tests)

# One GUI-subsystem executable also serves Chrome native messaging via inherited pipes.
target_link_options(chrome-alt-tab-switcher PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup $<$<CONFIG:Release>:/DEBUG:NONE>)
