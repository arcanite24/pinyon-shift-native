add_library(shift_garage STATIC
  src/garage.cpp
  src/pi_calc.cpp
  src/tuning.cpp
  src/auto_upgrade.cpp)
target_include_directories(shift_garage PUBLIC include)
target_link_libraries(shift_garage PUBLIC shift_cars shift_drive PRIVATE shift_sqlite3)
if(MSVC)
  target_compile_options(shift_garage PRIVATE /W4)
else()
  target_compile_options(shift_garage PRIVATE -Wall -Wextra)
endif()
