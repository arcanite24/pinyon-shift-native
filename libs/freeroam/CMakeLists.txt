add_library(shift_freeroam STATIC
  src/club.cpp
  src/collectibles.cpp
  src/outposts.cpp
  src/road_discovery.cpp
  src/skills.cpp)
target_include_directories(shift_freeroam PUBLIC include)
target_link_libraries(shift_freeroam PUBLIC shift_traffic PRIVATE fh1data shift_audio)
if(MSVC)
  target_compile_options(shift_freeroam PRIVATE /W4)
else()
  target_compile_options(shift_freeroam PRIVATE -Wall -Wextra)
endif()
