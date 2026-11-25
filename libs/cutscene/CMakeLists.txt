add_library(shift_cutscene STATIC
  src/cutscene.cpp)
target_include_directories(shift_cutscene PUBLIC include)
target_link_libraries(shift_cutscene PRIVATE shift_audio fh1data)
if(MSVC)
  target_compile_options(shift_cutscene PRIVATE /W4)
else()
  target_compile_options(shift_cutscene PRIVATE -Wall -Wextra)
endif()
