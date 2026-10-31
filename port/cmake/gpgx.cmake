# Genesis Plus GX, compiled statically into the port.
#
# The core sources are NOT copied into this repository. At configure time we export the
# pinned commit from a Genesis-Plus-GX git checkout (default: <repo>/ext/gpgx, see README
# Setup) with `git archive`, apply tools/patches/*.patch in a fixed order, and compile the
# result from the build tree. Nothing is fetched from the network unless GPGX_CLONE=ON and
# the checkout is missing (one-time clone).

set(GPGX_COMMIT 60bee4c724859710ce05672239e5b601ad01382e)
set(GPGX_URL https://github.com/libretro/Genesis-Plus-GX)
set(GPGX_DIR "${SCOOBY_ROOT}/ext/gpgx" CACHE PATH "Genesis-Plus-GX git checkout that contains commit ${GPGX_COMMIT}")
option(GPGX_CLONE "Clone Genesis-Plus-GX into GPGX_DIR if it does not exist (network)" OFF)

# applied in this order (gpgx-hle.patch appends to vdp_ctrl.c after the vdptrace hunks)
set(GPGX_PATCHES gpgx-cpuhook.patch gpgx-vdptrace.patch gpgx-hle.patch gpgx-render.patch gpgx-native.patch)

set(GPGX_SRC "${CMAKE_BINARY_DIR}/gpgx-src")

find_package(Git REQUIRED)
find_program(PATCH_EXE patch REQUIRED)

set(_stamp "${GPGX_COMMIT}")
foreach(p ${GPGX_PATCHES})
  set(_pf "${SCOOBY_ROOT}/tools/patches/${p}")
  file(SHA1 "${_pf}" _h)
  string(APPEND _stamp " ${p}=${_h}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pf}")
endforeach()
set(_old "")
if(EXISTS "${GPGX_SRC}/.scooby-stamp")
  file(READ "${GPGX_SRC}/.scooby-stamp" _old)
endif()

if(NOT _old STREQUAL _stamp)
  if(NOT EXISTS "${GPGX_DIR}/.git")
    if(GPGX_CLONE)
      message(STATUS "Cloning ${GPGX_URL} into ${GPGX_DIR}")
      execute_process(COMMAND "${GIT_EXECUTABLE}" clone "${GPGX_URL}" "${GPGX_DIR}" RESULT_VARIABLE r)
      if(r)
        message(FATAL_ERROR "git clone failed")
      endif()
    else()
      message(FATAL_ERROR "No Genesis-Plus-GX checkout at ${GPGX_DIR}. Clone it "
        "(git clone ${GPGX_URL} ext/gpgx), pass -DGPGX_DIR=..., or configure with -DGPGX_CLONE=ON.")
    endif()
  endif()
  message(STATUS "Exporting Genesis-Plus-GX ${GPGX_COMMIT} to ${GPGX_SRC}")
  file(REMOVE_RECURSE "${GPGX_SRC}")
  file(MAKE_DIRECTORY "${GPGX_SRC}")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${GPGX_DIR}" archive --format=tar ${GPGX_COMMIT}
    COMMAND tar -x -C "${GPGX_SRC}"
    RESULT_VARIABLE r ERROR_VARIABLE e)
  if(r)
    message(FATAL_ERROR "git archive ${GPGX_COMMIT} failed in ${GPGX_DIR}: ${e}\n"
      "If the checkout is shallow, run: git -C ${GPGX_DIR} fetch --depth 1 origin ${GPGX_COMMIT}")
  endif()
  foreach(p ${GPGX_PATCHES})
    execute_process(
      COMMAND "${PATCH_EXE}" -p1 --binary --forward --batch -i "${SCOOBY_ROOT}/tools/patches/${p}"
      WORKING_DIRECTORY "${GPGX_SRC}" RESULT_VARIABLE r OUTPUT_VARIABLE o ERROR_VARIABLE e)
    if(r)
      message(FATAL_ERROR "applying ${p} failed:\n${o}${e}")
    endif()
  endforeach()
  file(WRITE "${GPGX_SRC}/.scooby-stamp" "${_stamp}")
endif()

# ---- source list (mirrors libretro/Makefile.common, HAVE_CHD=0, HAVE_CDROM=0) ----
set(_dirs core core/z80 core/m68k core/ntsc core/sound core/sound/minimp3 core/input_hw
          core/cd_hw core/cart_hw core/cart_hw/svp core/debug)
