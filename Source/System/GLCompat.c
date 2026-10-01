// GLCOMPAT.C
// Emulation of the OpenGL 1.x fixed-function pipeline on top of OpenGL ES 3,
// covering the subset of features that Bugdom 2 uses:
//   - modelview / projection / texture matrix stacks
//   - per-vertex lighting (directional lights, color material)
//   - two texture units with MODULATE / COMBINE(ADD) environments
//   - sphere-map texture coordinate generation (reflections)
//   - linear/exp fog, alpha test
//   - immediate mode (glBegin/glEnd, incl. GL_QUADS)
// Only compiled on Android (or on desktop with GLC_FORCE_GLES for testing).

#define GLC_IMPLEMENTATION		// this file calls the real GLES functions
#include "gl_compat.h"

#if GLC_ENABLED

#include <SDL3/SDL.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define NUM_UNITS				2
#define MAX_LIGHTS				4
#define MODELVIEW_STACK_DEPTH	32
#define PROJECTION_STACK_DEPTH	8
#define TEXTURE_STACK_DEPTH		4
#define MAX_IMMEDIATE_VERTICES	8192

#define GLC_FATAL(...) do { SDL_LogError(SDL_LOG_CATEGORY_RENDER, __VA_ARGS__); SDL_TriggerBreakpoint(); abort(); } while (0)

enum
{
	kAttrib_Position	= 0,
	kAttrib_Normal		= 1,
	kAttrib_Color		= 2,
	kAttrib_UV0			= 3,
	kAttrib_UV1			= 4,
	kNumAttribs
};

enum	// texture environment codes passed to the shader
{
	kEnv_Modulate		= 0,
	kEnv_AddRGB_ModA	= 1,
	kEnv_AddRGB_AddA	= 2,
	kEnv_Replace		= 3,
	kEnv_Decal			= 4,
};

typedef struct
{
	bool		enabled;
	GLint		size;
	GLenum		type;
	GLsizei		stride;
	const void*	ptr;
} ClientArray;

typedef struct
{
	bool		texture2D;
	bool		texGenS;
	bool		texGenT;
	GLenum		envMode;
	GLenum		combineRGB;
	GLenum		combineAlpha;
	GLfloat		matrix[TEXTURE_STACK_DEPTH][16];
	int			matrixTop;
	ClientArray	uvArray;
	GLfloat		currentUV[2];
} TexUnit;

typedef struct
{
	GLfloat		pos[3];
	GLfloat		uv[2];
	GLfloat		color[4];
	GLfloat		normal[3];
} ImmediateVertex;

static struct
{
	bool		initialized;

	GLenum		matrixMode;
	GLfloat		modelview[MODELVIEW_STACK_DEPTH][16];
	GLfloat		projection[PROJECTION_STACK_DEPTH][16];
	int			modelviewTop;
	int			projectionTop;

	GLfloat		color[4];
	GLfloat		normal[3];
	GLfloat		materialDiffuse[4];

	bool		lighting;
	bool		colorMaterial;
	bool		normalize;
	bool		rescaleNormal;
	bool		alphaTest;
	bool		fog;
	bool		lightEnabled[MAX_LIGHTS];
	GLfloat		lightEyeDir[MAX_LIGHTS][3];
	GLfloat		lightDiffuse[MAX_LIGHTS][3];
	GLfloat		lightAmbient[MAX_LIGHTS][3];
	GLfloat		sceneAmbient[3];

	GLenum		fogMode;
	GLfloat		fogStart;
	GLfloat		fogEnd;
	GLfloat		fogDensity;
	GLfloat		fogColor[3];

	GLenum		alphaFunc;
	GLfloat		alphaRef;

	GLenum		blendSrc;
	GLenum		blendDst;

	int			activeUnit;
	int			clientActiveUnit;
	TexUnit		units[NUM_UNITS];

	ClientArray	posArray;
	ClientArray	normalArray;
	ClientArray	colorArray;

	GLuint		program;
	GLint		uMVP, uMV, uNormalMat;
	GLint		uLighting, uNumLights, uLightDir, uLightDiffuse, uLightAmbientSum, uSceneAmbient;
	GLint		uTexEnabled[NUM_UNITS], uTexEnv[NUM_UNITS], uTexGen[NUM_UNITS], uTexMat[NUM_UNITS], uSampler[NUM_UNITS];
	GLint		uAlphaFunc, uAlphaRef;
	GLint		uFogMode, uFogParams, uFogColor;

	GLenum		immediateMode;
	int			immediateCount;
	ImmediateVertex	immediateVerts[MAX_IMMEDIATE_VERTICES];

	uint8_t*	textureOpaqueFlags;			// indexed by texture name: 1 if the game asked for an RGB (alpha-less) texture
	size_t		textureOpaqueFlagsCapacity;

	uint8_t*	convertBuffer;
	size_t		convertBufferSize;
} gGLC;

#pragma mark - Shaders

static const char* kVertexShader =
	"#version 300 es\n"
	"layout(location=0) in vec3 aPos;\n"
	"layout(location=1) in vec3 aNormal;\n"
	"layout(location=2) in vec4 aColor;\n"
	"layout(location=3) in vec2 aUV0;\n"
	"layout(location=4) in vec2 aUV1;\n"
	"uniform mat4 uMVP;\n"
	"uniform mat4 uMV;\n"
	"uniform mat3 uNormalMat;\n"
	"uniform bool uLighting;\n"
	"uniform int uNumLights;\n"
	"uniform vec3 uLightDir[4];\n"
	"uniform vec3 uLightDiffuse[4];\n"
	"uniform vec3 uLightAmbientSum;\n"
	"uniform vec3 uSceneAmbient;\n"
	"uniform bool uTexGen0;\n"
	"uniform bool uTexGen1;\n"
	"uniform mat4 uTexMat0;\n"
	"uniform mat4 uTexMat1;\n"
	"out vec4 vColor;\n"
	"out vec2 vUV0;\n"
	"out vec2 vUV1;\n"
	"out float vEyeDist;\n"
	"vec2 SphereMap(vec3 eyePos, vec3 n) {\n"
	"	vec3 u = normalize(eyePos);\n"
	"	vec3 r = reflect(u, n);\n"
	"	float m = 2.0 * sqrt(r.x*r.x + r.y*r.y + (r.z+1.0)*(r.z+1.0));\n"
	"	return vec2(r.x/m + 0.5, r.y/m + 0.5);\n"
	"}\n"
	"void main() {\n"
	"	vec4 eyePos = uMV * vec4(aPos, 1.0);\n"
	"	gl_Position = uMVP * vec4(aPos, 1.0);\n"
	"	vec3 n = normalize(uNormalMat * aNormal);\n"
	"	vec4 c = aColor;\n"
	"	if (uLighting) {\n"
	"		vec3 lit = (uSceneAmbient + uLightAmbientSum) * c.rgb;\n"
	"		for (int i = 0; i < uNumLights; i++)\n"
	"			lit += max(dot(n, uLightDir[i]), 0.0) * uLightDiffuse[i] * c.rgb;\n"
	"		c.rgb = lit;\n"
	"	}\n"
	"	vColor = clamp(c, 0.0, 1.0);\n"
	"	vec2 uv0 = uTexGen0 ? SphereMap(eyePos.xyz, n) : aUV0;\n"
	"	vec2 uv1 = uTexGen1 ? SphereMap(eyePos.xyz, n) : aUV1;\n"
	"	vUV0 = (uTexMat0 * vec4(uv0, 0.0, 1.0)).xy;\n"
	"	vUV1 = (uTexMat1 * vec4(uv1, 0.0, 1.0)).xy;\n"
	"	vEyeDist = abs(eyePos.z);\n"
	"}\n";

