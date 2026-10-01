// GL_COMPAT.H
// On desktop, this just pulls in regular OpenGL.
// On Android (Meta Quest / PICO), the game's fixed-function OpenGL 1.x calls are
// routed to a shader-based emulation layer on top of OpenGL ES 3 (System/GLCompat.c).
// Define GLC_FORCE_GLES to use the emulation layer on desktop too (for testing).

#pragma once

#if defined(__ANDROID__) || defined(GLC_FORCE_GLES)
	#define GLC_ENABLED 1
#else
	#define GLC_ENABLED 0
#endif

#if !GLC_ENABLED

#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>

#else

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#ifdef __cplusplus
extern "C" {
#endif

// Enums that don't exist in GLES
#define GL_PROJECTION					0x1701
#define GL_MODELVIEW					0x1700
#define GL_MODELVIEW_MATRIX				0x0BA6
#define GL_PROJECTION_MATRIX			0x0BA7
#define GL_CURRENT_COLOR				0x0B00
#define GL_BLEND_SRC					0x0BE1
#define GL_BLEND_DST					0x0BE0
#define GL_LIGHTING						0x0B50
#define GL_LIGHT0						0x4000
#define GL_NORMALIZE					0x0BA1
#define GL_RESCALE_NORMAL				0x803A
#define GL_ALPHA_TEST					0x0BC0
#define GL_COLOR_MATERIAL				0x0B57
#define GL_FOG							0x0B60
#define GL_FOG_DENSITY					0x0B62
#define GL_FOG_START					0x0B63
#define GL_FOG_END						0x0B64
#define GL_FOG_MODE						0x0B65
#define GL_FOG_COLOR					0x0B66
#define GL_FOG_HINT						0x0C54
#define GL_EXP							0x0800
#define GL_EXP2							0x0801
#define GL_PERSPECTIVE_CORRECTION_HINT	0x0C50
#define GL_AMBIENT						0x1200
#define GL_DIFFUSE						0x1201
#define GL_SPECULAR						0x1202
#define GL_POSITION						0x1203
#define GL_EMISSION						0x1600
#define GL_SHININESS					0x1601
#define GL_AMBIENT_AND_DIFFUSE			0x1602
#define GL_LIGHT_MODEL_AMBIENT			0x0B53
#define GL_LIGHT_MODEL_TWO_SIDE			0x0B52
#define GL_VERTEX_ARRAY					0x8074
#define GL_NORMAL_ARRAY					0x8075
#define GL_COLOR_ARRAY					0x8076
#define GL_TEXTURE_COORD_ARRAY			0x8078
#define GL_TEXTURE_ENV					0x2300
#define GL_TEXTURE_ENV_MODE				0x2200
#define GL_MODULATE						0x2100
#define GL_DECAL						0x2101
#define GL_ADD							0x0104
#define GL_COMBINE						0x8570
#define GL_COMBINE_RGB					0x8571
#define GL_COMBINE_ALPHA				0x8572
#define GL_S							0x2000
#define GL_T							0x2001
#define GL_TEXTURE_GEN_MODE				0x2500
#define GL_SPHERE_MAP					0x2402
#define GL_TEXTURE_GEN_S				0x0C60
#define GL_TEXTURE_GEN_T				0x0C61
#define GL_TEXTURE0_ARB					GL_TEXTURE0
#define GL_TEXTURE1_ARB					GL_TEXTURE1
#define GL_BGR							0x80E0
#define GL_BGRA							0x80E1
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT						0x80E1
#endif
#define GL_UNSIGNED_INT_8_8_8_8_REV		0x8367
#define GL_UNSIGNED_SHORT_1_5_5_5_REV	0x8366
#define GL_LINE							0x1B01
#define GL_FILL							0x1B02
#define GL_QUADS						0x0007
#define GL_QUAD_STRIP					0x0008
#define GL_POLYGON						0x0009
#define GL_CLAMP						0x2900
#define GL_STACK_OVERFLOW				0x0503
#define GL_STACK_UNDERFLOW				0x0504

typedef double GLdouble;
typedef void (*PFNGLACTIVETEXTUREPROC)(GLenum texture);
typedef void (*PFNGLCLIENTACTIVETEXTUREARBPROC)(GLenum texture);

// Fixed-function emulation entry points (implemented in System/GLCompat.c)
void GLC_Init(void);
const GLfloat* GLC_GetMatrix(GLenum mode);
void GLC_RebaseLights(const GLfloat* oldWorldToView, const GLfloat* newWorldToView);

void GLC_MatrixMode(GLenum mode);
void GLC_PushMatrix(void);
void GLC_PopMatrix(void);
void GLC_LoadIdentity(void);
void GLC_LoadMatrixf(const GLfloat* m);
void GLC_MultMatrixf(const GLfloat* m);
void GLC_Ortho(double l, double r, double b, double t, double n, double f);
void GLC_Frustum(double l, double r, double b, double t, double n, double f);
void GLC_Translatef(GLfloat x, GLfloat y, GLfloat z);
void GLC_Scalef(GLfloat x, GLfloat y, GLfloat z);
void GLC_Rotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);

void GLC_Color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void GLC_Color4fv(const GLfloat* c);
void GLC_Color3f(GLfloat r, GLfloat g, GLfloat b);
void GLC_TexCoord2f(GLfloat u, GLfloat v);
void GLC_TexCoord2fv(const GLfloat* uv);
void GLC_Normal3f(GLfloat x, GLfloat y, GLfloat z);
void GLC_Materialfv(GLenum face, GLenum pname, const GLfloat* params);

