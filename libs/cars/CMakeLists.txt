add_library(shift_cars STATIC
  src/sources.cpp
  src/database.cpp
  src/util.cpp
  src/bake.cpp
  src/bake_format.cpp
  src/physics.cpp
  src/mods.cpp
  src/mod_manager.cpp
  src/db_patch.cpp
  src/mod_merge.cpp
  src/texture_mods.cpp
  src/shader_mods.cpp
  src/mod_plugins.cpp
  src/toml_lite.cpp
  src/strings.cpp
  src/visual_setup.cpp
  src/shader_settings.cpp
  src/livery.cpp)
target_include_directories(shift_cars PUBLIC include)
target_link_libraries(shift_cars PUBLIC fh1data PRIVATE shift_sqlite3)
# Replacement textures decode PNG/TGA/JPG with stb_image when it is fetched.
if(TARGET shift_stb)
  target_link_libraries(shift_cars PRIVATE shift_stb)
  target_compile_definitions(shift_cars PRIVATE SHIFT_HAVE_STB=1)
endif()
if(MSVC)
  target_compile_options(shift_cars PRIVATE /W4)
else()
  target_compile_options(shift_cars PRIVATE -Wall -Wextra)
  find_package(Threads REQUIRED)
  target_link_libraries(shift_cars PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
