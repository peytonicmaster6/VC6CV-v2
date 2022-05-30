#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include "applog.h"

#include "gcs.h"
#include "camGL.h"
#include "glhelp.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3platform.h> 
#include <EGL/egl.h>
#include <EGL/eglext.h>
//#include <EGL/eglext_brcm.h>
#include "interface/vcos/vcos.h"
#include "interface/vcos/vcos_stdint.h"
#include "interface/vcos/vcos_stdbool.h"

#include <libdrm/drm_fourcc.h>

VCOS_LOG_CAT_T app_log_category;

#define CHECK_STATUS_V(STATUS, MSG, ERRHANDLER) \
	if (STATUS != VCOS_SUCCESS) { \
		vcos_log_error(MSG); \
		goto ERRHANDLER; \
	}
#define CHECK_EVAL(EVAL, MSG, ERRHANDLER) \
	if (!(EVAL)) { \
		printf(MSG); \
		goto ERRHANDLER; \
	}

// Max number of simultaneous EGL images supported = max number of distinct video decoder buffers
#define MAX_SIMUL_FRAMES 4
#define EGL_IMAGE_BRCM_MULTIMEDIA        0x99930B2
#define EGL_IMAGE_BRCM_MULTIMEDIA_Y      0x99930C0
#define EGL_IMAGE_BRCM_MULTIMEDIA_U      0x99930C1
#define EGL_IMAGE_BRCM_MULTIMEDIA_V      0x99930C2

// A camera frame with MMAL opaque buffer handle and corresponding EGL image
typedef struct CamGL_FrameInternal
{
	void *mmalBufferHandle;
	CamGL_FrameFormat format;
	EGLImageKHR eglImageRGB;
	EGLImageKHR eglImageY;
	EGLImageKHR eglImageU;
	EGLImageKHR eglImageV;
} CamGL_FrameInternal;

/* Camera GL
	Wrapper around GCS that sets up a OpenGL ES 2.0 environment and passes
	GL_TEXTURE_EXTERNAL_OES textures to user ready for processing
*/
typedef struct CamGL
{
	// Flags
	bool started; // Camera is running
	bool quit; // Signal for thread to quit
	bool error; // Error Flag

	// CamGL parameters
	CamGL_Params params;

	// GPU Camera Stream Interface
	GCS *gcs;

	// EGL / GL
	EGL_Setup eglSetup;
	CamGL_Frame frame;

	// Realtime Threading
	VCOS_MUTEX_T accessMutex; // For synchronising access to all fields

	// Table of EGL images corresponding to MMAL opaque buffer handles
	CamGL_FrameInternal frames[MAX_SIMUL_FRAMES];
} CamGL;

/* Local function prototypes */
static int camGL_initGL(CamGL *camGL);
static void camGL_stopGL(CamGL *camGL);
static void camGL_checkGL(CamGL *camGL, uint32_t line);

static int camGL_processCameraFrame(CamGL *camGL, void *frameBuffer);

static void camGL_setQuit(CamGL *camGL, bool error);
static bool camGL_getQuit(CamGL *camGL);

#define CHECK_GL(GL) camGL_checkGL(GL, __LINE__)