static const char* kFragmentShader =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec4 vColor;\n"
	"in highp vec2 vUV0;\n"
	"in highp vec2 vUV1;\n"
	"in highp float vEyeDist;\n"
	"uniform sampler2D uSampler0;\n"
	"uniform sampler2D uSampler1;\n"
	"uniform bool uTex0;\n"
	"uniform bool uTex1;\n"
	"uniform int uTexEnv0;\n"
	"uniform int uTexEnv1;\n"
	"uniform int uAlphaFunc;\n"		// 0 = off, 1 = GREATER, 2 = NOTEQUAL, 3 = EQUAL, 4 = GEQUAL, 5 = LESS, 6 = LEQUAL
	"uniform float uAlphaRef;\n"
	"uniform int uFogMode;\n"		// 0 = off, 1 = linear, 2 = exp, 3 = exp2
	"uniform highp vec3 uFogParams;\n"	// start, end, density
	"uniform vec3 uFogColor;\n"
	"out vec4 fragColor;\n"
	"vec4 Combine(vec4 prev, vec4 t, int env) {\n"
	"	if (env == 0) return prev * t;\n"
	"	if (env == 1) return vec4(min(prev.rgb + t.rgb, 1.0), prev.a * t.a);\n"
	"	if (env == 2) return min(prev + t, 1.0);\n"
	"	if (env == 3) return t;\n"
	"	return vec4(mix(prev.rgb, t.rgb, t.a), prev.a);\n"
	"}\n"
	"void main() {\n"
	"	vec4 c = vColor;\n"
	"	if (uTex0) c = Combine(c, texture(uSampler0, vUV0), uTexEnv0);\n"
	"	if (uTex1) c = Combine(c, texture(uSampler1, vUV1), uTexEnv1);\n"
	"	if (uAlphaFunc != 0) {\n"
	"		float eps = 0.5 / 255.0;\n"
	"		bool pass = true;\n"
	"		if (uAlphaFunc == 1) pass = c.a > uAlphaRef;\n"
	"		else if (uAlphaFunc == 2) pass = abs(c.a - uAlphaRef) > eps;\n"
	"		else if (uAlphaFunc == 3) pass = abs(c.a - uAlphaRef) <= eps;\n"
	"		else if (uAlphaFunc == 4) pass = c.a >= uAlphaRef - eps;\n"
	"		else if (uAlphaFunc == 5) pass = c.a < uAlphaRef;\n"
	"		else if (uAlphaFunc == 6) pass = c.a <= uAlphaRef + eps;\n"
	"		if (!pass) discard;\n"
	"	}\n"
	"	if (uFogMode != 0) {\n"
	"		float f;\n"
	"		if (uFogMode == 1) f = (uFogParams.y - vEyeDist) / (uFogParams.y - uFogParams.x);\n"
	"		else if (uFogMode == 2) f = exp(-uFogParams.z * vEyeDist);\n"
	"		else f = exp(-(uFogParams.z * vEyeDist) * (uFogParams.z * vEyeDist));\n"
	"		c.rgb = mix(uFogColor, c.rgb, clamp(f, 0.0, 1.0));\n"
	"	}\n"
	"	fragColor = c;\n"
	"}\n";

static GLuint CompileShader(GLenum type, const char* source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[2048];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		GLC_FATAL("GLCompat shader compile error: %s", log);
	}
	return shader;
}

static void Mat4Identity(GLfloat* m)
{
	SDL_memset(m, 0, sizeof(GLfloat)*16);
	m[0] = m[5] = m[10] = m[15] = 1;
}

