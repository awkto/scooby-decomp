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
add_library(scooby_common OBJECT
  src/emu.c src/sha1.c src/script.c src/capture.c src/sndlog.c src/ipprof.c
  hle/hle.c hle/sandbox.c hle/gx.c
  ${GAME_SOURCES} ${MOUSE_SOURCES} ${RENDER_SOURCES})
target_include_directories(scooby_common PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/hle"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/game" "${CMAKE_CURRENT_SOURCE_DIR}/src/mouse"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render")
target_link_libraries(scooby_common PUBLIC gpgx_core)
target_compile_options(scooby_common PRIVATE -Wall -Wextra -Wno-unused-parameter
  -fno-strict-aliasing)
target_compile_definitions(scooby_common PUBLIC SCOOBY_ROOT="${SCOOBY_ROOT}")
set_source_files_properties(hle/gx.c PROPERTIES COMPILE_DEFINITIONS "${GPGX_DEFINES}")

# ---- SDL2 host ----
find_package(SDL2 QUIET)
if(SDL2_FOUND)
  add_executable(scoobyport src/main.c src/overlay.c)
  target_link_libraries(scoobyport PRIVATE scooby_common)
  if(TARGET SDL2::SDL2)
    target_link_libraries(scoobyport PRIVATE SDL2::SDL2)
  else()
    target_include_directories(scoobyport PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(scoobyport PRIVATE ${SDL2_LIBRARIES})
  endif()
  target_compile_options(scoobyport PRIVATE -Wall -Wextra -Wno-unused-parameter)
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

# ---- mouse mapping unit test ----
add_executable(mousemap_test tools/mousemap_test.c)
target_link_libraries(mousemap_test PRIVATE scooby_common)

# ---- password codec unit test (GitLab #21) ----
add_executable(password_test tools/password_test.c src/game/password.c)
target_include_directories(password_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/game")
target_compile_options(password_test PRIVATE -Wall -Wextra)
