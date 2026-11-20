add_library(shift_freeroam STATIC
  src/collectibles.cpp)
target_include_directories(shift_freeroam PUBLIC include)
target_link_libraries(shift_freeroam PRIVATE fh1data shift_audio)
if(MSVC)
  target_compile_options(shift_freeroam PRIVATE /W4)
else()
  target_compile_options(shift_freeroam PRIVATE -Wall -Wextra)
endif()