void GLC_Init(void)
{
	// Reset all state, but keep the texture-flag & conversion buffers (they survive context re-creation)
	uint8_t* keepFlags = gGLC.textureOpaqueFlags;
	size_t keepFlagsCap = gGLC.textureOpaqueFlagsCapacity;
	uint8_t* keepConvert = gGLC.convertBuffer;
	size_t keepConvertSize = gGLC.convertBufferSize;
	SDL_memset(&gGLC, 0, sizeof(gGLC));
	gGLC.textureOpaqueFlags = keepFlags;
	gGLC.textureOpaqueFlagsCapacity = keepFlagsCap;
	gGLC.convertBuffer = keepConvert;
	gGLC.convertBufferSize = keepConvertSize;

	gGLC.matrixMode = GL_MODELVIEW;
	Mat4Identity(gGLC.modelview[0]);
	Mat4Identity(gGLC.projection[0]);

	gGLC.color[0] = gGLC.color[1] = gGLC.color[2] = gGLC.color[3] = 1;
	gGLC.normal[2] = 1;
	gGLC.materialDiffuse[0] = gGLC.materialDiffuse[1] = gGLC.materialDiffuse[2] = 0.8f;
	gGLC.materialDiffuse[3] = 1;
	gGLC.sceneAmbient[0] = gGLC.sceneAmbient[1] = gGLC.sceneAmbient[2] = 0.2f;
	gGLC.normalize = false;

	for (int i = 0; i < MAX_LIGHTS; i++)
	{
		gGLC.lightEyeDir[i][2] = 1;
		gGLC.lightDiffuse[i][0] = gGLC.lightDiffuse[i][1] = gGLC.lightDiffuse[i][2] = (i == 0) ? 1 : 0;
	}

	gGLC.fogMode = GL_EXP;
	gGLC.fogStart = 0;
	gGLC.fogEnd = 1;
	gGLC.fogDensity = 1;
	gGLC.alphaFunc = GL_ALWAYS;
	gGLC.blendSrc = GL_ONE;
	gGLC.blendDst = GL_ZERO;

	for (int u = 0; u < NUM_UNITS; u++)
	{
		TexUnit* tu = &gGLC.units[u];
		tu->envMode = GL_MODULATE;
		tu->combineRGB = GL_MODULATE;
		tu->combineAlpha = GL_MODULATE;
		Mat4Identity(tu->matrix[0]);
	}

	// (Re)create shader program -- the GL context may have been recreated
	GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
	GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
	gGLC.program = glCreateProgram();
	glAttachShader(gGLC.program, vs);
	glAttachShader(gGLC.program, fs);
	glLinkProgram(gGLC.program);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(gGLC.program, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		char log[2048];
		glGetProgramInfoLog(gGLC.program, sizeof(log), NULL, log);
		GLC_FATAL("GLCompat program link error: %s", log);
	}

#define LOC(field, name) gGLC.field = glGetUniformLocation(gGLC.program, name)
	LOC(uMVP,				"uMVP");
	LOC(uMV,				"uMV");
	LOC(uNormalMat,			"uNormalMat");
	LOC(uLighting,			"uLighting");
	LOC(uNumLights,			"uNumLights");
	LOC(uLightDir,			"uLightDir");
	LOC(uLightDiffuse,		"uLightDiffuse");
	LOC(uLightAmbientSum,	"uLightAmbientSum");
	LOC(uSceneAmbient,		"uSceneAmbient");
	LOC(uTexEnabled[0],		"uTex0");
	LOC(uTexEnabled[1],		"uTex1");
	LOC(uTexEnv[0],			"uTexEnv0");
	LOC(uTexEnv[1],			"uTexEnv1");
	LOC(uTexGen[0],			"uTexGen0");
	LOC(uTexGen[1],			"uTexGen1");
	LOC(uTexMat[0],			"uTexMat0");
	LOC(uTexMat[1],			"uTexMat1");
	LOC(uSampler[0],		"uSampler0");
	LOC(uSampler[1],		"uSampler1");
	LOC(uAlphaFunc,			"uAlphaFunc");
	LOC(uAlphaRef,			"uAlphaRef");
	LOC(uFogMode,			"uFogMode");
	LOC(uFogParams,			"uFogParams");
	LOC(uFogColor,			"uFogColor");
#undef LOC

	glUseProgram(gGLC.program);
	glUniform1i(gGLC.uSampler[0], 0);
	glUniform1i(gGLC.uSampler[1], 1);

	glActiveTexture(GL_TEXTURE0);

	gGLC.initialized = true;
}

#pragma mark - Matrices

static void Mat4Mul(const GLfloat* a, const GLfloat* b, GLfloat* out)		// out = a * b (column-major)
{
	GLfloat r[16];
	for (int c = 0; c < 4; c++)
	{
		for (int row = 0; row < 4; row++)
		{
			r[c*4+row] =  a[0*4+row] * b[c*4+0]
						+ a[1*4+row] * b[c*4+1]
						+ a[2*4+row] * b[c*4+2]
						+ a[3*4+row] * b[c*4+3];
		}
	}
	SDL_memcpy(out, r, sizeof(r));
}

static GLfloat* CurrentMatrix(void)
{
	switch (gGLC.matrixMode)
	{
		case GL_PROJECTION:
			return gGLC.projection[gGLC.projectionTop];
		case GL_TEXTURE:
		{
			TexUnit* tu = &gGLC.units[gGLC.activeUnit];
			return tu->matrix[tu->matrixTop];
		}
		default:
			return gGLC.modelview[gGLC.modelviewTop];
	}
}

const GLfloat* GLC_GetMatrix(GLenum mode)
{
	GLenum saved = gGLC.matrixMode;
	gGLC.matrixMode = mode;
	const GLfloat* m = CurrentMatrix();
	gGLC.matrixMode = saved;
	return m;
}

void GLC_MatrixMode(GLenum mode)
{
	gGLC.matrixMode = mode;
}

void GLC_PushMatrix(void)
{
	GLfloat* src = CurrentMatrix();
	switch (gGLC.matrixMode)
	{
		case GL_PROJECTION:
			if (gGLC.projectionTop + 1 >= PROJECTION_STACK_DEPTH) GLC_FATAL("GLCompat: projection stack overflow");
			gGLC.projectionTop++;
			break;
		case GL_TEXTURE:
		{
			TexUnit* tu = &gGLC.units[gGLC.activeUnit];
			if (tu->matrixTop + 1 >= TEXTURE_STACK_DEPTH) GLC_FATAL("GLCompat: texture stack overflow");
			tu->matrixTop++;
			break;
		}
		default:
			if (gGLC.modelviewTop + 1 >= MODELVIEW_STACK_DEPTH) GLC_FATAL("GLCompat: modelview stack overflow");
			gGLC.modelviewTop++;
			break;
	}
	SDL_memcpy(CurrentMatrix(), src, sizeof(GLfloat)*16);
}

void GLC_PopMatrix(void)
{
	switch (gGLC.matrixMode)
	{
		case GL_PROJECTION:
			if (gGLC.projectionTop <= 0) GLC_FATAL("GLCompat: projection stack underflow");
			gGLC.projectionTop--;
			break;
		case GL_TEXTURE:
		{
			TexUnit* tu = &gGLC.units[gGLC.activeUnit];
			if (tu->matrixTop <= 0) GLC_FATAL("GLCompat: texture stack underflow");
			tu->matrixTop--;
			break;
		}
		default:
			if (gGLC.modelviewTop <= 0) GLC_FATAL("GLCompat: modelview stack underflow");
			gGLC.modelviewTop--;
			break;
	}
}

void GLC_LoadIdentity(void)
{
	Mat4Identity(CurrentMatrix());
}

void GLC_LoadMatrixf(const GLfloat* src)
{
	SDL_memcpy(CurrentMatrix(), src, sizeof(GLfloat)*16);
}

void GLC_MultMatrixf(const GLfloat* src)
{
	GLfloat* m = CurrentMatrix();
	Mat4Mul(m, src, m);
}

void GLC_Ortho(double l, double r, double b, double t, double n, double f)
{
	GLfloat o[16] = {0};
	o[0]  = (GLfloat) (2.0 / (r - l));
	o[5]  = (GLfloat) (2.0 / (t - b));
	o[10] = (GLfloat) (-2.0 / (f - n));
	o[12] = (GLfloat) (-(r + l) / (r - l));
	o[13] = (GLfloat) (-(t + b) / (t - b));
	o[14] = (GLfloat) (-(f + n) / (f - n));
	o[15] = 1;
	GLC_MultMatrixf(o);
}

