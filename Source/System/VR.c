// VR.C
// OpenXR support for standalone VR headsets (Meta Quest).
//
// The game renders its 3D scene once per eye into OpenXR swapchains.
// 2D elements (HUD, menus, text) go to a separate texture that is shown
// as a quad floating in front of the player.
// Touch controllers are exposed to the game as an SDL virtual gamepad.

#if __ANDROID__

#include "game.h"			// GL calls go through gl_compat.h; native GLES caps & FBO calls pass straight through
#include "vr.h"

#include <jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

/****************************/
/*    CONSTANTS             */
/****************************/

#define VR_UNITS_PER_METER		200.0f			// game world units per real-world meter
#define VR_SNAP_ANGLE			(PI / 6.0f)		// 30 degrees
#define VR_MSAA_SAMPLES			4
#define VR_MAX_SWAPCHAIN_IMAGES	4
#define VR_UI_DISTANCE			2.0f			// meters in front of the player
#define VR_UI_QUAD_WIDTH		2.6f			// meters (wider = HUD elements sit closer to the edges of view)
#define VR_UI_QUAD_Y			(-0.15f)		// meters (relative to initial head height)
#define VR_RECENTER_HOLD_SECS	1.0f			// hold right stick click this long to recenter

#define XR_CHECK(call) do {															\
		XrResult _res = (call);														\
		if (XR_FAILED(_res)) {														\
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenXR: %s failed (%d) at %s:%d",	\
					#call, _res, __FILE__, __LINE__);								\
			return false;															\
		}																			\
	} while (0)

enum
{
	kAction_Move,
	kAction_Turn,
	kAction_A,
	kAction_B,
	kAction_X,
	kAction_Y,
	kAction_Menu,
	kAction_TriggerL,
	kAction_TriggerR,
	kAction_GripL,
	kAction_GripR,
	kAction_ThumbClickL,
	kAction_ThumbClickR,
	kAction_Aim,
	kAction_COUNT
};

typedef struct
{
	XrSwapchain		handle;
	int				width;
	int				height;
	uint32_t		numImages;
	GLuint			fbo[VR_MAX_SWAPCHAIN_IMAGES];
	GLuint			depthRenderbuffer;
	uint32_t		acquiredIndex;
	bool			acquired;
} VRSwapchain;

/****************************/
/*    VARIABLES             */
/****************************/

static struct
{
	bool				active;
	jobject				activity;

	XrInstance			instance;
	XrSystemId			systemId;
	XrSession			session;
	XrSpace				baseSpace;			// raw LOCAL space (never moves, except on system recenter)
	XrSpace				localSpace;			// recentered space that everything is rendered in
	XrSpace				viewSpace;			// head pose, used to recenter
	bool				recenterPending;
	Uint64				recenterHoldStart;	// SDL ticks when right stick click went down (0 = not held)
	bool				recenterHoldDone;	// already recentered during this hold
	XrSessionState		sessionState;
	bool				sessionRunning;
	bool				hasColorScaleBias;
	bool				hasPicoControllers;
	bool				srgbSwapchain;		// swapchain stores sRGB; we write gamma-space colors into it directly

	VRSwapchain			eyes[2];
	VRSwapchain			ui;
	int					msaaSamples;

	bool				frameBegun;
	bool				shouldRender;
	XrTime				displayTime;
	XrView				views[2];
	bool				haveValidViews;

	OGLMatrix4x4		eyeWorldToView[2];
	OGLMatrix4x4		eyeViewToFrustum[2];
	float				lastYaw;

	float				rigYaw;
	bool				snapTurnPending;
	bool				turnStickCentered;

	XrActionSet			actionSet;
	XrAction			actions[kAction_COUNT];
	XrPath				handPaths[2];
	XrSpace				aimSpace;
	XrPosef				aimPose;
	bool				aimPoseValid;

	float				rigYawUsed;			// yaw & origin used for the current frame's camera
	float				rigOrigin[3];

	bool				pointerEnabled;
	float				pointerHitDistance;	// in world units; <= 0 if nothing hit

	SDL_Joystick*		virtualJoystick;
	SDL_JoystickID		virtualJoystickID;
} gVR;

static PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC		pglFramebufferTexture2DMultisampleEXT = NULL;
static PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC		pglRenderbufferStorageMultisampleEXT = NULL;

#pragma mark - Math

typedef struct { float m[3][3]; } Mat3;	// m[row][col], column-vector convention

static Mat3 Mat3_FromQuat(XrQuaternionf q)
{
	float x = q.x, y = q.y, z = q.z, w = q.w;
	Mat3 r;
	r.m[0][0] = 1 - 2*(y*y + z*z);	r.m[0][1] = 2*(x*y - w*z);		r.m[0][2] = 2*(x*z + w*y);
	r.m[1][0] = 2*(x*y + w*z);		r.m[1][1] = 1 - 2*(x*x + z*z);	r.m[1][2] = 2*(y*z - w*x);
	r.m[2][0] = 2*(x*z - w*y);		r.m[2][1] = 2*(y*z + w*x);		r.m[2][2] = 1 - 2*(x*x + y*y);
	return r;
}

static Mat3 Mat3_RotateY(float yaw)
{
	float c = cosf(yaw), s = sinf(yaw);
	Mat3 r = {{{ c, 0, s }, { 0, 1, 0 }, { -s, 0, c }}};
	return r;
}

static Mat3 Mat3_Mul(const Mat3* a, const Mat3* b)
{
	Mat3 r;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r.m[i][j] = a->m[i][0]*b->m[0][j] + a->m[i][1]*b->m[1][j] + a->m[i][2]*b->m[2][j];
	return r;
}

static void Mat3_Apply(const Mat3* a, const float v[3], float out[3])
{
	float x = v[0], y = v[1], z = v[2];
	out[0] = a->m[0][0]*x + a->m[0][1]*y + a->m[0][2]*z;
	out[1] = a->m[1][0]*x + a->m[1][1]*y + a->m[1][2]*z;
	out[2] = a->m[2][0]*x + a->m[2][1]*y + a->m[2][2]*z;
}

// World-to-view matrix (GL column-major layout, identical to OGLMatrix4x4's memory layout)
// for a camera with world orientation R and world position p.
static void MakeViewMatrix(const Mat3* R, const float p[3], OGLMatrix4x4* out)
{
	float* m = out->value;
	for (int c = 0; c < 3; c++)
	{
		for (int r = 0; r < 3; r++)
			m[c*4 + r] = R->m[c][r];		// transpose
		m[c*4 + 3] = 0;
	}
	for (int r = 0; r < 3; r++)
		m[12 + r] = -(R->m[0][r]*p[0] + R->m[1][r]*p[1] + R->m[2][r]*p[2]);
	m[15] = 1;
}

static void MakeProjectionMatrix(XrFovf fov, float nearZ, float farZ, OGLMatrix4x4* out)
{
	float tanL = tanf(fov.angleLeft);
	float tanR = tanf(fov.angleRight);
	float tanU = tanf(fov.angleUp);
	float tanD = tanf(fov.angleDown);
	float w = tanR - tanL;
	float h = tanU - tanD;

	float* m = out->value;
	SDL_memset(m, 0, sizeof(float) * 16);
	m[0]  = 2.0f / w;
	m[5]  = 2.0f / h;
	m[8]  = (tanR + tanL) / w;
	m[9]  = (tanU + tanD) / h;
	m[10] = -(farZ + nearZ) / (farZ - nearZ);
	m[11] = -1.0f;
	m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

#pragma mark - Init

static bool HasGLExtension(const char* name)
{
	const char* exts = (const char*) glGetString(GL_EXTENSIONS);
	return exts && SDL_strstr(exts, name);
}

static bool CreateSwapchain(VRSwapchain* sc, int width, int height, int64_t format, bool withDepth)
{
	XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	ci.usageFlags	= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format		= format;
	ci.sampleCount	= 1;
	ci.width		= width;
	ci.height		= height;
	ci.faceCount	= 1;
	ci.arraySize	= 1;
	ci.mipCount		= 1;
	XR_CHECK(xrCreateSwapchain(gVR.session, &ci, &sc->handle));

	sc->width = width;
	sc->height = height;

	XR_CHECK(xrEnumerateSwapchainImages(sc->handle, 0, &sc->numImages, NULL));
	if (sc->numImages > VR_MAX_SWAPCHAIN_IMAGES)
		sc->numImages = VR_MAX_SWAPCHAIN_IMAGES;

	XrSwapchainImageOpenGLESKHR images[VR_MAX_SWAPCHAIN_IMAGES];
	for (uint32_t i = 0; i < VR_MAX_SWAPCHAIN_IMAGES; i++)
		images[i] = (XrSwapchainImageOpenGLESKHR) { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR };
	XR_CHECK(xrEnumerateSwapchainImages(sc->handle, sc->numImages, &sc->numImages, (XrSwapchainImageBaseHeader*) images));

	int samples = withDepth ? gVR.msaaSamples : 1;

	if (withDepth)
	{
		glGenRenderbuffers(1, &sc->depthRenderbuffer);
		glBindRenderbuffer(GL_RENDERBUFFER, sc->depthRenderbuffer);
		if (samples > 1)
			pglRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, width, height);
		else
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	}

	for (uint32_t i = 0; i < sc->numImages; i++)
	{
		GLuint tex = images[i].image;
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		glGenFramebuffers(1, &sc->fbo[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, sc->fbo[i]);
		if (samples > 1)
			pglFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0, samples);
		else
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		if (withDepth)
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sc->depthRenderbuffer);

		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "VR: incomplete framebuffer 0x%x", status);
			return false;
		}
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

