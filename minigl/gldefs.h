/*
 * OpenGL 1.1 and AGL constants and types for NV34 MiniGL (values per the public OpenGL 1.1
 * specification and Apple agl.h; Retro68 does not include OpenGL headers).
 */
#ifndef NV34_GLDEFS_H
#define NV34_GLDEFS_H

typedef unsigned int    GLenum;
typedef unsigned char   GLboolean;
typedef unsigned int    GLbitfield;
typedef signed char     GLbyte;
typedef short           GLshort;
typedef int             GLint;
typedef int             GLsizei;
typedef unsigned char   GLubyte;
typedef unsigned short  GLushort;
typedef unsigned int    GLuint;
typedef float           GLfloat;
typedef float           GLclampf;
typedef double          GLdouble;
typedef double          GLclampd;
typedef void            GLvoid;

#define GL_FALSE                0
#define GL_TRUE                 1

/* errors */
#define GL_NO_ERROR             0
#define GL_INVALID_ENUM         0x0500
#define GL_INVALID_VALUE        0x0501
#define GL_INVALID_OPERATION    0x0502
#define GL_STACK_OVERFLOW       0x0503
#define GL_STACK_UNDERFLOW      0x0504
#define GL_OUT_OF_MEMORY        0x0505

/* primitives */
#define GL_POINTS               0x0000
#define GL_LINES                0x0001
#define GL_LINE_LOOP            0x0002
#define GL_LINE_STRIP           0x0003
#define GL_TRIANGLES            0x0004
#define GL_TRIANGLE_STRIP       0x0005
#define GL_TRIANGLE_FAN         0x0006
#define GL_QUADS                0x0007
#define GL_QUAD_STRIP           0x0008
#define GL_POLYGON              0x0009

/* comparisons */
#define GL_NEVER                0x0200
#define GL_LESS                 0x0201
#define GL_EQUAL                0x0202
#define GL_LEQUAL               0x0203
#define GL_GREATER              0x0204
#define GL_NOTEQUAL             0x0205
#define GL_GEQUAL               0x0206
#define GL_ALWAYS               0x0207

/* blending */
#define GL_ZERO                 0
#define GL_ONE                  1
#define GL_SRC_COLOR            0x0300
#define GL_ONE_MINUS_SRC_COLOR  0x0301
#define GL_SRC_ALPHA            0x0302
#define GL_ONE_MINUS_SRC_ALPHA  0x0303
#define GL_DST_ALPHA            0x0304
#define GL_ONE_MINUS_DST_ALPHA  0x0305
#define GL_DST_COLOR            0x0306
#define GL_ONE_MINUS_DST_COLOR  0x0307
#define GL_SRC_ALPHA_SATURATE   0x0308

/* states (glEnable) */
#define GL_CULL_FACE            0x0B44
#define GL_LIGHTING             0x0B50
#define GL_FOG                  0x0B60
#define GL_DEPTH_TEST           0x0B71
#define GL_STENCIL_TEST         0x0B90
#define GL_NORMALIZE            0x0BA1
#define GL_ALPHA_TEST           0x0BC0
#define GL_DITHER               0x0BD0
#define GL_BLEND                0x0BE2
#define GL_SCISSOR_TEST         0x0C11
#define GL_TEXTURE_1D           0x0DE0
#define GL_TEXTURE_2D           0x0DE1
#define GL_POLYGON_OFFSET_FILL  0x8037
#define GL_COLOR_MATERIAL       0x0B57
#define GL_LINE_SMOOTH          0x0B20
#define GL_POINT_SMOOTH         0x0B10

/* vertex arrays */
#define GL_VERTEX_ARRAY         0x8074
#define GL_NORMAL_ARRAY         0x8075
#define GL_COLOR_ARRAY          0x8076
#define GL_INDEX_ARRAY          0x8077
#define GL_TEXTURE_COORD_ARRAY  0x8078
#define GL_EDGE_FLAG_ARRAY      0x8079

/* faces */
#define GL_FRONT                0x0404
#define GL_BACK                 0x0405
#define GL_FRONT_AND_BACK       0x0408
#define GL_CW                   0x0900
#define GL_CCW                  0x0901

/* matrices */
#define GL_MODELVIEW            0x1700
#define GL_PROJECTION           0x1701
#define GL_TEXTURE              0x1702
#define GL_MATRIX_MODE          0x0BA0
#define GL_MODELVIEW_MATRIX     0x0BA6
#define GL_PROJECTION_MATRIX    0x0BA7
#define GL_TEXTURE_MATRIX       0x0BA8

/* glGet */
#define GL_VIEWPORT             0x0BA2
#define GL_MAX_TEXTURE_SIZE     0x0D33
#define GL_DEPTH_BITS           0x0D56
#define GL_RED_BITS             0x0D52
#define GL_ALPHA_BITS           0x0D55
#define GL_STENCIL_BITS         0x0D57
#define GL_MAX_MODELVIEW_STACK_DEPTH 0x0D36
#define GL_TEXTURE_BINDING_2D   0x8069

