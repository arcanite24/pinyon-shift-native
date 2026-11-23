add_library(shift_setup STATIC
  src/stream_reader.cpp
  src/xiso.cpp
  src/stfs.cpp
  src/mod_installer.cpp
  src/on_device_baker.cpp)

set_target_properties(shift_setup PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(shift_setup PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(shift_setup PUBLIC fh1data shift_cars shift_world shift_audio)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(shift_setup PUBLIC Threads::Threads)
endif()