static XrPath StringToPath(const char* s)
{
	XrPath path = XR_NULL_PATH;
	xrStringToPath(gVR.instance, s, &path);
	return path;
}

static bool CreateAction(int index, const char* name, XrActionType type, bool perHand)
{
	XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
	ci.actionType = type;
	SDL_strlcpy(ci.actionName, name, sizeof(ci.actionName));
	SDL_strlcpy(ci.localizedActionName, name, sizeof(ci.localizedActionName));
	if (perHand)
	{
		ci.countSubactionPaths = 2;
		ci.subactionPaths = gVR.handPaths;
	}
	XR_CHECK(xrCreateAction(gVR.actionSet, &ci, &gVR.actions[index]));
	return true;
}

static bool InitActions(void)
{
	XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
	SDL_strlcpy(asci.actionSetName, "gameplay", sizeof(asci.actionSetName));
	SDL_strlcpy(asci.localizedActionSetName, "Gameplay", sizeof(asci.localizedActionSetName));
	XR_CHECK(xrCreateActionSet(gVR.instance, &asci, &gVR.actionSet));

	gVR.handPaths[0] = StringToPath("/user/hand/left");
	gVR.handPaths[1] = StringToPath("/user/hand/right");

	if (!CreateAction(kAction_Move,			"move",			XR_ACTION_TYPE_VECTOR2F_INPUT, false)) return false;
	if (!CreateAction(kAction_Turn,			"turn",			XR_ACTION_TYPE_VECTOR2F_INPUT, false)) return false;
	if (!CreateAction(kAction_A,			"button_a",		XR_ACTION_TYPE_BOOLEAN_INPUT, false)) return false;
	if (!CreateAction(kAction_B,			"button_b",		XR_ACTION_TYPE_BOOLEAN_INPUT, false)) return false;
	if (!CreateAction(kAction_X,			"button_x",		XR_ACTION_TYPE_BOOLEAN_INPUT, false)) return false;
	if (!CreateAction(kAction_Y,			"button_y",		XR_ACTION_TYPE_BOOLEAN_INPUT, false)) return false;
	if (!CreateAction(kAction_Menu,			"menu",			XR_ACTION_TYPE_BOOLEAN_INPUT, false)) return false;
	if (!CreateAction(kAction_TriggerL,		"trigger_left",	XR_ACTION_TYPE_FLOAT_INPUT, false)) return false;
	if (!CreateAction(kAction_TriggerR,		"trigger_right",XR_ACTION_TYPE_FLOAT_INPUT, false)) return false;
	if (!CreateAction(kAction_GripL,		"grip_left",	XR_ACTION_TYPE_FLOAT_INPUT, false)) return false;
	if (!CreateAction(kAction_GripR,		"grip_right",	XR_ACTION_TYPE_FLOAT_INPUT, false)) return false;
	if (!CreateAction(kAction_ThumbClickL,	"thumb_left",	XR_ACTION_TYPE_BOOLEAN_INPUT, false)) return false;
	if (!CreateAction(kAction_ThumbClickR,	"thumb_right",	XR_ACTION_TYPE_BOOLEAN_INPUT, false)) return false;
	if (!CreateAction(kAction_Aim,			"aim_right",	XR_ACTION_TYPE_POSE_INPUT, false)) return false;

	const struct { int action; const char* path; } kBindings[] =
	{
		{ kAction_Move,			"/user/hand/left/input/thumbstick" },
		{ kAction_Turn,			"/user/hand/right/input/thumbstick" },
		{ kAction_A,			"/user/hand/right/input/a/click" },
		{ kAction_B,			"/user/hand/right/input/b/click" },
		{ kAction_X,			"/user/hand/left/input/x/click" },
		{ kAction_Y,			"/user/hand/left/input/y/click" },
		{ kAction_Menu,			"/user/hand/left/input/menu/click" },
		{ kAction_TriggerL,		"/user/hand/left/input/trigger/value" },
		{ kAction_TriggerR,		"/user/hand/right/input/trigger/value" },
		{ kAction_GripL,		"/user/hand/left/input/squeeze/value" },
		{ kAction_GripR,		"/user/hand/right/input/squeeze/value" },
		{ kAction_ThumbClickL,	"/user/hand/left/input/thumbstick/click" },
		{ kAction_ThumbClickR,	"/user/hand/right/input/thumbstick/click" },
		{ kAction_Aim,			"/user/hand/right/input/aim/pose" },
	};
	const int numBindings = sizeof(kBindings) / sizeof(kBindings[0]);

	XrActionSuggestedBinding suggested[sizeof(kBindings) / sizeof(kBindings[0])];
	for (int i = 0; i < numBindings; i++)
	{
		suggested[i].action = gVR.actions[kBindings[i].action];
		suggested[i].binding = StringToPath(kBindings[i].path);
	}

	// Meta Touch is mandatory. Pico controllers use the same button layout, so we suggest
	// the same bindings for their profiles too (only if the runtime knows them).
	const char* kProfiles[] =
	{
		"/interaction_profiles/oculus/touch_controller",
		"/interaction_profiles/bytedance/pico4_controller",
		"/interaction_profiles/bytedance/pico_neo3_controller",
	};
	const int numProfiles = gVR.hasPicoControllers ? 3 : 1;

	for (int p = 0; p < numProfiles; p++)
	{
		XrInteractionProfileSuggestedBinding isb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		isb.interactionProfile = StringToPath(kProfiles[p]);
		isb.countSuggestedBindings = numBindings;
		isb.suggestedBindings = suggested;
		XrResult res = xrSuggestInteractionProfileBindings(gVR.instance, &isb);
		if (XR_FAILED(res))
		{
			SDL_Log("VR: bindings for %s rejected (%d)", kProfiles[p], res);
			if (p == 0)
				return false;
		}
	}

	XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach.countActionSets = 1;
	attach.actionSets = &gVR.actionSet;
	XR_CHECK(xrAttachSessionActionSets(gVR.session, &attach));

	XrActionSpaceCreateInfo asci2 = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
	asci2.action = gVR.actions[kAction_Aim];
	asci2.poseInActionSpace.orientation.w = 1;
	XR_CHECK(xrCreateActionSpace(gVR.session, &asci2, &gVR.aimSpace));

	return true;
}

