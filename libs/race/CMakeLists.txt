add_library(shift_race STATIC
  src/owt.cpp
  src/route.cpp
  src/event.cpp
  src/difficulty.cpp
  src/race_line.cpp
  src/session.cpp
  src/rewards.cpp
  src/street_line.cpp
  src/rally.cpp
  src/career.cpp)
target_include_directories(shift_race PUBLIC include)
target_link_libraries(shift_race PUBLIC shift_drive shift_traffic shift_data fh1data PRIVATE shift_sqlite3 shift_audio)
if(MSVC)
  target_compile_options(shift_race PRIVATE /W4)
else()
  target_compile_options(shift_race PRIVATE -Wall -Wextra)
endif()
