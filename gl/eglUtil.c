#include "eglUtil.h"

SDL_Window *window;
//SDL_GLContext gl_context;
//SDL_Event event;
//EGLContext  glContext;
int setupEGL(EGL_Setup *setup, int width, int height)
{
	printf("Creating Window \n");
	
	//Create Window
	window = SDL_CreateWindow(
		"test",
		0,
		0,
		width,
		height,
		SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
	
	if(window==NULL){
		printf("SDL Could not create window: ");
		SDL_GetError();
		SDL_Quit();
		return 1;
	}
	
	//SDL_GLContext gl_context;    
	//SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1"); /// Seems to work!
	//SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1"); // 0=glx, "By default SDL will use GLX when both are present."
	//SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles3"); /// Seems to work and be enough!
	
	//SDL_GL_CreateContext(window);
	
	printf("Created Window \n");
    
	/*EGLint configAttributes[] =
	{
		EGL_CONFORMANT,		EGL_OPENGL_ES2_BIT,
		EGL_SURFACE_TYPE,	EGL_WINDOW_BIT,
		EGL_RED_SIZE,		8, //5
		EGL_GREEN_SIZE,		8, //6
		EGL_BLUE_SIZE,		8, //5
		EGL_ALPHA_SIZE,		0,
		EGL_DEPTH_SIZE,		0,
		EGL_NONE
	};*/
	
	EGLint egl_config_attr[] = {
		//EGL_SURFACE_TYPE,	EGL_WINDOW_BIT,
		EGL_RED_SIZE,		8, //5
		EGL_GREEN_SIZE,		8, //6
		EGL_BLUE_SIZE,		8, //5
		EGL_RENDERABLE_TYPE,		EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};
    
	EGLint contextAttributes[] =
	{
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
    
	EGLint numConfigs, majorVersion, minorVersion;
	//EGLDisplay  glDisplay;
	EGLConfig   glConfig;

	//EGLSurface  glSurface;
	
	setup->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	eglInitialize(setup->display, &majorVersion, &minorVersion);
	eglChooseConfig(setup->display, egl_config_attr, &glConfig, 1, &numConfigs);
	
    EGLint vid;
    eglGetConfigAttrib(setup->display, glConfig, EGL_NATIVE_VISUAL_ID, &vid); ///don't know if I need or not
    
	eglBindAPI(EGL_OPENGL_ES_API);
	
	SDL_SysWMinfo sysInfo;
	SDL_VERSION(&sysInfo.version); // Set SDL version
	SDL_GetWindowWMInfo(window, &sysInfo);
	
	setup->context = eglCreateContext(setup->display, glConfig, EGL_NO_CONTEXT, contextAttributes);
	setup->surface = eglCreateWindowSurface(setup->display, glConfig,
                                       (EGLNativeWindowType)sysInfo.info.x11.window, NULL); // X11?
	eglMakeCurrent(setup->display, setup->surface, setup->surface, setup->context);
	eglSwapInterval(setup->display, 1);
	//glClearColor(0.8f, 0.2f, 0.1f, 1.0f);
	
	GLenum glerror = glGetError();
	if (glerror != GL_NO_ERROR)
	{
		vcos_log_error("GL error before init: error 0x%04x", glerror);
		//goto error_display;
	}
	
	EGLint eglerror = eglGetError();
	if (eglerror != EGL_SUCCESS)
	{
		vcos_log_error("EGL error before init: error 0x%04x", eglerror);
		//goto error_display;
	}
	
	return 0;
}

void terminateEGL(EGL_Setup *setup)
{
	eglMakeCurrent(setup->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(setup->display, setup->context);
	eglDestroySurface(setup->display, setup->surface);
	eglTerminate(setup->display);
}

/* Create native window (basically just a rectangle on the screen we can render to 
int createNativeWindow(EGL_DISPMANX_WINDOW_T *window)
{
	// Get display size
	uint32_t displayNum = 0; // Primary display
	uint32_t displayWidth, displayHeight;
	int32_t status = graphics_get_display_size(displayNum, &displayWidth, &displayHeight);
	if (status != 0) return status;

	// Setup Fullscreen
	VC_RECT_T destRect = {
		.width = (int32_t)displayWidth,
		.height = (int32_t)displayHeight,
	};
	VC_RECT_T srcRect = {
		.width = (int32_t)displayWidth << 16,
		.height = (int32_t)displayHeight << 16,
	};
	VC_DISPMANX_ALPHA_T alpha = {DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS, 255, 0};

	// Create window
	DISPMANX_DISPLAY_HANDLE_T display = vc_dispmanx_display_open(displayNum);
	DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
	DISPMANX_ELEMENT_HANDLE_T element = vc_dispmanx_element_add(update, display, 0, &destRect, 0,
		 &srcRect, DISPMANX_PROTECTION_NONE, &alpha, NULL, DISPMANX_NO_ROTATE);

	window->element = element;
	window->width = displayWidth;
	window->height = displayHeight;
	vc_dispmanx_update_submit_sync(update);

	return 0;
}*/