CamGL *camGL_create(EGL_Setup EGLSetup, const CamGL_Params *params)
{
	VCOS_STATUS_T vstatus;
	int istatus;

	// Init VCOS logging
	vcos_log_set_level(VCOS_LOG_CATEGORY, VCOS_LOG_INFO);
	vcos_log_register("CamGL", VCOS_LOG_CATEGORY);

	// Allocate CamGL structure
	CamGL *camGL = vcos_calloc(1, sizeof(*camGL), "camGL");
	CHECK_STATUS_V((camGL? VCOS_SUCCESS : VCOS_ENOMEM), "Memory allocation failure", error_ctx);
	camGL->params = *params;
	camGL->eglSetup = EGLSetup;

	// Initialize Semaphores
	vstatus = vcos_mutex_create(&camGL->accessMutex, "camGL-access");
	CHECK_STATUS_V(vstatus, "Error creating semaphore", error_mutex1);

	// Init EGL
	istatus = camGL_initGL(camGL);
	CHECK_STATUS_V(istatus, "Error initialising EGL", error_gl);

	// Init GCS
	GCS_CameraParams gcsParams;
	gcsParams.mmalEnc = 0;
	gcsParams.width = params->width;
	gcsParams.height = params->height;
	gcsParams.fps = params->fps;
	gcsParams.shutterSpeed = params->shutterSpeed;
	gcsParams.iso = params->iso;
	gcsParams.camera_num = params->camera_num;

	camGL->gcs = gcs_create(&gcsParams);
	CHECK_STATUS_V(camGL->gcs? 0 : 1, "Error initialising GCS", error_gcs);

	camGL->quit = false;
	camGL->error = false;

	vcos_log_trace("Finished CamGL init");

	return camGL;

error_gcs:
	camGL_stopGL(camGL);
error_gl:
	vcos_mutex_delete(&camGL->accessMutex);
error_mutex1:
	vcos_free(camGL);
error_ctx:
	return NULL;
}

void camGL_destroy(CamGL *camGL)
{
	gcs_destroy(camGL->gcs);
	camGL_stopGL(camGL);
	vcos_mutex_delete(&camGL->accessMutex);
	vcos_free(camGL);
}

static int camGL_initGL(CamGL *camGL)
{
	// Generate textures the current frame image is bound to
	glGenTextures(1, &camGL->frame.textureRGB);
	glGenTextures(1, &camGL->frame.textureY);
	glGenTextures(1, &camGL->frame.textureU);
	glGenTextures(1, &camGL->frame.textureV);

	// Init GL for video processing
	glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_DITHER);

	GLenum glerror = glGetError();
	if (glerror != GL_NO_ERROR)
	{
		vcos_log_error("GL error during init: error 0x%04x", glerror);
		return -1;
	}

	return 0;
}

static void camGL_stopGL(CamGL *camGL)
{
	// Delete EGL images
	for (int i = 0; i < MAX_SIMUL_FRAMES; i++)
	{
		CamGL_FrameInternal *slot = &camGL->frames[i];
		slot->mmalBufferHandle = NULL;
		if (slot->eglImageRGB) eglDestroyImageKHR(camGL->eglSetup.display, slot->eglImageRGB);
		if (slot->eglImageY) eglDestroyImageKHR(camGL->eglSetup.display, slot->eglImageY);
		if (slot->eglImageU) eglDestroyImageKHR(camGL->eglSetup.display, slot->eglImageU);
		if (slot->eglImageV) eglDestroyImageKHR(camGL->eglSetup.display, slot->eglImageV);
		slot->eglImageRGB = slot->eglImageY = slot->eglImageU = slot->eglImageV = NULL;
	}

	// Delete frame textures
	glDeleteTextures(1, &camGL->frame.textureRGB);
	glDeleteTextures(1, &camGL->frame.textureY);
	glDeleteTextures(1, &camGL->frame.textureU);
	glDeleteTextures(1, &camGL->frame.textureV);
}

static void camGL_checkGL(CamGL *camGL, uint32_t line)
{
	GLenum error = glGetError();
	if (error != GL_NO_ERROR) camGL_setQuit(camGL, true);
	while (error != GL_NO_ERROR)
	{
		vcos_log_error("GL error: line %d error 0x%04x", line, error);
		error = glGetError();
	}
}

/** Start camera */
uint8_t camGL_startCamera(CamGL *camGL)
{
	CHECK_GL(camGL);
	if (camGL->error)
	{
		vcos_log_error("CamGL error before starting camera!");
		return CAMGL_ERROR;
	}

	if (camGL->started)
	{
		vcos_log_error("Camera already started!");
		return CAMGL_ALREADY_STARTED;
	}

	// Reset stats and flags
	camGL->quit = false;
	camGL->error = false;

	// Start GPU Camera Stream and process incoming frames
	if (gcs_start(camGL->gcs) == 0)
	{
		vcos_log_info("Started GCS!");
		camGL->started = true;
		return CAMGL_SUCCESS;
	}
	else
	{
		vcos_log_error("Failed to start GPU Camera Stream!");
		camGL->started = false;
		return CAMGL_START_FAILED;
	}
}