static void InitVirtualGamepad(void)
{
	SDL_VirtualJoystickDesc desc;
	SDL_INIT_INTERFACE(&desc);
	desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
	desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
	desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
	desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
	desc.name = "Meta Quest Touch Controllers";

	gVR.virtualJoystickID = SDL_AttachVirtualJoystick(&desc);
	if (!gVR.virtualJoystickID)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "VR: couldn't attach virtual gamepad: %s", SDL_GetError());
		return;
	}

	gVR.virtualJoystick = SDL_OpenJoystick(gVR.virtualJoystickID);

	// Make the game use our virtual gamepad instead of the Android gamepad it may have opened already
	ForceGamepad(gVR.virtualJoystickID);
}

static bool VR_InitInternal(void)
{
	JNIEnv* env = (JNIEnv*) SDL_GetAndroidJNIEnv();
	JavaVM* vm = NULL;
	(*env)->GetJavaVM(env, &vm);
	jobject activity = (jobject) SDL_GetAndroidActivity();
	gVR.activity = (*env)->NewGlobalRef(env, activity);
	(*env)->DeleteLocalRef(env, activity);

			/* INIT LOADER */

	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = NULL;
	XR_CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*) &xrInitializeLoaderKHR));
	XrLoaderInitInfoAndroidKHR loaderInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loaderInfo.applicationVM = vm;
	loaderInfo.applicationContext = gVR.activity;
	XR_CHECK(xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR*) &loaderInfo));

			/* CHECK EXTENSIONS */

	uint32_t numExts = 0;
	XR_CHECK(xrEnumerateInstanceExtensionProperties(NULL, 0, &numExts, NULL));
	XrExtensionProperties* exts = SDL_calloc(numExts, sizeof(XrExtensionProperties));
	for (uint32_t i = 0; i < numExts; i++)
		exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	xrEnumerateInstanceExtensionProperties(NULL, numExts, &numExts, exts);
	bool hasGLES = false;
	for (uint32_t i = 0; i < numExts; i++)
	{
		if (0 == SDL_strcmp(exts[i].extensionName, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME))
			hasGLES = true;
		if (0 == SDL_strcmp(exts[i].extensionName, XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME))
			gVR.hasColorScaleBias = true;
		if (0 == SDL_strcmp(exts[i].extensionName, "XR_BD_controller_interaction"))
			gVR.hasPicoControllers = true;
	}
	SDL_free(exts);

	if (!hasGLES)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "VR: runtime lacks " XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
		return false;
	}

			/* CREATE INSTANCE */

	const char* enabledExts[4];
	uint32_t numEnabledExts = 0;
	enabledExts[numEnabledExts++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
	enabledExts[numEnabledExts++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
	if (gVR.hasColorScaleBias)
		enabledExts[numEnabledExts++] = XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME;
	if (gVR.hasPicoControllers)
		enabledExts[numEnabledExts++] = "XR_BD_controller_interaction";

	XrInstanceCreateInfoAndroidKHR androidInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidInfo.applicationVM = vm;
	androidInfo.applicationActivity = gVR.activity;

	XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
	ici.next = &androidInfo;
	SDL_strlcpy(ici.applicationInfo.applicationName, GAME_FULL_NAME, sizeof(ici.applicationInfo.applicationName));
	SDL_strlcpy(ici.applicationInfo.engineName, "Pomme", sizeof(ici.applicationInfo.engineName));
	ici.applicationInfo.applicationVersion = 1;
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = numEnabledExts;
	ici.enabledExtensionNames = enabledExts;
	XR_CHECK(xrCreateInstance(&ici, &gVR.instance));

	XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(xrGetSystem(gVR.instance, &sgi, &gVR.systemId));

			/* GRAPHICS REQUIREMENTS (mandatory call before creating a session) */

	PFN_xrGetOpenGLESGraphicsRequirementsKHR getReqs = NULL;
	XR_CHECK(xrGetInstanceProcAddr(gVR.instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction*) &getReqs));
	XrGraphicsRequirementsOpenGLESKHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	XR_CHECK(getReqs(gVR.instance, gVR.systemId, &reqs));

			/* CREATE SESSION USING SDL'S EGL CONTEXT */

	EGLDisplay display = eglGetCurrentDisplay();
	EGLContext context = eglGetCurrentContext();
	EGLint configID = 0;
	eglQueryContext(display, context, EGL_CONFIG_ID, &configID);
	EGLint configAttribs[] = { EGL_CONFIG_ID, configID, EGL_NONE };
	EGLConfig config = NULL;
	EGLint numConfigs = 0;
	eglChooseConfig(display, configAttribs, &config, 1, &numConfigs);

	XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	binding.display = display;
	binding.config = config;
	binding.context = context;

	XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
	sci.next = &binding;
	sci.systemId = gVR.systemId;
	XR_CHECK(xrCreateSession(gVR.instance, &sci, &gVR.session));

	XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation.w = 1;
	XR_CHECK(xrCreateReferenceSpace(gVR.session, &rsci, &gVR.baseSpace));
	XR_CHECK(xrCreateReferenceSpace(gVR.session, &rsci, &gVR.localSpace));

	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_CHECK(xrCreateReferenceSpace(gVR.session, &rsci, &gVR.viewSpace));

			/* SWAPCHAINS */

	if (HasGLExtension("GL_EXT_multisampled_render_to_texture"))
	{
		pglFramebufferTexture2DMultisampleEXT = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC) eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
		pglRenderbufferStorageMultisampleEXT = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC) eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
	}
	gVR.msaaSamples = (pglFramebufferTexture2DMultisampleEXT && pglRenderbufferStorageMultisampleEXT) ? VR_MSAA_SAMPLES : 1;

	uint32_t numFormats = 0;
	XR_CHECK(xrEnumerateSwapchainFormats(gVR.session, 0, &numFormats, NULL));
	int64_t* formats = SDL_calloc(numFormats, sizeof(int64_t));
	xrEnumerateSwapchainFormats(gVR.session, numFormats, &numFormats, formats);
	// The game's colors are already gamma-encoded (sRGB). An sRGB swapchain is correct, but only
	// if GL doesn't encode them a second time on write -- that double encoding washes everything out.
	// GL_EXT_sRGB_write_control lets us turn that encoding off.
	bool canDisableSRGBWrite = HasGLExtension("GL_EXT_sRGB_write_control");
	int64_t colorFormat = 0;
	for (uint32_t i = 0; i < numFormats && !colorFormat && canDisableSRGBWrite; i++)
		if (formats[i] == GL_SRGB8_ALPHA8)
			colorFormat = formats[i];
	for (uint32_t i = 0; i < numFormats && !colorFormat; i++)
		if (formats[i] == GL_RGBA8)
			colorFormat = formats[i];
	if (!colorFormat && numFormats > 0)
		colorFormat = formats[0];
	SDL_free(formats);

	gVR.srgbSwapchain = (colorFormat == GL_SRGB8_ALPHA8);

	XrViewConfigurationView viewConfigs[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
	uint32_t numViews = 0;
	XR_CHECK(xrEnumerateViewConfigurationViews(gVR.instance, gVR.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &numViews, viewConfigs));

	for (int eye = 0; eye < 2; eye++)
	{
		if (!CreateSwapchain(&gVR.eyes[eye],
				viewConfigs[eye].recommendedImageRectWidth,
				viewConfigs[eye].recommendedImageRectHeight,
				colorFormat, true))
		{
			return false;
		}
	}

	if (!CreateSwapchain(&gVR.ui, VR_UI_WIDTH, VR_UI_HEIGHT, colorFormat, false))
		return false;

	SDL_Log("VR: eye buffers %dx%d, MSAA %dx, format 0x%llx (srgb=%d), color scale/bias: %d, pico controllers: %d",
			gVR.eyes[0].width, gVR.eyes[0].height, gVR.msaaSamples, (long long) colorFormat, gVR.srgbSwapchain,
			gVR.hasColorScaleBias, gVR.hasPicoControllers);

			/* INPUT */

	if (!InitActions())
		return false;

	InitVirtualGamepad();

	gVR.views[0].type = XR_TYPE_VIEW;
	gVR.views[1].type = XR_TYPE_VIEW;
	gVR.turnStickCentered = true;

	return true;
}

