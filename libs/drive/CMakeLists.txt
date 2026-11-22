add_library(shift_drive STATIC
  src/vehicle.cpp
  src/from_bake.cpp
  src/mesh_ground.cpp
  src/from_physics.cpp
  src/profile_ground.cpp
  src/oracle_lap.cpp
  src/save.cpp
  src/cameras.cpp
  src/controls.cpp
  src/rumble.cpp
  src/ai_driver.cpp
  src/contact.cpp
  src/rewind.cpp
  src/driving_line.cpp
  src/lamps.cpp
  src/damage.cpp
  src/replay.cpp
  src/wheel_ffb.cpp
  src/dualsense.cpp
  src/head_tracking.cpp
  src/telemetry_udp.cpp)
target_include_directories(shift_drive PUBLIC include)
target_link_libraries(shift_drive PUBLIC shift_cars)
if(WIN32)
  target_link_libraries(shift_drive PRIVATE dinput8 dxguid ws2_32)
endif()
if(MSVC)
  target_compile_options(shift_drive PRIVATE /W4)
else()
  target_compile_options(shift_drive PRIVATE -Wall -Wextra)
endif()