/** Explicitly stop gl camera stream */
uint8_t camGL_stopCamera(CamGL *camGL)
{
	uint8_t code = CAMGL_SUCCESS;

	if (camGL->started)
	{
		vcos_log_info("Stopping GCS!");

		// Stop
		camGL->started = false;
		camGL_setQuit(camGL, false);
		gcs_stop(camGL->gcs);

		// Log potential errors
		if (camGL->error)
		{
			vcos_log_error("CamGL encountered an error!");
			code = CAMGL_GL_ERROR;
		}
	}

	// Make sure buffers are released
	glFlush();
	glClearColor(0, 0, 0, 0);
	for (uint8_t i = 0; i < 10; i++)
	{
		glClear(GL_COLOR_BUFFER_BIT);
		eglSwapBuffers(camGL->eglSetup.display, camGL->eglSetup.surface);
	}
	glFlush();
	CHECK_GL(camGL);

	return code;
}

/* Returns constant reference to frame  */
CamGL_Frame* camGL_getFrame(CamGL *camGL)
{
	return &camGL->frame;
}

/* Returns whether there is a new camera frame available */
uint8_t camGL_hasNextFrame(CamGL *camGL)
{
	return gcs_hasFrameBuffer(camGL->gcs);
}

/* Updates the frame structure with the most recent camera frame. 
 * If no camera frame is available yet, blocks until there is.
 * If the last frame has not been returned yet or camera stream was interrupted, returns error code. */
int camGL_nextFrame(CamGL *camGL)
{
	if (!camGL->started)
	{
		vcos_log_error("Camera not started before requesting frames!");
		return CAMGL_NOT_STARTED;
	}

	if (camGL_getQuit(camGL))
	{
		uint8_t code = camGL_stopCamera(camGL);
		return code? CAMGL_QUIT : code;
	}

	gcs_returnFrameBuffer(camGL->gcs);

	void *cameraBufferHeader = gcs_requestFrameBuffer(camGL->gcs);
	if (cameraBufferHeader)
	{
		void *cameraBuffer = gcs_getFrameBufferData(cameraBufferHeader);
		if (camGL_processCameraFrame(camGL, cameraBuffer) == 0)
			return CAMGL_SUCCESS;
		vcos_log_error("Failed to process frame!");
		return CAMGL_ERROR;
	}
	else 
		vcos_log_error("No frame received!");
	return CAMGL_NO_FRAMES;
}