bool VR_Init(void)
{
	SDL_memset(&gVR, 0, sizeof(gVR));

	gVR.active = VR_InitInternal();

	if (!gVR.active)
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "VR: initialization failed, falling back to flat mode");

	return gVR.active;
}

void VR_Shutdown(void)
{
	if (gVR.session)
		xrDestroySession(gVR.session);
	if (gVR.instance)
		xrDestroyInstance(gVR.instance);
	gVR.session = XR_NULL_HANDLE;
	gVR.instance = XR_NULL_HANDLE;
	gVR.active = false;
}

bool VR_IsActive(void)
{
	return gVR.active;
}

#pragma mark - Recenter

static void ResetRecenter(void)
{
	gVR.recenterPending = false;
	if (gVR.localSpace && gVR.localSpace != gVR.baseSpace)
		xrDestroySpace(gVR.localSpace);
	XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation.w = 1;
	if (XR_FAILED(xrCreateReferenceSpace(gVR.session, &rsci, &gVR.localSpace)))
		gVR.localSpace = gVR.baseSpace;
}

// Makes the current head position the new origin and the current head yaw the new "forward".
// Pitch and roll are ignored so the horizon stays level.
static void ApplyRecenter(XrTime time)
{
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	if (XR_FAILED(xrLocateSpace(gVR.viewSpace, gVR.baseSpace, time, &loc))
		|| !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
		|| !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
	{
		return;		// head not tracked yet -- stay pending and retry next frame
	}

	gVR.recenterPending = false;

	XrQuaternionf q = loc.pose.orientation;
	Mat3 R = Mat3_FromQuat(q);
	float yaw = atan2f(R.m[0][2], R.m[2][2]);		// heading of the head's forward vector (matches Mat3_RotateY)

	XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation = (XrQuaternionf) { 0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f) };
	rsci.poseInReferenceSpace.position = loc.pose.position;

	XrSpace newSpace = XR_NULL_HANDLE;
	if (XR_FAILED(xrCreateReferenceSpace(gVR.session, &rsci, &newSpace)))
	{
		SDL_Log("VR: recenter failed to create space");
		return;
	}

	if (gVR.localSpace && gVR.localSpace != gVR.baseSpace)
		xrDestroySpace(gVR.localSpace);
	gVR.localSpace = newSpace;

	SDL_Log("VR: recentered (yaw %.1f deg, head %.2f %.2f %.2f m)",
			yaw * 180.0f / PI, loc.pose.position.x, loc.pose.position.y, loc.pose.position.z);
}

