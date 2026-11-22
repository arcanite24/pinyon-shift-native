add_library(shift_audio STATIC
  src/xml.cpp
  src/engine_sound.cpp
  src/fsb4.cpp
  src/sources.cpp
  src/engine_player.cpp
  src/bake.cpp
  src/radio.cpp
  src/stream.cpp
  src/user_music.cpp
  src/mixer.cpp
  src/snapshots.cpp
  src/fev.cpp
  src/event_configs.cpp
  src/audio_engine.cpp)
target_include_directories(shift_audio PUBLIC include)
target_link_libraries(shift_audio PUBLIC shift_cars PRIVATE shift_dr_libs)
if(MSVC)
  target_compile_options(shift_audio PRIVATE /W4)
else()
  target_compile_options(shift_audio PRIVATE -Wall -Wextra)
endif()