/** Process one incoming camera frame buffer */
static int camGL_processCameraFrame(CamGL *camGL, void *frameBuffer)
{
	//uint32_t offsets[4] = { 0 };
	//uint32_t pitches[4] = { 0 };
	//uint32_t bo_handles[4] = { 0 };
	//uint64_t modifiers[4] = { 0 };
	//unsigned int fourcc = MMAL_FOURCC('Y','U','1','2');
	//int ret;
	
	//EGLint egl_major, egl_minor;
	//eglInitialize(camGL->eglSetup.display, &egl_major, &egl_minor);
	
	/* Lookup or create EGL image corresponding to supplied buffer handle
	 * Frames array is filled in sequentially and frames are bound to one buffer over their lifetime */
	int i;
	CamGL_FrameInternal *frameInt = NULL;
	for (i = 0; i < MAX_SIMUL_FRAMES; i++)
	{
		frameInt = &camGL->frames[i];
		if (frameInt->mmalBufferHandle == frameBuffer)
		{ // Found cached frame and corresponding image
			break;
		}

		if (frameInt->mmalBufferHandle == NULL)
		{ // Found unused frame - buffer has yet to be cached and associated with an image
			CHECK_GL(camGL);

			//EGLint attribs[] = {
				//EGL_IMAGE_PRESERVED_KHR, GL_TRUE,
				//EGL_NONE
				///All of these are required
				//EGL_WIDTH, 1920,
				//EGL_HEIGHT, 1080,
				//EGL_LINUX_DRM_FOURCC_EXT, fourcc,
				//EGL_DMA_BUF_PLANE0_FD_EXT, (EGLint)frameBuffer,
				//EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
				//EGL_DMA_BUF_PLANE0_PITCH_EXT, 0,
				//EGL_NONE
				
				//EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC601_EXT,
				//EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_FULL_RANGE_EXT,
				//EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT,
				//EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT,
			//};
			EGLint attribs[] =
			{
				EGL_WIDTH, camGL->params.width,
				EGL_HEIGHT, camGL->params.height,
				EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_YUV411,
				EGL_DMA_BUF_PLANE0_FD_EXT, (EGLint)frameBuffer,
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
				EGL_DMA_BUF_PLANE0_PITCH_EXT, camGL->params.width * 4,
				EGL_NONE
			};
			//pitches[0] = camGL->params.width;
			//pitches[1] = pitches[0];
			
			//modifiers[0] = DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(get_sand_column_pitch(camGL->params.height));
			//modifiers[1] = modifiers[0];
			
			//offsets[1] = 128 * (camGL->params.height+32);
			//offsets[2] = offsets[1] + pitches[1] * camGL->params.height;

			/*bo_handles[1] = bo_handles[2] = bo_handles[0];
					
			EGLint attribs[50];
			int j = 0;
			attribs[j++] = EGL_WIDTH;
			attribs[j++] = camGL->params.width;
			attribs[j++] = EGL_HEIGHT;
			attribs[j++] = camGL->params.height;

			attribs[j++] = EGL_LINUX_DRM_FOURCC_EXT;
			attribs[j++] = fourcc;

			attribs[j++] = EGL_DMA_BUF_PLANE0_FD_EXT;
			attribs[j++] = (EGLint)frameBuffer;

			attribs[j++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
			attribs[j++] = offsets[0];

			attribs[j++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
			attribs[j++] = pitches[0];
			
			//Not sure if I need these or not since the encoding format is I420
			if (modifiers[0]) { 
				attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT ;
				attribs[i++] = modifiers[0] & 0xFFFFFFFF;
				attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT ;
				attribs[i++] = modifiers[0] >> 32;
			}

			if (pitches[1]) {
				attribs[i++] = EGL_DMA_BUF_PLANE1_FD_EXT;
				attribs[i++] = (EGLint)frameBuffer;

				attribs[i++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
				attribs[i++] = offsets[1];

				attribs[i++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
				attribs[i++] = pitches[1];

				if (modifiers[1]) {
					attribs[i++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT ;
					attribs[i++] = modifiers[1] & 0xFFFFFFFF;
					attribs[i++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT ;
					attribs[i++] = modifiers[1] >> 32;
				}
			}

			if (pitches[2]) {
				attribs[i++] = EGL_DMA_BUF_PLANE2_FD_EXT;
				attribs[i++] = (EGLint)frameBuffer;

				attribs[i++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
				attribs[i++] = offsets[2];

				attribs[i++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
				attribs[i++] = pitches[2];

				if (modifiers[2]) {
					attribs[i++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT ;
					attribs[i++] = modifiers[2] & 0xFFFFFFFF;
					attribs[i++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT ;
					attribs[i++] = modifiers[2] >> 32;
				}
			}
			
			attribs[j++] = EGL_NONE;*/
			
			if(eglGetCurrentContext() == 0){
				printf("no egl context\n");
			}
			
			if(camGL->eglSetup.display == 0){
				printf("no egl display\n");
			}
			
			
			// Create EGL textures from frame buffers according to format
			if (camGL->params.format == CAMGL_RGB)
			{
				frameInt->eglImageRGB = eglCreateImageKHR(camGL->eglSetup.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, attribs);
				CHECK_EVAL(frameInt->eglImageRGB != EGL_NO_IMAGE_KHR, "Failed to convert frame buffer to RGB EGL image!", errorKHR);

			}
			else
			{
				frameInt->eglImageY = eglCreateImageKHR(camGL->eglSetup.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
				if(!frameInt->eglImageY){
					printf("eglCreateImageKHR: error 0x%x\n", eglGetError());
				}
				//EGLint eglerror = eglGetError();
				//if (eglerror != EGL_SUCCESS)
				//{
				//	printf("eglCreateImageKHR: error 0x%x\n", eglGetError());
				//	vcos_log_error("GL error during eglCreateImageKHR() : error 0x%04x", eglerror);
				//	goto errorKHR;
				//}
				CHECK_EVAL(frameInt->eglImageY != EGL_NO_IMAGE_KHR, "Failed to convert frame buffer to Y EGL image!", errorKHR);
				if (camGL->params.format == CAMGL_YUV)
				{
					frameInt->eglImageU = eglCreateImageKHR(camGL->eglSetup.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
					eglGetError();
					CHECK_EVAL(frameInt->eglImageU != EGL_NO_IMAGE_KHR, "Failed to convert frame buffer to U EGL image!", errorKHRU);
					frameInt->eglImageV = eglCreateImageKHR(camGL->eglSetup.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
					CHECK_EVAL(frameInt->eglImageV != EGL_NO_IMAGE_KHR, "Failed to convert frame buffer to V EGL image!", errorKHRV);
				}
			}

			// Success
			vcos_log_trace("Created EGL images format %d for buffer index %d", camGL->params.format, i);
			frameInt->mmalBufferHandle = frameBuffer;
			frameInt->format = camGL->params.format;
			CHECK_GL(camGL);
			break;

			// Handle Error
			errorKHRV:
			eglDestroyImageKHR(camGL->eglSetup.display, frameInt->eglImageU);
			errorKHRU:
			eglDestroyImageKHR(camGL->eglSetup.display, frameInt->eglImageY);
			errorKHR:
			frameInt->eglImageRGB = frameInt->eglImageY = frameInt->eglImageU = frameInt->eglImageV = NULL;
			return -1;
		}
	}

	if (i == MAX_SIMUL_FRAMES)
	{
		vcos_log_error("Exceeded configured max number of EGL images");
		return -1;
	}

	// Create frame information for client
	camGL->frame.format = frameInt->format;
	camGL->frame.width = camGL->params.width;
	camGL->frame.height = camGL->params.height;

	// Bind images to textures
	if (frameInt->format == CAMGL_RGB)
	{
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, camGL->frame.textureRGB);
		glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, frameInt->eglImageRGB);
		CHECK_GL(camGL);
	}
	else
	{
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, camGL->frame.textureY);
		CHECK_GL(camGL);
		glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, frameInt->eglImageY);
		CHECK_GL(camGL);
		if (frameInt->format == CAMGL_YUV)
		{
			glBindTexture(GL_TEXTURE_EXTERNAL_OES, camGL->frame.textureU);
			glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, frameInt->eglImageU);
			CHECK_GL(camGL);
			glBindTexture(GL_TEXTURE_EXTERNAL_OES, camGL->frame.textureV);
			glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, frameInt->eglImageV);
			CHECK_GL(camGL);
		}
	}
	return 0;
}

/* Thread-Safe state accessors */

static void camGL_setQuit(CamGL *camGL, bool error)
{
	vcos_mutex_lock(&camGL->accessMutex);
	camGL->quit = true;
	if (error) camGL->error = error;
	vcos_mutex_unlock(&camGL->accessMutex);
}
static bool camGL_getQuit(CamGL *camGL)
{
	vcos_mutex_lock(&camGL->accessMutex);
	bool quit = camGL->quit || camGL->error;
	vcos_mutex_unlock(&camGL->accessMutex);
	return quit;
}

void camGL_update_annotation(CamGL *camGL, const char *string)
{
	gcs_annotate(camGL->gcs, string);
	
} 