void GLC_Frustum(double l, double r, double b, double t, double n, double f)
{
	GLfloat o[16] = {0};
	o[0]  = (GLfloat) (2.0 * n / (r - l));
	o[5]  = (GLfloat) (2.0 * n / (t - b));
	o[8]  = (GLfloat) ((r + l) / (r - l));
	o[9]  = (GLfloat) ((t + b) / (t - b));
	o[10] = (GLfloat) (-(f + n) / (f - n));
	o[11] = -1;
	o[14] = (GLfloat) (-2.0 * f * n / (f - n));
	GLC_MultMatrixf(o);
}

void GLC_Translatef(GLfloat x, GLfloat y, GLfloat z)
{
	GLfloat o[16];
	Mat4Identity(o);
	o[12] = x;
	o[13] = y;
	o[14] = z;
	GLC_MultMatrixf(o);
}

void GLC_Scalef(GLfloat x, GLfloat y, GLfloat z)
{
	GLfloat o[16];
	Mat4Identity(o);
	o[0] = x;
	o[5] = y;
	o[10] = z;
	GLC_MultMatrixf(o);
}

void GLC_Rotatef(GLfloat angleDegrees, GLfloat x, GLfloat y, GLfloat z)
{
	float len = sqrtf(x*x + y*y + z*z);
	if (len <= 0)
		return;
	x /= len; y /= len; z /= len;

	float a = angleDegrees * (float) (M_PI / 180.0);
	float c = cosf(a), s = sinf(a), ic = 1 - c;

	GLfloat o[16];
	Mat4Identity(o);
	o[0] = x*x*ic + c;		o[4] = x*y*ic - z*s;	o[8]  = x*z*ic + y*s;
	o[1] = y*x*ic + z*s;	o[5] = y*y*ic + c;		o[9]  = y*z*ic - x*s;
	o[2] = x*z*ic - y*s;	o[6] = y*z*ic + x*s;	o[10] = z*z*ic + c;
	GLC_MultMatrixf(o);
}

// Inverse-transpose of the upper 3x3 of a column-major 4x4 matrix
static void NormalMatrix(const GLfloat* m, GLfloat* out)
{
	float a = m[0], b = m[4], c = m[8];
	float d = m[1], e = m[5], f = m[9];
	float g = m[2], h = m[6], i = m[10];

	float A =  (e*i - f*h);
	float B = -(d*i - f*g);
	float C =  (d*h - e*g);
	float det = a*A + b*B + c*C;
	float invDet = fabsf(det) > 1e-12f ? 1.0f / det : 0.0f;

	out[0] = A * invDet;
	out[3] = B * invDet;
	out[6] = C * invDet;
	out[1] = -(b*i - c*h) * invDet;
	out[4] =  (a*i - c*g) * invDet;
	out[7] = -(a*h - b*g) * invDet;
	out[2] =  (b*f - c*e) * invDet;
	out[5] = -(a*f - c*d) * invDet;
	out[8] =  (a*e - b*d) * invDet;
}

#pragma mark - Current vertex state

void GLC_Color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	gGLC.color[0] = r;
	gGLC.color[1] = g;
	gGLC.color[2] = b;
	gGLC.color[3] = a;
}

void GLC_Color4fv(const GLfloat* c)		{ GLC_Color4f(c[0], c[1], c[2], c[3]); }
void GLC_Color3f(GLfloat r, GLfloat g, GLfloat b)	{ GLC_Color4f(r, g, b, 1); }

void GLC_TexCoord2f(GLfloat u, GLfloat v)
{
	gGLC.units[0].currentUV[0] = u;
	gGLC.units[0].currentUV[1] = v;
}

void GLC_TexCoord2fv(const GLfloat* uv)	{ GLC_TexCoord2f(uv[0], uv[1]); }

void GLC_Normal3f(GLfloat x, GLfloat y, GLfloat z)
{
	gGLC.normal[0] = x;
	gGLC.normal[1] = y;
	gGLC.normal[2] = z;
}

void GLC_Materialfv(GLenum face, GLenum pname, const GLfloat* params)
{
	(void) face;
	if (pname == GL_AMBIENT_AND_DIFFUSE || pname == GL_DIFFUSE)
		SDL_memcpy(gGLC.materialDiffuse, params, sizeof(GLfloat) * 4);
}

#pragma mark - Enables & queries

// Caps that GLES accepts in glEnable/glDisable/glIsEnabled. Anything else from
// desktop GL must not reach GLES, or it raises GL_INVALID_ENUM (fatal in this game).
static bool IsNativeCap(GLenum cap)
{
	switch (cap)
	{
		case GL_BLEND:
		case GL_CULL_FACE:
		case GL_DEPTH_TEST:
		case GL_DITHER:
		case GL_POLYGON_OFFSET_FILL:
		case GL_SAMPLE_ALPHA_TO_COVERAGE:
		case GL_SAMPLE_COVERAGE:
		case GL_SCISSOR_TEST:
		case GL_STENCIL_TEST:
		case GL_RASTERIZER_DISCARD:
		case GL_PRIMITIVE_RESTART_FIXED_INDEX:
#ifdef GL_FRAMEBUFFER_SRGB_EXT
		case GL_FRAMEBUFFER_SRGB_EXT:
#endif
			return true;
		default:
			return false;
	}
}

static void SetCap(GLenum cap, bool on)
{
	TexUnit* tu = &gGLC.units[gGLC.activeUnit];

	switch (cap)
	{
		case GL_LIGHTING:			gGLC.lighting = on;			return;
		case GL_ALPHA_TEST:			gGLC.alphaTest = on;		return;
		case GL_FOG:				gGLC.fog = on;				return;
		case GL_NORMALIZE:			gGLC.normalize = on;		return;
		case GL_RESCALE_NORMAL:		gGLC.rescaleNormal = on;	return;
		case GL_COLOR_MATERIAL:		gGLC.colorMaterial = on;	return;
		case GL_TEXTURE_2D:			tu->texture2D = on;			return;
		case GL_TEXTURE_GEN_S:		tu->texGenS = on;			return;
		case GL_TEXTURE_GEN_T:		tu->texGenT = on;			return;
	}

	if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + MAX_LIGHTS)
	{
		gGLC.lightEnabled[cap - GL_LIGHT0] = on;
		return;
	}

	if (IsNativeCap(cap))
	{
		if (on)
			glEnable(cap);
		else
			glDisable(cap);
		return;
	}

	static GLenum lastWarned = 0;
	if (cap != lastWarned)
	{
		SDL_Log("GLCompat: ignoring unsupported cap 0x%x", cap);
		lastWarned = cap;
	}
}

