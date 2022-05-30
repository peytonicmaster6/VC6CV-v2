#ifndef EGLUTIL_H
#define EGLUTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include "bcm_host.h"
#include <stdio.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL.h>

#include "applog.h"

#define CHECK_EVAL(EVAL, MSG, ERRHANDLER) \
	if (!(EVAL)) { \
		printf(MSG); \
		goto ERRHANDLER; \
	}
	
#define CHECK_CONDITION(cond, ...) \
	if (cond) { \
		int errsv = errno; \
		fprintf(stderr, "ERROR(%s:%d) : ", \
		__FILE__, __LINE__); \
		errno = errsv; \
		fprintf(stderr,  __VA_ARGS__); \
		abort(); \
	}

typedef struct EGL_Setup
{
	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;
	EGLint versionMinor;
	EGLint versionMajor;
} EGL_Setup;

int setupEGL(EGL_Setup *setup, int width, int height);

void terminateEGL(EGL_Setup *setup);

/* Create native window (basically just a rectangle on the screen we can render to */
//int createNativeWindow(EGL_DISPMANX_WINDOW_T *window);

#ifdef __cplusplus
}
#endif

#endif