/* clear */
#define GL_DEPTH_BUFFER_BIT     0x00000100
#define GL_STENCIL_BUFFER_BIT   0x00000400
#define GL_COLOR_BUFFER_BIT     0x00004000

/* strings */
#define GL_VENDOR               0x1F00
#define GL_RENDERER             0x1F01
#define GL_VERSION              0x1F02
#define GL_EXTENSIONS           0x1F03

/* textures */
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_WRAP_S       0x2802
#define GL_TEXTURE_WRAP_T       0x2803
#define GL_NEAREST              0x2600
#define GL_LINEAR               0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST  0x2701
#define GL_NEAREST_MIPMAP_LINEAR  0x2702
#define GL_LINEAR_MIPMAP_LINEAR   0x2703
#define GL_CLAMP                0x2900
#define GL_REPEAT               0x2901
#define GL_CLAMP_TO_EDGE        0x812F
#define GL_TEXTURE_ENV          0x2300
#define GL_TEXTURE_ENV_MODE     0x2200
#define GL_TEXTURE_ENV_COLOR    0x2201
#define GL_MODULATE             0x2100
#define GL_DECAL                0x2101
#define GL_REPLACE              0x1E01
#define GL_ADD                  0x0104

/* formats */
#define GL_COLOR_INDEX          0x1900
#define GL_ALPHA                0x1906
#define GL_RGB                  0x1907
#define GL_RGBA                 0x1908
#define GL_LUMINANCE            0x1909
#define GL_LUMINANCE_ALPHA      0x190A
#define GL_INTENSITY            0x8049
#define GL_BGRA_EXT             0x80E1
#define GL_UNSIGNED_BYTE        0x1401
#define GL_BYTE                 0x1400
#define GL_SHORT                0x1402
#define GL_UNSIGNED_SHORT       0x1403
#define GL_INT                  0x1404
#define GL_UNSIGNED_INT         0x1405
#define GL_FLOAT                0x1406
#define GL_DOUBLE               0x140A

/* shade, polygon */
#define GL_FLAT                 0x1D00
#define GL_SMOOTH               0x1D01
#define GL_POINT                0x1B00
#define GL_LINE                 0x1B01
#define GL_FILL                 0x1B02

/* fog */
#define GL_FOG_DENSITY          0x0B62
#define GL_FOG_START            0x0B63
#define GL_FOG_END              0x0B64
#define GL_FOG_MODE             0x0B65
#define GL_FOG_COLOR            0x0B66
#define GL_EXP                  0x0800
#define GL_EXP2                 0x0801

/* --- AGL --- */
#define AGL_NONE                0
#define AGL_ALL_RENDERERS       1
#define AGL_BUFFER_SIZE         2
#define AGL_LEVEL               3
#define AGL_RGBA                4
#define AGL_DOUBLEBUFFER        5
#define AGL_STEREO              6
#define AGL_AUX_BUFFERS         7
#define AGL_RED_SIZE            8
#define AGL_GREEN_SIZE          9
#define AGL_BLUE_SIZE           10
#define AGL_ALPHA_SIZE          11
#define AGL_DEPTH_SIZE          12
#define AGL_STENCIL_SIZE        13
#define AGL_ACCUM_RED_SIZE      14
#define AGL_ACCUM_GREEN_SIZE    15
#define AGL_ACCUM_BLUE_SIZE     16
#define AGL_ACCUM_ALPHA_SIZE    17
#define AGL_PIXEL_SIZE          50
#define AGL_MINIMUM_POLICY      51
#define AGL_MAXIMUM_POLICY      52
#define AGL_OFFSCREEN           53
#define AGL_FULLSCREEN          54
#define AGL_RENDERER_ID         70
#define AGL_SINGLE_RENDERER     71
#define AGL_NO_RECOVERY         72
#define AGL_ACCELERATED         73
#define AGL_CLOSEST_POLICY      74
#define AGL_ROBUST              75
#define AGL_BACKING_STORE       76
#define AGL_MP_SAFE             78
#define AGL_WINDOW              80
#define AGL_MULTISCREEN         81
#define AGL_VIRTUAL_SCREEN      82
#define AGL_COMPLIANT           83
#define AGL_BUFFER_MODES        100
#define AGL_MIN_LEVEL           101
#define AGL_MAX_LEVEL           102
#define AGL_COLOR_MODES         103
#define AGL_ACCUM_MODES         104
#define AGL_DEPTH_MODES         105
#define AGL_STENCIL_MODES       106
#define AGL_MAX_AUX_BUFFERS     107
#define AGL_VIDEO_MEMORY        120
#define AGL_TEXTURE_MEMORY      121
#define AGL_SWAP_RECT           200
#define AGL_BUFFER_RECT         202
#define AGL_SWAP_LIMIT          203
#define AGL_SWAP_INTERVAL       222

#endif
