add_library(fh1data STATIC
  src/archive.cpp
  src/xmem.cpp
  src/xds.cpp
  src/carbin.cpp
  src/rmb.cpp
  src/members.cpp
  src/placements.cpp
  src/props.cpp)
target_include_directories(fh1data PUBLIC include)
target_link_libraries(fh1data PRIVATE shift_mspack_lzxd)
if(MSVC)
  target_compile_options(fh1data PRIVATE /W4)
else()
  target_compile_options(fh1data PRIVATE -Wall -Wextra)
endif()
