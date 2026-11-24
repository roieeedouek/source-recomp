#pragma once
// CGL share-group emulation for gl.c: one shared context per process here.
#include <SDL2/SDL.h>
typedef void *CGLContextObj;
typedef void *CGLShareGroupObj;
static inline CGLContextObj CGLGetCurrentContext(void) { return SDL_GL_GetCurrentContext(); }
static inline CGLShareGroupObj CGLGetShareGroup(CGLContextObj c) { return c; }
