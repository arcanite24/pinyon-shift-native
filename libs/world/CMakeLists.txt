add_library(shift_world STATIC
  src/world.cpp
  src/stream.cpp
  src/road_follow.cpp
  src/collision.cpp
  src/tod.cpp
  src/vfx.cpp
  src/smashables.cpp
  src/world_anim.cpp
  src/cell_streamer.cpp)
target_include_directories(shift_world PUBLIC include)
target_link_libraries(shift_world PUBLIC fh1data shift_drive)
if(MSVC)
  target_compile_options(shift_world PRIVATE /W4)
else()
  target_compile_options(shift_world PRIVATE -Wall -Wextra)
endif()
