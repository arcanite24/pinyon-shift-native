add_library(shift_traffic STATIC
  src/nav.cpp
  src/roads.cpp
  src/config.cpp
  src/population.cpp
  src/bodies.cpp)
target_include_directories(shift_traffic PUBLIC include)
target_link_libraries(shift_traffic PUBLIC shift_drive PRIVATE fh1data shift_audio)
if(MSVC)
  target_compile_options(shift_traffic PRIVATE /W4)
else()
  target_compile_options(shift_traffic PRIVATE -Wall -Wextra)
endif()