void VR_RequestRecenter(void)
{
	gVR.recenterPending = true;
}

#pragma mark - Session events

static void PollEvents(void)
{
	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };

	while (xrPollEvent(gVR.instance, &event) == XR_SUCCESS)
	{
		switch (event.type)
		{
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			{
				const XrEventDataSessionStateChanged* e = (const XrEventDataSessionStateChanged*) &event;
				gVR.sessionState = e->state;
				SDL_Log("VR: session state %d", e->state);

				switch (e->state)
				{
					case XR_SESSION_STATE_READY:
					{
						XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
						bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
						if (XR_SUCCEEDED(xrBeginSession(gVR.session, &bi)))
						{
							gVR.sessionRunning = true;
							gVR.recenterPending = true;		// start out facing forward at the current head height
						}
						break;
					}

					case XR_SESSION_STATE_STOPPING:
						xrEndSession(gVR.session);
						gVR.sessionRunning = false;
						break;

					case XR_SESSION_STATE_EXITING:
					case XR_SESSION_STATE_LOSS_PENDING:
						gVR.sessionRunning = false;
						CleanQuit();
						break;

					default:
						break;
				}
				break;
			}

			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				CleanQuit();
				break;

			case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
				// The user recentered via the Meta button: LOCAL space already moved,
				// so drop our own offset to avoid applying it twice.
				SDL_Log("VR: system recenter, resetting our offset");
				ResetRecenter();
				break;

			default:
				break;
		}

		event = (XrEventDataBuffer) { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

#pragma mark - Input

static bool GetBool(int action)
{
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = gVR.actions[action];
	XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
	if (XR_FAILED(xrGetActionStateBoolean(gVR.session, &gi, &s)))
		return false;
	return s.isActive && s.currentState;
}

static float GetFloat(int action)
{
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = gVR.actions[action];
	XrActionStateFloat s = { XR_TYPE_ACTION_STATE_FLOAT };
	if (XR_FAILED(xrGetActionStateFloat(gVR.session, &gi, &s)) || !s.isActive)
		return 0;
	return s.currentState;
}

static XrVector2f GetVec2(int action)
{
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = gVR.actions[action];
	XrActionStateVector2f s = { XR_TYPE_ACTION_STATE_VECTOR2F };
	if (XR_FAILED(xrGetActionStateVector2f(gVR.session, &gi, &s)) || !s.isActive)
		return (XrVector2f) { 0, 0 };
	return s.currentState;
}

static Sint16 AxisToSint16(float v)
{
	if (v > 1) v = 1;
	if (v < -1) v = -1;
	return (Sint16) (v * 32767.0f);
}

static void PollActions(void)
{
	bool focused = gVR.sessionState == XR_SESSION_STATE_FOCUSED;

	if (focused)
	{
		XrActiveActionSet active = { gVR.actionSet, XR_NULL_PATH };
		XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
		si.countActiveActionSets = 1;
		si.activeActionSets = &active;
		if (XR_FAILED(xrSyncActions(gVR.session, &si)))
			focused = false;
	}

	XrVector2f move		= focused ? GetVec2(kAction_Move) : (XrVector2f) {0,0};
	XrVector2f turn		= focused ? GetVec2(kAction_Turn) : (XrVector2f) {0,0};
	float trigL			= focused ? GetFloat(kAction_TriggerL) : 0;
	float trigR			= focused ? GetFloat(kAction_TriggerR) : 0;
	float gripL			= focused ? GetFloat(kAction_GripL) : 0;
	float gripR			= focused ? GetFloat(kAction_GripR) : 0;
	bool a				= focused && GetBool(kAction_A);
	bool b				= focused && GetBool(kAction_B);
	bool x				= focused && GetBool(kAction_X);
	bool y				= focused && GetBool(kAction_Y);
	bool menu			= focused && GetBool(kAction_Menu);
	bool thumbL			= focused && GetBool(kAction_ThumbClickL);
	bool thumbR			= focused && GetBool(kAction_ThumbClickR);

			/* SNAP TURN */

	if (fabsf(turn.x) < 0.3f)
	{
		gVR.turnStickCentered = true;
	}
	else if (gVR.turnStickCentered && fabsf(turn.x) > 0.7f)
	{
		gVR.turnStickCentered = false;
		gVR.rigYaw += (turn.x > 0) ? -VR_SNAP_ANGLE : VR_SNAP_ANGLE;
		gVR.snapTurnPending = true;
	}

			/* HOLD RIGHT STICK CLICK TO RECENTER */

	if (thumbR)
	{
		if (!gVR.recenterHoldStart)
			gVR.recenterHoldStart = SDL_GetTicks();
		else if (!gVR.recenterHoldDone && (SDL_GetTicks() - gVR.recenterHoldStart) >= (Uint64) (VR_RECENTER_HOLD_SECS * 1000))
		{
			gVR.recenterHoldDone = true;
			gVR.recenterPending = true;
		}
	}
	else
	{
		gVR.recenterHoldStart = 0;
		gVR.recenterHoldDone = false;
	}

			/* FEED VIRTUAL GAMEPAD */

	SDL_Joystick* joy = gVR.virtualJoystick;
	if (!joy)
		return;

	SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, AxisToSint16(move.x));
	SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, AxisToSint16(-move.y));		// SDL: +Y is down
	SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHTX, 0);							// right stick X is reserved for snap turns
	SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHTY, AxisToSint16(-turn.y));
	SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, AxisToSint16(trigL));
	SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, AxisToSint16(trigR));

	// Bugdom 2 default gamepad bindings (InputDefaults.c)
	SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH,				a);					// jump / confirm
	SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_EAST,				b);					// pick up / drop, back
	SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST,				x || trigR > 0.5f);	// kick
	SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_NORTH,				y);					// launch buddy
	SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_START,				menu);				// pause
	SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,		gripL > 0.5f);		// menu: previous
	SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,	gripR > 0.5f);		// menu: next
	SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_RIGHT_STICK,		thumbL);			// camera distance (right stick click is recenter)
}

