add_library(shift_launcher_core STATIC
  sha256.cpp
  launcher_core.cpp
  wifi_sync.cpp)
target_include_directories(shift_launcher_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(TARGET shift_volk)
  target_link_libraries(shift_launcher_core PUBLIC shift_volk)
  target_compile_definitions(shift_launcher_core PUBLIC SHIFT_LAUNCHER_HAS_VOLK=1)
endif()
if(WIN32)
  target_link_libraries(shift_launcher_core PUBLIC ws2_32 iphlpapi winhttp)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(shift_launcher_core PUBLIC Threads::Threads)
endif()

if(SHIFT_BUILD_CARVIEW AND NOT ANDROID)
  add_executable(shift-launcher
    main.cpp
    launcher_ui.cpp)
  target_include_directories(shift-launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(shift-launcher PRIVATE shift_launcher_core carview_render shift_stb SDL3::SDL3-static)
  if(APPLE)
    set_target_properties(shift-launcher PROPERTIES
      BUILD_RPATH "@executable_path/../Frameworks"
      INSTALL_RPATH "@executable_path/../Frameworks")
  endif()
endif()
