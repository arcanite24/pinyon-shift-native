add_library(shift_audio STATIC
  src/xml.cpp
  src/engine_sound.cpp
  src/fsb4.cpp
  src/sources.cpp
  src/engine_player.cpp
  src/bake.cpp
  src/radio.cpp
  src/radio_dj.cpp
  src/runtime_radio.cpp
  src/stream.cpp
  src/user_music.cpp
  src/mixer.cpp
  src/snapshots.cpp
  src/fev.cpp
  src/event_configs.cpp
  src/sfx_engine.cpp
  src/ui_audio.cpp
  src/satnav_vo.cpp
  src/codriver_vo.cpp
  src/vo_player.cpp
  src/world_acoustics.cpp
  src/xma_decode.cpp
  src/audio_engine.cpp)
target_include_directories(shift_audio PUBLIC include)
target_link_libraries(shift_audio PUBLIC shift_cars fh1data PRIVATE shift_dr_libs)
if(SHIFT_EMBED_XMA2)
  target_link_libraries(shift_audio PRIVATE shift_ffmpeg_avcodec)
  target_compile_definitions(shift_audio PRIVATE SHIFT_HAVE_INPROCESS_XMA2=1)
endif()
if(MSVC)
  target_compile_options(shift_audio PRIVATE /W4)
else()
  target_compile_options(shift_audio PRIVATE -Wall -Wextra)
endif()