void GLC_Enable(GLenum cap);
void GLC_Disable(GLenum cap);
GLboolean GLC_IsEnabled(GLenum cap);
void GLC_GetFloatv(GLenum pname, GLfloat* params);
void GLC_GetIntegerv(GLenum pname, GLint* params);
void GLC_ActiveTexture(GLenum unit);
void GLC_ClientActiveTexture(GLenum unit);
void GLC_TexEnvi(GLenum target, GLenum pname, GLint param);
void GLC_TexGeni(GLenum coord, GLenum pname, GLint param);

void GLC_EnableClientState(GLenum array);
void GLC_DisableClientState(GLenum array);
void GLC_VertexPointer(GLint size, GLenum type, GLsizei stride, const void* ptr);
void GLC_NormalPointer(GLenum type, GLsizei stride, const void* ptr);
void GLC_ColorPointer(GLint size, GLenum type, GLsizei stride, const void* ptr);
void GLC_TexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* ptr);
void GLC_DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
void GLC_DrawArrays(GLenum mode, GLint first, GLsizei count);

void GLC_Lightfv(GLenum light, GLenum pname, const GLfloat* params);
void GLC_LightModelfv(GLenum pname, const GLfloat* params);
void GLC_Fogi(GLenum pname, GLint param);
void GLC_Fogf(GLenum pname, GLfloat param);
void GLC_Fogfv(GLenum pname, const GLfloat* params);
void GLC_AlphaFunc(GLenum func, GLfloat ref);
void GLC_BlendFunc(GLenum sfactor, GLenum dfactor);

void GLC_Begin(GLenum mode);
void GLC_Vertex3f(GLfloat x, GLfloat y, GLfloat z);
void GLC_Vertex3fv(const GLfloat* v);
void GLC_Vertex2f(GLfloat x, GLfloat y);
void GLC_End(void);

void GLC_TexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void* pixels);
void GLC_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels);
void GLC_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels);
void GLC_DebugSnapshot(int w, int h);		// desktop test builds: dump frames listed in $GLC_SNAPSHOT_FRAMES to /tmp

// GLCompat.c and the VR code must call the real GLES functions
#ifndef GLC_IMPLEMENTATION

#define glMatrixMode			GLC_MatrixMode
#define glPushMatrix			GLC_PushMatrix
#define glPopMatrix				GLC_PopMatrix
#define glLoadIdentity			GLC_LoadIdentity
#define glLoadMatrixf			GLC_LoadMatrixf
#define glMultMatrixf			GLC_MultMatrixf
#define glOrtho					GLC_Ortho
#define glFrustum				GLC_Frustum
#define glTranslatef			GLC_Translatef
#define glScalef				GLC_Scalef
#define glRotatef				GLC_Rotatef
#define glColor4f				GLC_Color4f
#define glColor4fv				GLC_Color4fv
#define glColor3f				GLC_Color3f
#define glTexCoord2f			GLC_TexCoord2f
#define glTexCoord2fv			GLC_TexCoord2fv
#define glNormal3f				GLC_Normal3f
#define glMaterialfv			GLC_Materialfv
#define glEnable				GLC_Enable
#define glDisable				GLC_Disable
#define glIsEnabled				GLC_IsEnabled
#define glGetFloatv				GLC_GetFloatv
#define glGetIntegerv			GLC_GetIntegerv
#define glActiveTexture			GLC_ActiveTexture
#define glClientActiveTexture	GLC_ClientActiveTexture
#define glTexEnvi				GLC_TexEnvi
#define glTexGeni				GLC_TexGeni
#define glEnableClientState		GLC_EnableClientState
#define glDisableClientState	GLC_DisableClientState
#define glVertexPointer			GLC_VertexPointer
#define glNormalPointer			GLC_NormalPointer
#define glColorPointer			GLC_ColorPointer
#define glTexCoordPointer		GLC_TexCoordPointer
#define glDrawElements			GLC_DrawElements
#define glDrawArrays			GLC_DrawArrays
#define glLightfv				GLC_Lightfv
#define glLightModelfv			GLC_LightModelfv
#define glFogi					GLC_Fogi
#define glFogf					GLC_Fogf
#define glFogfv					GLC_Fogfv
#define glAlphaFunc				GLC_AlphaFunc
#define glBlendFunc				GLC_BlendFunc
#define glBegin					GLC_Begin
#define glVertex3f				GLC_Vertex3f
#define glVertex3fv				GLC_Vertex3fv
#define glVertex2f				GLC_Vertex2f
#define glEnd					GLC_End
#define glTexImage2D			GLC_TexImage2D
#define glTexSubImage2D			GLC_TexSubImage2D
#define glReadPixels			GLC_ReadPixels
#define glColorMaterial(face, mode)		((void)0)	// always GL_AMBIENT_AND_DIFFUSE in this game
#define glLightModeli(pname, param)		((void)0)	// two-sided lighting is off in this game anyway
#define glPolygonMode(face, mode)		((void)0)	// wireframe debug mode unsupported on GLES
#define glHint(target, mode)			((void)0)
#define glLockArraysEXT(first, count)	((void)0)
#define glUnlockArraysEXT()				((void)0)

#endif // GLC_IMPLEMENTATION

#ifdef __cplusplus
}
#endif

#endif // GLC_ENABLED