void GLC_Enable(GLenum cap)		{ SetCap(cap, true); }
void GLC_Disable(GLenum cap)	{ SetCap(cap, false); }

GLboolean GLC_IsEnabled(GLenum cap)
{
	const TexUnit* tu = &gGLC.units[gGLC.activeUnit];

	switch (cap)
	{
		case GL_LIGHTING:			return gGLC.lighting;
		case GL_ALPHA_TEST:			return gGLC.alphaTest;
		case GL_FOG:				return gGLC.fog;
		case GL_NORMALIZE:			return gGLC.normalize;
		case GL_RESCALE_NORMAL:		return gGLC.rescaleNormal;
		case GL_COLOR_MATERIAL:		return gGLC.colorMaterial;
		case GL_TEXTURE_2D:			return tu->texture2D;
		case GL_TEXTURE_GEN_S:		return tu->texGenS;
		case GL_TEXTURE_GEN_T:		return tu->texGenT;
	}

	if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + MAX_LIGHTS)
		return gGLC.lightEnabled[cap - GL_LIGHT0];

	if (IsNativeCap(cap))
		return glIsEnabled(cap);

	return GL_FALSE;
}

void GLC_GetFloatv(GLenum pname, GLfloat* params)
{
	switch (pname)
	{
		case GL_CURRENT_COLOR:			SDL_memcpy(params, gGLC.color, sizeof(GLfloat)*4);				break;
		case GL_MODELVIEW_MATRIX:		SDL_memcpy(params, GLC_GetMatrix(GL_MODELVIEW), sizeof(GLfloat)*16);	break;
		case GL_PROJECTION_MATRIX:		SDL_memcpy(params, GLC_GetMatrix(GL_PROJECTION), sizeof(GLfloat)*16);	break;
		default:						glGetFloatv(pname, params);										break;
	}
}

void GLC_GetIntegerv(GLenum pname, GLint* params)
{
	switch (pname)
	{
		case GL_BLEND_SRC:		*params = (GLint) gGLC.blendSrc;	break;
		case GL_BLEND_DST:		*params = (GLint) gGLC.blendDst;	break;
		default:				glGetIntegerv(pname, params);		break;
	}
}

void GLC_ActiveTexture(GLenum unit)
{
	int i = (int) (unit - GL_TEXTURE0);
	if (i < 0 || i >= NUM_UNITS)
		GLC_FATAL("GLCompat: texture unit %d not supported", i);
	gGLC.activeUnit = i;
	glActiveTexture(unit);
}

void GLC_ClientActiveTexture(GLenum unit)
{
	int i = (int) (unit - GL_TEXTURE0);
	if (i < 0 || i >= NUM_UNITS)
		GLC_FATAL("GLCompat: client texture unit %d not supported", i);
	gGLC.clientActiveUnit = i;
}

void GLC_TexEnvi(GLenum target, GLenum pname, GLint param)
{
	(void) target;
	TexUnit* tu = &gGLC.units[gGLC.activeUnit];
	switch (pname)
	{
		case GL_TEXTURE_ENV_MODE:	tu->envMode = param;		break;
		case GL_COMBINE_RGB:		tu->combineRGB = param;		break;
		case GL_COMBINE_ALPHA:		tu->combineAlpha = param;	break;
		default:												break;
	}
}

void GLC_TexGeni(GLenum coord, GLenum pname, GLint param)
{
	(void) coord;
	(void) pname;
	if (param != GL_SPHERE_MAP)
		SDL_Log("GLCompat: unsupported texgen mode 0x%x", param);
}

#pragma mark - Client arrays

static ClientArray* GetClientArray(GLenum array)
{
	switch (array)
	{
		case GL_VERTEX_ARRAY:			return &gGLC.posArray;
		case GL_NORMAL_ARRAY:			return &gGLC.normalArray;
		case GL_COLOR_ARRAY:			return &gGLC.colorArray;
		case GL_TEXTURE_COORD_ARRAY:	return &gGLC.units[gGLC.clientActiveUnit].uvArray;
		default:						return NULL;
	}
}

void GLC_EnableClientState(GLenum array)
{
	ClientArray* a = GetClientArray(array);
	if (a) a->enabled = true;
}

void GLC_DisableClientState(GLenum array)
{
	ClientArray* a = GetClientArray(array);
	if (a) a->enabled = false;
}

static void SetArray(ClientArray* a, GLint size, GLenum type, GLsizei stride, const void* ptr)
{
	a->size = size;
	a->type = type;
	a->stride = stride;
	a->ptr = ptr;
}

void GLC_VertexPointer(GLint size, GLenum type, GLsizei stride, const void* ptr)	{ SetArray(&gGLC.posArray, size, type, stride, ptr); }
void GLC_NormalPointer(GLenum type, GLsizei stride, const void* ptr)				{ SetArray(&gGLC.normalArray, 3, type, stride, ptr); }
void GLC_ColorPointer(GLint size, GLenum type, GLsizei stride, const void* ptr)		{ SetArray(&gGLC.colorArray, size, type, stride, ptr); }
void GLC_TexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* ptr)	{ SetArray(&gGLC.units[gGLC.clientActiveUnit].uvArray, size, type, stride, ptr); }

#pragma mark - Lights, fog, blending

void GLC_Lightfv(GLenum light, GLenum pname, const GLfloat* params)
{
	int i = light - GL_LIGHT0;
	if (i < 0 || i >= MAX_LIGHTS)
		return;

	switch (pname)
	{
		case GL_POSITION:
		{
			// Directional light: transform direction into eye space by the current modelview matrix
			const GLfloat* m = gGLC.modelview[gGLC.modelviewTop];
			float x = params[0], y = params[1], z = params[2];
			float ex = m[0]*x + m[4]*y + m[8]*z;
			float ey = m[1]*x + m[5]*y + m[9]*z;
			float ez = m[2]*x + m[6]*y + m[10]*z;
			float len = sqrtf(ex*ex + ey*ey + ez*ez);
			if (len > 0)
			{
				ex /= len;
				ey /= len;
				ez /= len;
			}
			gGLC.lightEyeDir[i][0] = ex;
			gGLC.lightEyeDir[i][1] = ey;
			gGLC.lightEyeDir[i][2] = ez;
			break;
		}

		case GL_DIFFUSE:
			SDL_memcpy(gGLC.lightDiffuse[i], params, sizeof(GLfloat)*3);
			break;

		case GL_AMBIENT:
			SDL_memcpy(gGLC.lightAmbient[i], params, sizeof(GLfloat)*3);
			break;

		default:
			break;
	}
}

