# Builds the pieces a native PS5 title links besides the game: SDL2 (static, with the console's video, audio and pad drivers
# from deps/ps5-doom3/ps5/sdl, patched in by deps/sdl2-ps5) and the console glue (klog, pad, time, libc names).
# Called by tools/ps5/build_title_deps.sh with the payload SDK's toolchain file. The deps/ code is MIT/zlib/GPL by its
# authors: it is compiled from there, never copied into this repository.
cmake_minimum_required(VERSION 3.16)
project(p2_ps5_title_deps C CXX ASM)

foreach(required PS5_PORT_DIR SDL_DIR)
    if(NOT ${required})
        message(FATAL_ERROR "set ${required}")
    endif()
endforeach()
set(ps5 ${PS5_PORT_DIR}/ps5)

add_compile_options(-g1 -ffunction-sections -fdata-sections -fno-omit-frame-pointer -march=znver2)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF)
include_directories(BEFORE ${ps5}/include)

# The console glue; startup.c (Doom 3's constructor) and devloop_shot.c are left out: runtime/ps5.c is the game's.
add_library(ps5_platform STATIC ${ps5}/src/platform.c ${ps5}/src/shims.c)
target_include_directories(ps5_platform PUBLIC ${ps5}/src)
target_compile_definitions(ps5_platform PRIVATE "PS5_TITLE_NAME=\"Portal 2\"")
target_include_directories(ps5_platform PRIVATE ${SDL_DIR}/include)

set(SDL_PS5_PORT_DIR ${ps5}/sdl CACHE INTERNAL "")
set(SDL_PS5_PLATFORM_DIR ${ps5}/src CACHE INTERNAL "")
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
foreach(off SDL_SHARED SDL_TEST SDL_TESTS
        SDL_X11 SDL_WAYLAND SDL_KMSDRM SDL_RPI SDL_VIVANTE SDL_OFFSCREEN SDL_DUMMYVIDEO SDL_DIRECTFB
        SDL_OPENGL SDL_OPENGLES SDL_VULKAN
        SDL_OSS SDL_ALSA SDL_JACK SDL_ESD SDL_ARTS SDL_NAS SDL_PIPEWIRE SDL_PULSEAUDIO SDL_SNDIO SDL_FUSIONSOUND
        SDL_DISKAUDIO SDL_DUMMYAUDIO SDL_LIBSAMPLERATE
        SDL_DBUS SDL_IBUS SDL_LIBUDEV SDL_HIDAPI SDL_HIDAPI_LIBUSB SDL_HIDAPI_JOYSTICK SDL_VIRTUAL_JOYSTICK
        SDL_SYSTEM_ICONV SDL_LIBICONV SDL_RPATH SDL_SENSOR SDL_POWER)   # SDL_HAPTIC stays on (dummy backend): Source gives up on the pad when SDL_INIT_HAPTIC fails
    set(${off} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDL_HAPTIC ON CACHE BOOL "" FORCE)
add_subdirectory(${SDL_DIR} ${CMAKE_BINARY_DIR}/SDL2 EXCLUDE_FROM_ALL)
target_include_directories(ps5_platform PRIVATE ${SDL_DIR}/include)
