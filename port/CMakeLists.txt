cmake_minimum_required(VERSION 3.16)
project(scoobyport C)

# usage (from the repo root):
#   cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release && cmake --build build/port -j4
# targets: scoobyport (SDL2 host), difftest (headless HLE equivalence tester)

get_filename_component(SCOOBY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(cmake/gpgx.cmake)

# ---- emulator wrapper + HLE layer + decompiled game code (shared by all programs) ----
# OBJECT library so that HLE_FUNC registrations (constructors) are never dropped by the linker.
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.c")
file(GLOB MOUSE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/mouse/*.c")
file(GLOB RENDER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/render/*.c")
set(SCOOBY_COMMON_SOURCES
  src/emu.c src/sha1.c src/saveslot.c src/script.c src/capture.c src/sndlog.c src/ipprof.c
  hle/hle.c hle/sandbox.c hle/gx.c hle/native.c
  ${GAME_SOURCES} ${MOUSE_SOURCES} ${RENDER_SOURCES})
option(SCOOBY_DIST "release build" OFF)
# scooby_common: on GPGX with its 68k core (scoobyport, difftest, the tools);
# scooby_native_common: the same code on gpgx_core_native, without a 68k core (the scooby build, #24)
foreach(lib scooby_common scooby_native_common)
  if(lib STREQUAL scooby_common)
    add_library(${lib} OBJECT ${SCOOBY_COMMON_SOURCES})
    target_link_libraries(${lib} PUBLIC gpgx_core)
  else()
    add_library(${lib} OBJECT ${SCOOBY_COMMON_SOURCES} hle/native_core.c)
    target_link_libraries(${lib} PUBLIC gpgx_core_native)
    target_compile_definitions(${lib} PUBLIC SCOOBY_NO_68K)
  endif()
  target_include_directories(${lib} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/hle"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/game" "${CMAKE_CURRENT_SOURCE_DIR}/src/mouse"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/render")
  target_compile_options(${lib} PRIVATE -Wall -Wextra -Wno-unused-parameter
    -fno-strict-aliasing)
  target_compile_definitions(${lib} PUBLIC SCOOBY_ROOT="${SCOOBY_ROOT}")
  # release build (tools/package.sh): no development-tree ROM lookups
  if(SCOOBY_DIST)
    target_compile_definitions(${lib} PUBLIC SCOOBY_DIST)
  endif()
endforeach()
set_source_files_properties(hle/gx.c hle/native.c hle/native_core.c PROPERTIES COMPILE_DEFINITIONS "${GPGX_DEFINES}")

# ---- SDL2 host ----
find_package(SDL2 QUIET)
if(SDL2_FOUND)
  add_executable(scoobyport src/main.c src/overlay.c src/options.c src/romfind.c src/hostmenu.c)
  target_link_libraries(scoobyport PRIVATE scooby_common)
  if(TARGET SDL2::SDL2)
    target_link_libraries(scoobyport PRIVATE SDL2::SDL2)
  else()
    target_include_directories(scoobyport PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(scoobyport PRIVATE ${SDL2_LIBRARIES})
  endif()
  target_compile_options(scoobyport PRIVATE -Wall -Wextra -Wno-unused-parameter)
  # the native build (#24): the same host, no 68k core linked (docs/native.md)
  add_executable(scooby src/main.c src/overlay.c src/options.c src/romfind.c src/hostmenu.c)
  target_link_libraries(scooby PRIVATE scooby_native_common)
  if(TARGET SDL2::SDL2)
    target_link_libraries(scooby PRIVATE SDL2::SDL2)
  else()
    target_include_directories(scooby PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(scooby PRIVATE ${SDL2_LIBRARIES})
  endif()
  target_compile_options(scooby PRIVATE -Wall -Wextra -Wno-unused-parameter)
else()
  message(WARNING "SDL2 not found: only the headless tools will be built (apt install libsdl2-dev)")
endif()

# ---- headless differential tester ----
add_executable(difftest tools/difftest.c)
target_link_libraries(difftest PRIVATE scooby_common)
target_compile_options(difftest PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---- frame-structure profiler (main loop / interrupt hooks per frame) ----
add_executable(frameprof tools/frameprof.c)
target_link_libraries(frameprof PRIVATE scooby_common)
target_compile_options(frameprof PRIVATE -Wall -Wextra -Wno-unused-parameter)
# the same on the build without a 68k core (#24: irqtest on the native runner, port/tools/nativewrap.sh)
add_executable(frameprof_native tools/frameprof.c)
target_link_libraries(frameprof_native PRIVATE scooby_native_common)
target_compile_options(frameprof_native PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---- mouse mapping unit test ----
add_executable(mousemap_test tools/mousemap_test.c)
target_link_libraries(mousemap_test PRIVATE scooby_common)

# ---- password codec unit test (GitLab #21) ----
add_executable(password_test tools/password_test.c src/game/password.c)
target_include_directories(password_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/game")
target_compile_options(password_test PRIVATE -Wall -Wextra)