void GLC_LightModelfv(GLenum pname, const GLfloat* params)
{
	if (pname == GL_LIGHT_MODEL_AMBIENT)
		SDL_memcpy(gGLC.sceneAmbient, params, sizeof(GLfloat)*3);
}

void GLC_Fogi(GLenum pname, GLint param)
{
	if (pname == GL_FOG_MODE)
		gGLC.fogMode = (GLenum) param;
}

void GLC_Fogf(GLenum pname, GLfloat param)
{
	switch (pname)
	{
		case GL_FOG_START:		gGLC.fogStart = param;			break;
		case GL_FOG_END:		gGLC.fogEnd = param;			break;
		case GL_FOG_DENSITY:	gGLC.fogDensity = param;		break;
		case GL_FOG_MODE:		gGLC.fogMode = (GLenum) param;	break;
	}
}

void GLC_Fogfv(GLenum pname, const GLfloat* params)
{
	if (pname == GL_FOG_COLOR)
		SDL_memcpy(gGLC.fogColor, params, sizeof(GLfloat)*3);
	else
		GLC_Fogf(pname, params[0]);
}

void GLC_AlphaFunc(GLenum func, GLfloat ref)
{
	gGLC.alphaFunc = func;
	gGLC.alphaRef = ref;
}

// Keep destination alpha meaningful (premultiplied), so that a render target
// with transparent areas (the VR HUD layer) composites correctly.
void GLC_BlendFunc(GLenum sfactor, GLenum dfactor)
{
	gGLC.blendSrc = sfactor;
	gGLC.blendDst = dfactor;

	if (sfactor == GL_SRC_ALPHA && dfactor == GL_ONE)							// additive
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
	else if (sfactor == GL_SRC_ALPHA && dfactor == GL_ONE_MINUS_SRC_ALPHA)
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	else
		glBlendFunc(sfactor, dfactor);
}

// Light directions are stored in eye space (like fixed-function GL).
// When switching between views (e.g. per-eye VR rendering), move them from one
// view's eye space to another. Assumes rigid (rotation + translation) view matrices.
void GLC_RebaseLights(const GLfloat* a, const GLfloat* b)
{
	for (int i = 0; i < MAX_LIGHTS; i++)
	{
		const float* e = gGLC.lightEyeDir[i];

		// world = transpose(A3x3) * eyeA
		float wx = a[0]*e[0] + a[1]*e[1] + a[2]*e[2];
		float wy = a[4]*e[0] + a[5]*e[1] + a[6]*e[2];
		float wz = a[8]*e[0] + a[9]*e[1] + a[10]*e[2];

		// eyeB = B3x3 * world
		gGLC.lightEyeDir[i][0] = b[0]*wx + b[4]*wy + b[8]*wz;
		gGLC.lightEyeDir[i][1] = b[1]*wx + b[5]*wy + b[9]*wz;
		gGLC.lightEyeDir[i][2] = b[2]*wx + b[6]*wy + b[10]*wz;
	}
}

#pragma mark - Drawing

static int EnvCode(const TexUnit* tu)
{
	switch (tu->envMode)
	{
		case GL_REPLACE:	return kEnv_Replace;
		case GL_DECAL:		return kEnv_Decal;
		case GL_COMBINE:
			if (tu->combineRGB == GL_ADD)
				return tu->combineAlpha == GL_ADD ? kEnv_AddRGB_AddA : kEnv_AddRGB_ModA;
			return kEnv_Modulate;
		default:			return kEnv_Modulate;
	}
}

static int AlphaFuncCode(void)
{
	if (!gGLC.alphaTest)
		return 0;
	switch (gGLC.alphaFunc)
	{
		case GL_GREATER:	return 1;
		case GL_NOTEQUAL:	return 2;
		case GL_EQUAL:		return 3;
		case GL_GEQUAL:		return 4;
		case GL_LESS:		return 5;
		case GL_LEQUAL:		return 6;
		default:			return 0;		// GL_ALWAYS (GL_NEVER isn't used)
	}
}

static void BindAttrib(int index, const ClientArray* a, const GLfloat* fallback4)
{
	if (a && a->enabled && a->ptr)
	{
		glEnableVertexAttribArray(index);
		GLboolean normalized = (a->type != GL_FLOAT);		// e.g. GL_UNSIGNED_BYTE colors -> 0..1
		glVertexAttribPointer(index, a->size, a->type, normalized, a->stride, a->ptr);
	}
	else
	{
		glDisableVertexAttribArray(index);
		glVertexAttrib4fv(index, fallback4);
	}
}