float VR_GetRigYaw(void)
{
	return gVR.rigYaw;
}

void VR_SetRigYaw(float yaw)
{
	gVR.rigYaw = yaw;
}

bool VR_ConsumeSnapTurn(void)
{
	bool pending = gVR.snapTurnPending;
	gVR.snapTurnPending = false;
	return pending;
}

#pragma mark - Frame loop

static bool AcquireSwapchain(VRSwapchain* sc)
{
	XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XR_CHECK(xrAcquireSwapchainImage(sc->handle, &ai, &sc->acquiredIndex));

	XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wi.timeout = XR_INFINITE_DURATION;
	XR_CHECK(xrWaitSwapchainImage(sc->handle, &wi));

	sc->acquired = true;
	return true;
}

static void ReleaseSwapchain(VRSwapchain* sc)
{
	if (!sc->acquired)
		return;
	XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(sc->handle, &ri);
	sc->acquired = false;
}

bool VR_BeginFrame(void)
{
	GAME_ASSERT(!gVR.frameBegun);

	PollEvents();

	if (!gVR.sessionRunning)
	{
		SDL_Delay(20);		// don't spin while the headset is idle
		return false;
	}

	XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState frameState = { XR_TYPE_FRAME_STATE };
	if (XR_FAILED(xrWaitFrame(gVR.session, &fwi, &frameState)))
		return false;

	XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
	if (XR_FAILED(xrBeginFrame(gVR.session, &fbi)))
		return false;

	gVR.frameBegun = true;
	gVR.displayTime = frameState.predictedDisplayTime;
	gVR.shouldRender = frameState.shouldRender;

	PollActions();

	if (gVR.recenterPending)
		ApplyRecenter(gVR.displayTime);

	if (!gVR.shouldRender)
		return false;

	XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
	vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	vli.displayTime = gVR.displayTime;
	vli.space = gVR.localSpace;
	XrViewState viewState = { XR_TYPE_VIEW_STATE };
	XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	uint32_t numViews = 0;
	if (XR_SUCCEEDED(xrLocateViews(gVR.session, &vli, &viewState, 2, &numViews, views))
		&& (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)
		&& numViews == 2)
	{
		gVR.views[0] = views[0];
		gVR.views[1] = views[1];
		gVR.haveValidViews = true;
	}

	XrSpaceLocation aimLoc = { XR_TYPE_SPACE_LOCATION };
	gVR.aimPoseValid = XR_SUCCEEDED(xrLocateSpace(gVR.aimSpace, gVR.localSpace, gVR.displayTime, &aimLoc))
			&& (aimLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
			&& (aimLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
	if (gVR.aimPoseValid)
		gVR.aimPose = aimLoc.pose;

	if (!gVR.haveValidViews)
	{
		gVR.shouldRender = false;
		return false;
	}

	if (!AcquireSwapchain(&gVR.eyes[0]) || !AcquireSwapchain(&gVR.eyes[1]) || !AcquireSwapchain(&gVR.ui))
	{
		gVR.shouldRender = false;
		return false;
	}

	return true;
}

void VR_EndFrame(float fadeFactor)
{
	if (!gVR.frameBegun)
		return;

	XrCompositionLayerProjectionView projViews[2];
	XrCompositionLayerProjection projLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad quadLayers[1];
	XrCompositionLayerColorScaleBiasKHR fadeProj = { XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR };
	XrCompositionLayerColorScaleBiasKHR fadeQuad = { XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR };
	const XrCompositionLayerBaseHeader* layers[2];
	uint32_t numLayers = 0;

	if (gVR.shouldRender)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		ReleaseSwapchain(&gVR.eyes[0]);
		ReleaseSwapchain(&gVR.eyes[1]);
		ReleaseSwapchain(&gVR.ui);

		if (fadeFactor < 0) fadeFactor = 0;
		if (fadeFactor > 1) fadeFactor = 1;

		for (int eye = 0; eye < 2; eye++)
		{
			projViews[eye] = (XrCompositionLayerProjectionView) { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
			projViews[eye].pose = gVR.views[eye].pose;
			projViews[eye].fov = gVR.views[eye].fov;
			projViews[eye].subImage.swapchain = gVR.eyes[eye].handle;
			projViews[eye].subImage.imageRect.offset = (XrOffset2Di) { 0, 0 };
			projViews[eye].subImage.imageRect.extent = (XrExtent2Di) { gVR.eyes[eye].width, gVR.eyes[eye].height };
		}

		projLayer.space = gVR.localSpace;
		projLayer.viewCount = 2;
		projLayer.views = projViews;

		if (gVR.hasColorScaleBias && fadeFactor < 1)
		{
			fadeProj.colorScale = (XrColor4f) { fadeFactor, fadeFactor, fadeFactor, 1 };
			fadeQuad.colorScale = (XrColor4f) { fadeFactor, fadeFactor, fadeFactor, fadeFactor };
			projLayer.next = &fadeProj;
		}

		layers[numLayers++] = (const XrCompositionLayerBaseHeader*) &projLayer;

		// HUD layer: one square quad. Bugdom 2 anchors its infobar to the edges of the 2D area,
		// so on a square layer the top/bottom infobars sit well above/below the middle of the view,
		// while menus (laid out in the central 640x480) stay where they were.
		XrCompositionLayerQuad* q = &quadLayers[0];
		*q = (XrCompositionLayerQuad) { XR_TYPE_COMPOSITION_LAYER_QUAD };
		q->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;		// premultiplied alpha
		q->space = gVR.localSpace;
		q->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		q->subImage.swapchain = gVR.ui.handle;
		q->subImage.imageRect.extent = (XrExtent2Di) { gVR.ui.width, gVR.ui.height };
		q->pose.orientation.w = 1;
		q->pose.position = (XrVector3f) { 0, VR_UI_QUAD_Y, -VR_UI_DISTANCE };
		q->size = (XrExtent2Df) { VR_UI_QUAD_WIDTH, VR_UI_QUAD_WIDTH * VR_UI_HEIGHT / (float) VR_UI_WIDTH };
		if (gVR.hasColorScaleBias && fadeFactor < 1)
			q->next = &fadeQuad;
		layers[numLayers++] = (const XrCompositionLayerBaseHeader*) q;
	}

	XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
	fei.displayTime = gVR.displayTime;
	fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	fei.layerCount = numLayers;
	fei.layers = layers;
	xrEndFrame(gVR.session, &fei);

	gVR.frameBegun = false;
	gVR.shouldRender = false;
}

#pragma mark - Render targets

int VR_GetEyeCount(void)
{
	return 2;
}

static void BindTarget(const VRSwapchain* sc)
{
	glBindFramebuffer(GL_FRAMEBUFFER, sc->fbo[sc->acquiredIndex]);
	glViewport(0, 0, sc->width, sc->height);
	if (gVR.srgbSwapchain)
		glDisable(GL_FRAMEBUFFER_SRGB_EXT);		// store gamma-space colors as-is (no double gamma)
}

void VR_BindEyeTarget(int eye)
{
	BindTarget(&gVR.eyes[eye]);
}

void VR_BindUITarget(void)
{
	BindTarget(&gVR.ui);
}

void VR_ClearTargets(void)
{
	GLfloat clearColor[4];
	glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);

	for (int eye = 0; eye < 2; eye++)
	{
		BindTarget(&gVR.eyes[eye]);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}

	BindTarget(&gVR.ui);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
}

#pragma mark - Camera

void VR_ComputeCameraMatrices(
		const OGLPoint3D* from,
		const OGLPoint3D* to,
		float hither,
		float yon,
		bool useRigYaw,
		OGLMatrix4x4* centerWorldToView,
		OGLMatrix4x4* centerViewToFrustum)
{
			/* DETERMINE YAW OF THE GAME CAMERA (IGNORE PITCH/ROLL -- KEEP HORIZON LEVEL) */

	float yaw;
	if (useRigYaw)
	{
		yaw = gVR.rigYaw;
	}
	else
	{
		float dx = to->x - from->x;
		float dz = to->z - from->z;
		if (dx*dx + dz*dz > 1e-4f)
			yaw = atan2f(-dx, -dz);					// forward (0,0,-1) rotated by yaw == (dx,dz)
		else
			yaw = gVR.lastYaw;
	}
	gVR.lastYaw = yaw;

	Mat3 rigRot = Mat3_RotateY(yaw);
	const float origin[3] = { from->x, from->y, from->z };

	gVR.rigYawUsed = yaw;
	gVR.rigOrigin[0] = origin[0];
	gVR.rigOrigin[1] = origin[1];
	gVR.rigOrigin[2] = origin[2];

			/* PER-EYE MATRICES */

	float centerPos[3] = { 0, 0, 0 };
	Mat3 centerRot = rigRot;

	for (int eye = 0; eye < 2; eye++)
	{
		const XrPosef* pose = &gVR.views[eye].pose;

		Mat3 headRot = Mat3_FromQuat(pose->orientation);
		Mat3 eyeRot = Mat3_Mul(&rigRot, &headRot);

		float localPos[3] =
		{
			pose->position.x * VR_UNITS_PER_METER,
			pose->position.y * VR_UNITS_PER_METER,
			pose->position.z * VR_UNITS_PER_METER,
		};
		float worldOffset[3];
		Mat3_Apply(&rigRot, localPos, worldOffset);

		float eyePos[3] = { origin[0] + worldOffset[0], origin[1] + worldOffset[1], origin[2] + worldOffset[2] };

		MakeViewMatrix(&eyeRot, eyePos, &gVR.eyeWorldToView[eye]);
		MakeProjectionMatrix(gVR.views[eye].fov, hither, yon, &gVR.eyeViewToFrustum[eye]);

		centerPos[0] += eyePos[0] * 0.5f;
		centerPos[1] += eyePos[1] * 0.5f;
		centerPos[2] += eyePos[2] * 0.5f;
		if (eye == 0)
			centerRot = eyeRot;
	}

			/* CENTER EYE: UNION OF BOTH EYES' FIELDS OF VIEW (FOR CULLING & SORTING) */

	XrFovf fov;
	fov.angleLeft	= SDL_min(gVR.views[0].fov.angleLeft,	gVR.views[1].fov.angleLeft);
	fov.angleRight	= SDL_max(gVR.views[0].fov.angleRight,	gVR.views[1].fov.angleRight);
	fov.angleUp		= SDL_max(gVR.views[0].fov.angleUp,		gVR.views[1].fov.angleUp);
	fov.angleDown	= SDL_min(gVR.views[0].fov.angleDown,	gVR.views[1].fov.angleDown);

	MakeViewMatrix(&centerRot, centerPos, centerWorldToView);
	MakeProjectionMatrix(fov, hither, yon, centerViewToFrustum);
}

const OGLMatrix4x4* VR_GetEyeWorldToView(int eye)
{
	return &gVR.eyeWorldToView[eye];
}

const OGLMatrix4x4* VR_GetEyeViewToFrustum(int eye)
{
	return &gVR.eyeViewToFrustum[eye];
}

#pragma mark - Draw passes

static int gVRDrawPass = VR_PASS_NONE;

void VR_SetDrawPass(int pass)
{
	gVRDrawPass = pass;
}

int VR_GetDrawPass(void)
{
	return gVRDrawPass;
}

#endif

#if __ANDROID__
#pragma mark - Laser pointer

void VR_SetPointerEnabled(bool enabled)
{
	gVR.pointerEnabled = enabled;
	gVR.pointerHitDistance = 0;
}

bool VR_GetPointerRay(OGLPoint3D* outOrigin, OGLVector3D* outDir)
{
	if (!gVR.active || !gVR.aimPoseValid)
		return false;

	Mat3 rigRot = Mat3_RotateY(gVR.rigYawUsed);
	Mat3 handRot = Mat3_FromQuat(gVR.aimPose.orientation);
	Mat3 worldRot = Mat3_Mul(&rigRot, &handRot);

	float localPos[3] =
	{
		gVR.aimPose.position.x * VR_UNITS_PER_METER,
		gVR.aimPose.position.y * VR_UNITS_PER_METER,
		gVR.aimPose.position.z * VR_UNITS_PER_METER,
	};
	float worldOffset[3];
	Mat3_Apply(&rigRot, localPos, worldOffset);

	const float forward[3] = { 0, 0, -1 };
	float dir[3];
	Mat3_Apply(&worldRot, forward, dir);

	*outOrigin = (OGLPoint3D) { gVR.rigOrigin[0] + worldOffset[0], gVR.rigOrigin[1] + worldOffset[1], gVR.rigOrigin[2] + worldOffset[2] };
	*outDir = (OGLVector3D) { dir[0], dir[1], dir[2] };
	return true;
}

void VR_SetPointerHitDistance(float distance)
{
	gVR.pointerHitDistance = distance;
}

// Draws the laser beam into the currently-bound eye target (called by the renderer for each eye)
void VR_DrawPointer(void)
{
	if (!gVR.pointerEnabled)
		return;

	OGLPoint3D o;
	OGLVector3D d;
	if (!VR_GetPointerRay(&o, &d))
		return;

	bool hit = gVR.pointerHitDistance > 0;
	float len = hit ? gVR.pointerHitDistance : 4.0f * VR_UNITS_PER_METER;
	OGLPoint3D e = { o.x + d.x * len, o.y + d.y * len, o.z + d.z * len };

	glDisable(GL_DEPTH_TEST);

	if (hit)
		GLC_Color4f(1.0f, 0.9f, 0.2f, 1.0f);		// yellow when pointing at something clickable
	else
		GLC_Color4f(0.6f, 0.8f, 1.0f, 1.0f);

	GLC_Begin(GL_LINES);
	GLC_Vertex3f(o.x, o.y, o.z);
	GLC_Vertex3f(e.x, e.y, e.z);

	// Little 3D cross at the tip
	float c = 0.02f * VR_UNITS_PER_METER;
	GLC_Vertex3f(e.x - c, e.y, e.z);	GLC_Vertex3f(e.x + c, e.y, e.z);
	GLC_Vertex3f(e.x, e.y - c, e.z);	GLC_Vertex3f(e.x, e.y + c, e.z);
	GLC_Vertex3f(e.x, e.y, e.z - c);	GLC_Vertex3f(e.x, e.y, e.z + c);
	GLC_End();

	glEnable(GL_DEPTH_TEST);
}
#endif
