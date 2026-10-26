cmake_minimum_required(VERSION 3.16)
project(gw3nx C CXX)
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS A32 CACHE STRING "" FORCE)
add_subdirectory(ext/dynarmic EXCLUDE_FROM_ALL)

set(SOURCES src/main.cpp src/core.cpp src/loader.cpp src/hle_libc.cpp src/hle_gl.cpp src/jni.cpp
            src/hle_android.cpp src/hle_opensl.cpp src/overlay.cpp src/options.cpp)
add_executable(gw3nx ${SOURCES})
target_include_directories(gw3nx PRIVATE ext/stb)

if(NX)
  # Switch: SDL2 + Mesa (EGL/GLESv2 over nouveau) portlibs, then pack an .nro
  target_link_libraries(gw3nx PRIVATE dynarmic SDL2 EGL GLESv2 glapi drm_nouveau nx m)
  target_link_options(gw3nx PRIVATE -Wl,--wrap=exit,--wrap=_exit)   # log silent exits
  set_target_properties(gw3nx PROPERTIES SUFFIX ".elf")
  set(TOOLS ${DEVKITPRO}/tools/bin)
  # icon.jpg (from tools/make_icon.sh with your APK) becomes the HOME-menu icon, if present
  if(EXISTS ${CMAKE_SOURCE_DIR}/icon.jpg)
    set(GW3_ICON ${CMAKE_SOURCE_DIR}/icon.jpg)
  endif()
  add_custom_command(TARGET gw3nx POST_BUILD
    COMMAND ${TOOLS}/nacptool --create "Geometry Wars 3: Dimensions" "gw3nx" "0.28" ${CMAKE_BINARY_DIR}/gw3nx.nacp
    COMMAND ${TOOLS}/elf2nro $<TARGET_FILE:gw3nx> ${CMAKE_BINARY_DIR}/gw3nx.nro --nacp=${CMAKE_BINARY_DIR}/gw3nx.nacp $<$<BOOL:${GW3_ICON}>:--icon=${GW3_ICON}>
    COMMENT "Packing gw3nx.nro")
elseif(GW3_HEADLESS)
  # ARM64 backend test: real JIT, stub SDL/GL (see test/)
  target_sources(gw3nx PRIVATE test/gl_stub.cpp test/sdl_stub.cpp)
  target_link_libraries(gw3nx PRIVATE dynarmic)
  target_link_options(gw3nx PRIVATE -static)
else()
  find_package(SDL2 REQUIRED)
  target_link_libraries(gw3nx PRIVATE dynarmic SDL2::SDL2 GLESv2)
endif()