static void ApplyState(void)
{
	if (!gGLC.initialized)
		GLC_FATAL("GLCompat: not initialized");

	glUseProgram(gGLC.program);

			/* MATRICES */

	const GLfloat* mv = gGLC.modelview[gGLC.modelviewTop];
	const GLfloat* proj = gGLC.projection[gGLC.projectionTop];
	GLfloat mvp[16];
	Mat4Mul(proj, mv, mvp);
	glUniformMatrix4fv(gGLC.uMVP, 1, GL_FALSE, mvp);
	glUniformMatrix4fv(gGLC.uMV, 1, GL_FALSE, mv);

	GLfloat nm[9];
	NormalMatrix(mv, nm);
	glUniformMatrix3fv(gGLC.uNormalMat, 1, GL_FALSE, nm);

			/* LIGHTING */

	glUniform1i(gGLC.uLighting, gGLC.lighting);
	if (gGLC.lighting)
	{
		GLfloat dirs[MAX_LIGHTS*3];
		GLfloat diffuse[MAX_LIGHTS*3];
		GLfloat ambientSum[3] = {0, 0, 0};
		int n = 0;
		for (int i = 0; i < MAX_LIGHTS; i++)
		{
			if (!gGLC.lightEnabled[i])
				continue;
			SDL_memcpy(&dirs[n*3], gGLC.lightEyeDir[i], sizeof(GLfloat)*3);
			SDL_memcpy(&diffuse[n*3], gGLC.lightDiffuse[i], sizeof(GLfloat)*3);
			ambientSum[0] += gGLC.lightAmbient[i][0];
			ambientSum[1] += gGLC.lightAmbient[i][1];
			ambientSum[2] += gGLC.lightAmbient[i][2];
			n++;
		}
		glUniform1i(gGLC.uNumLights, n);
		if (n > 0)
		{
			glUniform3fv(gGLC.uLightDir, n, dirs);
			glUniform3fv(gGLC.uLightDiffuse, n, diffuse);
		}
		glUniform3fv(gGLC.uLightAmbientSum, 1, ambientSum);
		glUniform3fv(gGLC.uSceneAmbient, 1, gGLC.sceneAmbient);
	}

			/* TEXTURE UNITS */

	for (int u = 0; u < NUM_UNITS; u++)
	{
		const TexUnit* tu = &gGLC.units[u];
		glUniform1i(gGLC.uTexEnabled[u], tu->texture2D);
		if (tu->texture2D)
		{
			glUniform1i(gGLC.uTexEnv[u], EnvCode(tu));
			glUniform1i(gGLC.uTexGen[u], tu->texGenS && tu->texGenT);
			glUniformMatrix4fv(gGLC.uTexMat[u], 1, GL_FALSE, tu->matrix[tu->matrixTop]);
		}
		else
		{
			glUniform1i(gGLC.uTexGen[u], 0);
		}
	}

			/* ALPHA TEST & FOG */

	glUniform1i(gGLC.uAlphaFunc, AlphaFuncCode());
	glUniform1f(gGLC.uAlphaRef, gGLC.alphaRef);

	int fogCode = 0;
	if (gGLC.fog)
		fogCode = gGLC.fogMode == GL_LINEAR ? 1 : (gGLC.fogMode == GL_EXP2 ? 3 : 2);
	glUniform1i(gGLC.uFogMode, fogCode);
	if (fogCode)
	{
		glUniform3f(gGLC.uFogParams, gGLC.fogStart, gGLC.fogEnd, gGLC.fogDensity);
		glUniform3fv(gGLC.uFogColor, 1, gGLC.fogColor);
	}

			/* VERTEX ATTRIBUTES (client-side arrays are legal in GLES3 with the default VAO) */

	static const GLfloat kZero4[4] = {0, 0, 0, 1};
	GLfloat normal4[4] = { gGLC.normal[0], gGLC.normal[1], gGLC.normal[2], 0 };
	GLfloat uv0[4] = { gGLC.units[0].currentUV[0], gGLC.units[0].currentUV[1], 0, 1 };

	BindAttrib(kAttrib_Position, &gGLC.posArray, kZero4);
	BindAttrib(kAttrib_Normal, &gGLC.normalArray, normal4);

	if (gGLC.lighting && !gGLC.colorMaterial)
		BindAttrib(kAttrib_Color, NULL, gGLC.materialDiffuse);		// lit with the material color, vertex colors are ignored
	else
		BindAttrib(kAttrib_Color, &gGLC.colorArray, gGLC.color);

	BindAttrib(kAttrib_UV0, &gGLC.units[0].uvArray, uv0);
	BindAttrib(kAttrib_UV1, &gGLC.units[1].uvArray, kZero4);
}

void GLC_DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
	ApplyState();
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDrawElements(mode, count, type, indices);
}

void GLC_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
	ApplyState();
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDrawArrays(mode, first, count);
}

#pragma mark - Immediate mode

void GLC_Begin(GLenum mode)
{
	gGLC.immediateMode = mode;
	gGLC.immediateCount = 0;
}

void GLC_Vertex3f(GLfloat x, GLfloat y, GLfloat z)
{
	if (gGLC.immediateCount >= MAX_IMMEDIATE_VERTICES)
		return;

	ImmediateVertex* v = &gGLC.immediateVerts[gGLC.immediateCount++];
	v->pos[0] = x;
	v->pos[1] = y;
	v->pos[2] = z;
	v->uv[0] = gGLC.units[0].currentUV[0];
	v->uv[1] = gGLC.units[0].currentUV[1];
	SDL_memcpy(v->color, gGLC.color, sizeof(v->color));
	SDL_memcpy(v->normal, gGLC.normal, sizeof(v->normal));
}

void GLC_Vertex3fv(const GLfloat* v)			{ GLC_Vertex3f(v[0], v[1], v[2]); }
void GLC_Vertex2f(GLfloat x, GLfloat y)			{ GLC_Vertex3f(x, y, 0); }

void GLC_End(void)
{
	int count = gGLC.immediateCount;
	gGLC.immediateCount = 0;
	if (count == 0)
		return;

	GLenum mode = gGLC.immediateMode;
	static uint16_t indices[MAX_IMMEDIATE_VERTICES / 4 * 6];
	int numIndices = 0;

	switch (mode)
	{
		case GL_QUADS:
			for (int q = 0; q + 3 < count; q += 4)
			{
				indices[numIndices++] = q+0;	indices[numIndices++] = q+1;	indices[numIndices++] = q+2;
				indices[numIndices++] = q+0;	indices[numIndices++] = q+2;	indices[numIndices++] = q+3;
			}
			mode = GL_TRIANGLES;
			break;
		case GL_QUAD_STRIP:
			mode = GL_TRIANGLE_STRIP;		// same vertex order
			break;
		case GL_POLYGON:
			mode = GL_TRIANGLE_FAN;
			break;
	}

			/* SUBSTITUTE CLIENT ARRAYS WITH THE IMMEDIATE VERTICES */

	ClientArray savedPos = gGLC.posArray;
	ClientArray savedNormal = gGLC.normalArray;
	ClientArray savedColor = gGLC.colorArray;
	ClientArray savedUV0 = gGLC.units[0].uvArray;
	ClientArray savedUV1 = gGLC.units[1].uvArray;

	const ImmediateVertex* v = gGLC.immediateVerts;
	const GLsizei stride = sizeof(ImmediateVertex);
	gGLC.posArray		= (ClientArray) { true, 3, GL_FLOAT, stride, v->pos };
	gGLC.normalArray	= (ClientArray) { true, 3, GL_FLOAT, stride, v->normal };
	gGLC.colorArray		= (ClientArray) { true, 4, GL_FLOAT, stride, v->color };
	gGLC.units[0].uvArray = (ClientArray) { true, 2, GL_FLOAT, stride, v->uv };
	gGLC.units[1].uvArray = (ClientArray) { false, 0, 0, 0, NULL };

	ApplyState();
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	if (numIndices > 0)
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		glDrawElements(mode, numIndices, GL_UNSIGNED_SHORT, indices);
	}
	else
	{
		glDrawArrays(mode, 0, count);
	}

	gGLC.posArray = savedPos;
	gGLC.normalArray = savedNormal;
	gGLC.colorArray = savedColor;
	gGLC.units[0].uvArray = savedUV0;
	gGLC.units[1].uvArray = savedUV1;
}

#pragma mark - Textures

