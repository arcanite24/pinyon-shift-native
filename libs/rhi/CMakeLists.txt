option(SHIFT_RHI_VULKAN "Compile the Vulkan 1.3 reference RHI backend" ON)

set(SHIFT_RHI_SOURCES
  src/rhi_factory.cpp
  src/rhi_render_graph.cpp
  src/rhi_meshlet.cpp
  src/rhi_clustered_lights.cpp
  src/rhi_vis_buffer.cpp
  src/rhi_xenos_detile_gpu.cpp
  src/rhi_post_fx_compute.cpp)

set(SHIFT_RHI_USE_VK OFF)
if(SHIFT_RHI_VULKAN AND TARGET shift_volk AND TARGET shift_vma)
  set(SHIFT_RHI_USE_VK ON)
  list(APPEND SHIFT_RHI_SOURCES
    src/rhi_vk_device.cpp
    src/rhi_vk_cmd.cpp
    src/rhi_vk_rt.cpp)
endif()

set(SHIFT_RHI_USE_METAL OFF)
if(APPLE)
  option(SHIFT_RHI_METAL "Compile the native Metal 3 RHI backend" ON)
  if(SHIFT_RHI_METAL)
    set(SHIFT_RHI_USE_METAL ON)
    list(APPEND SHIFT_RHI_SOURCES
      src/rhi_metal_device.mm
      src/rhi_metal_cmd.mm
      src/rhi_metal_rt.mm)
  endif()
endif()

set(SHIFT_RHI_USE_D3D12 OFF)
if(WIN32)
  option(SHIFT_RHI_D3D12 "Compile the native DirectX 12 RHI backend on Windows" ON)
  if(SHIFT_RHI_D3D12)
    set(SHIFT_RHI_USE_D3D12 ON)
    list(APPEND SHIFT_RHI_SOURCES
      src/rhi_d3d12_device.cpp
      src/rhi_d3d12_cmd.cpp
      src/rhi_d3d12_rt.cpp)
  endif()
endif()

add_library(shift_rhi STATIC ${SHIFT_RHI_SOURCES})
set_target_properties(shift_rhi PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(shift_rhi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(SHIFT_RHI_USE_VK)
  target_link_libraries(shift_rhi PRIVATE shift_volk shift_vma)
  target_compile_definitions(shift_rhi PRIVATE SHIFT_RHI_VULKAN=1)
  target_compile_definitions(shift_rhi PUBLIC SHIFT_RHI_PROVIDES_VMA=1)
  if(SHIFT_ENABLE_RAY_TRACING)
    target_compile_definitions(shift_rhi PRIVATE SHIFT_ENABLE_RAY_TRACING=1)
  endif()
  if(TARGET SDL3::SDL3-static)
    target_link_libraries(shift_rhi PRIVATE SDL3::SDL3-static)
    target_compile_definitions(shift_rhi PRIVATE SHIFT_RHI_HAS_SDL3=1)
  endif()
endif()

if(APPLE AND SHIFT_RHI_USE_METAL)
  set_source_files_properties(
    src/rhi_metal_device.mm
    src/rhi_metal_cmd.mm
    src/rhi_metal_rt.mm
    PROPERTIES
      LANGUAGE CXX
      COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
  target_compile_definitions(shift_rhi PRIVATE SHIFT_RHI_METAL=1)
  if(SHIFT_ENABLE_RAY_TRACING)
    target_compile_definitions(shift_rhi PRIVATE SHIFT_ENABLE_RAY_TRACING=1)
  endif()
  if(TARGET SDL3::SDL3-static)
    target_link_libraries(shift_rhi PRIVATE SDL3::SDL3-static)
    target_compile_definitions(shift_rhi PRIVATE SHIFT_RHI_HAS_SDL3=1)
  endif()
  find_library(SHIFT_METAL_FRAMEWORK Metal REQUIRED)
  find_library(SHIFT_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
  find_library(SHIFT_FOUNDATION_FRAMEWORK Foundation REQUIRED)
  target_link_libraries(shift_rhi PRIVATE
    ${SHIFT_METAL_FRAMEWORK}
    ${SHIFT_QUARTZCORE_FRAMEWORK}
    ${SHIFT_FOUNDATION_FRAMEWORK})
  find_library(SHIFT_COREGRAPHICS_FRAMEWORK CoreGraphics)
  if(SHIFT_COREGRAPHICS_FRAMEWORK)
    target_link_libraries(shift_rhi PRIVATE ${SHIFT_COREGRAPHICS_FRAMEWORK})
  endif()
  find_library(SHIFT_APPKIT_FRAMEWORK AppKit)
  if(SHIFT_APPKIT_FRAMEWORK)
    target_link_libraries(shift_rhi PRIVATE ${SHIFT_APPKIT_FRAMEWORK})
    target_compile_definitions(shift_rhi PRIVATE SHIFT_RHI_HAS_APPKIT=1)
  endif()
  find_library(SHIFT_METALFX_FRAMEWORK MetalFX)
  if(SHIFT_METALFX_FRAMEWORK)
    target_link_libraries(shift_rhi PRIVATE ${SHIFT_METALFX_FRAMEWORK})
    target_compile_definitions(shift_rhi PRIVATE SHIFT_RHI_HAS_METALFX=1)
  endif()
endif()

if(SHIFT_RHI_USE_D3D12)
  target_link_libraries(shift_rhi PRIVATE d3d12 dxgi dxguid d3dcompiler)
  target_compile_definitions(shift_rhi PRIVATE SHIFT_RHI_D3D12=1)
  if(SHIFT_ENABLE_RAY_TRACING)
    target_compile_definitions(shift_rhi PRIVATE SHIFT_ENABLE_RAY_TRACING=1)
  endif()
  if(TARGET SDL3::SDL3-static)
    target_link_libraries(shift_rhi PRIVATE SDL3::SDL3-static)
    target_compile_definitions(shift_rhi PRIVATE SHIFT_RHI_HAS_SDL3=1)
  endif()
endif()

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(shift_rhi PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

if(MSVC)
  target_compile_options(shift_rhi PRIVATE /W4)
else()
  target_compile_options(shift_rhi PRIVATE -Wall -Wextra)
endif()
