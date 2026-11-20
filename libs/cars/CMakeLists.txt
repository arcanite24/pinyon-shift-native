add_library(shift_cars STATIC
  src/sources.cpp
  src/database.cpp
  src/util.cpp
  src/bake.cpp
  src/bake_format.cpp
  src/physics.cpp)
target_include_directories(shift_cars PUBLIC include)
target_link_libraries(shift_cars PUBLIC fh1data PRIVATE shift_sqlite3)
if(MSVC)
  target_compile_options(shift_cars PRIVATE /W4)
else()
  target_compile_options(shift_cars PRIVATE -Wall -Wextra)
  find_package(Threads REQUIRED)
  target_link_libraries(shift_cars PUBLIC Threads::Threads)
endif()