static uint8_t* GetOpaqueFlagForBoundTexture(void)
{
	GLint name = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &name);
	if (name <= 0)
		return NULL;

	if ((size_t) name >= gGLC.textureOpaqueFlagsCapacity)
	{
		size_t newCap = SDL_max(1024, (size_t) name * 2);
		gGLC.textureOpaqueFlags = SDL_realloc(gGLC.textureOpaqueFlags, newCap);
		SDL_memset(gGLC.textureOpaqueFlags + gGLC.textureOpaqueFlagsCapacity, 0, newCap - gGLC.textureOpaqueFlagsCapacity);
		gGLC.textureOpaqueFlagsCapacity = newCap;
	}

	return &gGLC.textureOpaqueFlags[name];
}

// Converts a desktop-GL pixel buffer into tightly-packed RGBA8.
static const void* ConvertToRGBA8(
		int w, int h, GLenum format, GLenum type, const void* pixels, int srcRowPixels, bool forceOpaque)
{
	if (!pixels)
		return NULL;

	if (srcRowPixels <= 0)
		srcRowPixels = w;

	size_t needed = (size_t) w * h * 4;
	if (needed > gGLC.convertBufferSize)
	{
		gGLC.convertBuffer = SDL_realloc(gGLC.convertBuffer, needed);
		gGLC.convertBufferSize = needed;
	}

	uint8_t* out = gGLC.convertBuffer;

	for (int y = 0; y < h; y++)
	{
		uint8_t* dst = out + (size_t) y * w * 4;

		if (type == GL_UNSIGNED_SHORT_1_5_5_5_REV)		// format is GL_BGRA: A1 R5 G5 B5 from the top bit down
		{
			const uint16_t* src = (const uint16_t*) pixels + (size_t) y * srcRowPixels;
			for (int x = 0; x < w; x++)
			{
				uint16_t p = src[x];
				uint8_t b5 = (p      ) & 0x1F;
				uint8_t g5 = (p >>  5) & 0x1F;
				uint8_t r5 = (p >> 10) & 0x1F;
				dst[x*4+0] = (r5 << 3) | (r5 >> 2);
				dst[x*4+1] = (g5 << 3) | (g5 >> 2);
				dst[x*4+2] = (b5 << 3) | (b5 >> 2);
				dst[x*4+3] = (p & 0x8000) ? 0xFF : 0x00;
			}
		}
		else if (format == GL_BGRA)		// GL_UNSIGNED_INT_8_8_8_8_REV or GL_UNSIGNED_BYTE: B,G,R,A in memory
		{
			const uint8_t* src = (const uint8_t*) pixels + (size_t) y * srcRowPixels * 4;
			for (int x = 0; x < w; x++)
			{
				dst[x*4+0] = src[x*4+2];
				dst[x*4+1] = src[x*4+1];
				dst[x*4+2] = src[x*4+0];
				dst[x*4+3] = src[x*4+3];
			}
		}
		else if (format == GL_BGR)
		{
			const uint8_t* src = (const uint8_t*) pixels + (size_t) y * srcRowPixels * 3;
			for (int x = 0; x < w; x++)
			{
				dst[x*4+0] = src[x*3+2];
				dst[x*4+1] = src[x*3+1];
				dst[x*4+2] = src[x*3+0];
				dst[x*4+3] = 0xFF;
			}
		}
		else if (format == GL_RGB)
		{
			const uint8_t* src = (const uint8_t*) pixels + (size_t) y * srcRowPixels * 3;
			for (int x = 0; x < w; x++)
			{
				dst[x*4+0] = src[x*3+0];
				dst[x*4+1] = src[x*3+1];
				dst[x*4+2] = src[x*3+2];
				dst[x*4+3] = 0xFF;
			}
		}
		else		// GL_RGBA, GL_UNSIGNED_BYTE
		{
			const uint8_t* src = (const uint8_t*) pixels + (size_t) y * srcRowPixels * 4;
			SDL_memcpy(dst, src, (size_t) w * 4);
		}

		if (forceOpaque)
		{
			for (int x = 0; x < w; x++)
				dst[x*4+3] = 0xFF;
		}
	}

	return out;
}

void GLC_TexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void* pixels)
{
	(void) border;

	bool forceOpaque = (internalFormat == GL_RGB || internalFormat == GL_RGB8 || internalFormat == 3);
	uint8_t* opaqueFlag = GetOpaqueFlagForBoundTexture();
	if (opaqueFlag)
		*opaqueFlag = forceOpaque;

	GLint rowLength = 0;
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	const void* rgba = ConvertToRGBA8(w, h, format, type, pixels, rowLength, forceOpaque);
	glTexImage2D(target, level, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

	glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
}

void GLC_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels)
{
	uint8_t* opaqueFlag = GetOpaqueFlagForBoundTexture();
	bool forceOpaque = opaqueFlag && *opaqueFlag;

	GLint rowLength = 0;
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	const void* rgba = ConvertToRGBA8(w, h, format, type, pixels, rowLength, forceOpaque);
	glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

	glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
}

void GLC_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels)
{
	if (format == GL_DEPTH_COMPONENT)
	{
		// GLES can't read the depth buffer. Only used for the lens flare's sun occlusion test:
		// report "far plane" (= not blocked).
		if (type == GL_FLOAT)
			for (int i = 0; i < w * h; i++)
				((GLfloat*) pixels)[i] = 1.0f;
		return;
	}

	glReadPixels(x, y, w, h, format, type, pixels);
}

#if !__ANDROID__
// Desktop GLES test build only: saves the given frame numbers as PPM images,
// e.g. GLC_SNAPSHOT_FRAMES="60,300,900" -> /tmp/glc-frame-60.ppm ...
void GLC_DebugSnapshot(int w, int h)
{
	static int frame = 0;
	static const char* list = NULL;
	static bool checked = false;
	if (!checked)
	{
		list = SDL_getenv("GLC_SNAPSHOT_FRAMES");
		checked = true;
	}
	frame++;
	if (!list)
		return;

	char needle[32];
	SDL_snprintf(needle, sizeof(needle), ",%d,", frame);
	char hay[512];
	SDL_snprintf(hay, sizeof(hay), ",%s,", list);
	if (!SDL_strstr(hay, needle))
		return;

	uint8_t* px = SDL_malloc((size_t) w * h * 4);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	char path[256];
	SDL_snprintf(path, sizeof(path), "/tmp/glc-frame-%d.ppm", frame);
	FILE* f = fopen(path, "wb");
	if (f)
	{
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		for (int y = h - 1; y >= 0; y--)
			for (int x = 0; x < w; x++)
				fwrite(&px[((size_t) y * w + x) * 4], 1, 3, f);
		fclose(f);
		SDL_Log("GLC: saved %s", path);
	}
	SDL_free(px);
}
#endif

#endif // GLC_ENABLED
