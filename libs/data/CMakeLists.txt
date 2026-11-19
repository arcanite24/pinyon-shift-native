add_library(shift_data STATIC
  src/event_restrictions.cpp)
target_include_directories(shift_data PUBLIC include)
if(MSVC)
  target_compile_options(shift_data PRIVATE /W4)
else()
  target_compile_options(shift_data PRIVATE -Wall -Wextra)
endif()
