add_library(shift_drive STATIC
  src/vehicle.cpp
  src/from_bake.cpp
  src/mesh_ground.cpp
  src/from_physics.cpp
  src/profile_ground.cpp
  src/oracle_lap.cpp
  src/save.cpp
  src/cameras.cpp
  src/rumble.cpp
  src/ai_driver.cpp
  src/contact.cpp)
target_include_directories(shift_drive PUBLIC include)
target_link_libraries(shift_drive PUBLIC shift_cars)
if(MSVC)
  target_compile_options(shift_drive PRIVATE /W4)
else()
  target_compile_options(shift_drive PRIVATE -Wall -Wextra)
endif()