set(GPGX_SOURCES)
foreach(d ${_dirs})
  file(GLOB _s "${GPGX_SRC}/${d}/*.c")
  list(APPEND GPGX_SOURCES ${_s})
endforeach()
file(GLOB _tremor "${GPGX_SRC}/core/sound/tremor/*.c")
set(_lrc "${GPGX_SRC}/libretro/libretro-common")
set(_zlib "${GPGX_SRC}/libretro/deps/zlib-1.2.11")
list(APPEND GPGX_SOURCES ${_tremor}
  ${_lrc}/streams/file_stream.c ${_lrc}/streams/file_stream_transforms.c
  ${_lrc}/compat/fopen_utf8.c ${_lrc}/compat/compat_snprintf.c ${_lrc}/compat/compat_strl.c
  ${_lrc}/compat/compat_strcasestr.c ${_lrc}/compat/compat_posix_string.c
  ${_lrc}/encodings/encoding_utf.c ${_lrc}/file/file_path.c ${_lrc}/file/retro_dirent.c
  ${_lrc}/lists/string_list.c ${_lrc}/lists/dir_list.c ${_lrc}/memmap/memalign.c
  ${_lrc}/string/stdstring.c ${_lrc}/vfs/vfs_implementation.c
  ${_zlib}/adler32.c ${_zlib}/crc32.c ${_zlib}/inffast.c ${_zlib}/inflate.c
  ${_zlib}/inftrees.c ${_zlib}/zutil.c
  ${GPGX_SRC}/libretro/libretro.c)

add_library(gpgx_core STATIC ${GPGX_SOURCES})
set(_inc)
foreach(d ${_dirs})
  list(APPEND _inc "${GPGX_SRC}/${d}")
endforeach()
target_include_directories(gpgx_core SYSTEM PUBLIC ${_inc} "${GPGX_SRC}/libretro" "${_lrc}/include")
target_include_directories(gpgx_core PRIVATE "${_zlib}")
# same defines as `make -f Makefile.libretro HOOK_CPU=1` on unix (minus CHD/CD-ROM), plus HOOK_HLE
set(GPGX_DEFINES
  LSB_FIRST BYTE_ORDER=LITTLE_ENDIAN HAVE_ZLIB USE_LIBTREMOR
  USE_16BPP_RENDERING FRONTEND_SUPPORTS_RGB565
  __LIBRETRO__ USE_LIBRETRO_VFS M68K_OVERCLOCK_SHIFT=20 Z80_OVERCLOCK_SHIFT=20
  HAVE_YM3438_CORE HAVE_OPLL_CORE MAXROMSIZE=33554432 "INLINE=static inline"
  HOOK_CPU HOOK_HLE)
# PRIVATE: only port/hle/gx.c includes GPGX internals (it gets the same defines)
target_compile_definitions(gpgx_core PRIVATE ${GPGX_DEFINES})
target_compile_options(gpgx_core PRIVATE -w -O2 -fno-strict-aliasing)
target_link_libraries(gpgx_core PUBLIC m)

# The same machine without a 68000 (GitLab #24, the native build `scooby`): the main CPU and the
# Sega CD sub-CPU interpreters (core/m68k/m68kcpu.c, s68kcpu.c with the opcode tables they include)
# are not compiled; port/hle/native_core.c provides the CPU API on top of the native runner.
set(GPGX_SOURCES_NATIVE ${GPGX_SOURCES})
list(FILTER GPGX_SOURCES_NATIVE EXCLUDE REGEX "/core/m68k/[ms]68kcpu\\.c$")
add_library(gpgx_core_native STATIC ${GPGX_SOURCES_NATIVE})
target_include_directories(gpgx_core_native SYSTEM PUBLIC ${_inc} "${GPGX_SRC}/libretro" "${_lrc}/include")
target_include_directories(gpgx_core_native PRIVATE "${_zlib}")
target_compile_definitions(gpgx_core_native PRIVATE ${GPGX_DEFINES})
target_compile_options(gpgx_core_native PRIVATE -w -O2 -fno-strict-aliasing)
target_link_libraries(gpgx_core_native PUBLIC m)
