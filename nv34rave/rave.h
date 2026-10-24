/*
 * Minimálne deklarácie QuickDraw 3D RAVE 1.6 pre engine a test (rozhranie podľa verejnej
 * dokumentácie RAVE a Apple „Empty Engine“; Retro68 hlavičku RAVE.h neobsahuje).
 * Zarovnanie a veľkosti zodpovedajú PPC (power alignment, enum = 4 B).
 */
#ifndef NV34_RAVE_H
#define NV34_RAVE_H

#include <Quickdraw.h>

typedef long TQAError;
enum {
    kQANoErr = 0, kQAError = 1, kQAOutOfMemory = 2, kQANotSupported = 3, kQAOutOfDate = 4,
    kQAParamErr = 5, kQAGestaltUnknown = 6, kQADisplayModeUnsupported = 7, kQAOutOfVideoMemory = 8
};

typedef unsigned char TQABoolean;
typedef long TQAImagePixelType;
enum {
    kQAPixel_Alpha1 = 0, kQAPixel_RGB16 = 1, kQAPixel_ARGB16 = 2, kQAPixel_RGB32 = 3,
    kQAPixel_ARGB32 = 4, kQAPixel_CL4 = 5, kQAPixel_CL8 = 6, kQAPixel_RGB16_565 = 7,
    kQAPixel_RGB24 = 8, kQAPixel_RGB8_332 = 9, kQAPixel_ARGB16_4444 = 10, kQAPixel_ACL16_88 = 11,
    kQAPixel_I8 = 12, kQAPixel_AI16_88 = 13
};
typedef long TQAColorTableType;
enum { kQAColorTable_CL8_RGB32 = 0, kQAColorTable_CL4_RGB32 = 1 };

enum { kQADeviceMemory = 0, kQADeviceGDevice = 1 };
typedef struct TQADeviceMemory {
    long rowBytes;
    TQAImagePixelType pixelType;
    long width, height;
    void *baseAddr;
} TQADeviceMemory;
typedef struct TQADevice {
    long deviceType;
    union {
        TQADeviceMemory memoryDevice;
        GDHandle gDevice;
    } device;
} TQADevice;
enum { kQAClipRgn = 0 };
typedef struct TQAClip {
    long clipType;
    union { RgnHandle clipRgn; } clip;
} TQAClip;
typedef struct TQARect { long left, right, top, bottom; } TQARect;

typedef struct TQAEngine TQAEngine;
typedef struct TQATexture TQATexture;
typedef struct TQABitmap TQABitmap;
typedef struct TQADrawPrivate TQADrawPrivate;
typedef struct TQAColorTable TQAColorTable;

typedef struct TQAIndexedTriangle { unsigned long triangleFlags; unsigned long vertices[3]; } TQAIndexedTriangle;
typedef struct TQAImage { long width, height, rowBytes; void *pixmap; } TQAImage;
typedef TQADeviceMemory TQAPixelBuffer;
typedef struct TQAZBuffer { long width, height, rowBytes; void *zbuffer; long zDepth, isBigEndian; } TQAZBuffer;

typedef struct TQAVGouraud { float x, y, z, invW, r, g, b, a; } TQAVGouraud;
typedef struct TQAVTexture {
    float x, y, z, invW, r, g, b, a, uOverW, vOverW, kd_r, kd_g, kd_b, ks_r, ks_g, ks_b;
} TQAVTexture;
typedef struct TQAVMultiTexture { float invW, uOverW, vOverW; } TQAVMultiTexture;

/* tagy */
enum {
    kQATag_ZFunction = 0, kQATag_ColorBG_a = 1, kQATag_ColorBG_r = 2, kQATag_ColorBG_g = 3,
    kQATag_ColorBG_b = 4, kQATag_Width = 5, kQATag_ZMinOffset = 6, kQATag_ZMinScale = 7,
    kQATag_Antialias = 8, kQATag_Blend = 9, kQATag_PerspectiveZ = 10, kQATag_TextureFilter = 11,
    kQATag_TextureOp = 12, kQATag_Texture = 13, kQATag_CSGTag = 14, kQATag_CSGEquation = 15,
    kQATag_BufferComposite = 16, kQATag_FogMode = 17, kQATag_FogColor_a = 18, kQATag_FogColor_r = 19,
    kQATag_FogColor_g = 20, kQATag_FogColor_b = 21, kQATag_FogStart = 22, kQATag_FogEnd = 23,
    kQATag_FogDensity = 24, kQATag_FogMaxDepth = 25, kQATag_MultiTexture = 26, kQATag_ChannelMask = 27,
    kQATag_ZBufferMask = 28, kQATag_ZSortedHint = 29, kQATag_ChromakeyEnable = 30,
    kQATag_AlphaTestFunc = 31, kQATag_DontSwap = 32, kQATag_MultiTextureEnable = 33,
    kQATag_MipmapBias = 41, kQATag_AlphaTestRef = 46, kQATag_BitmapScale_x = 52,
    kQATag_BitmapScale_y = 53, kQATag_BitmapFilter = 54, kQATag_DrawContextFilter = 55,
    kQATagGL_DrawBuffer = 100, kQATagGL_TextureWrapU = 101, kQATagGL_TextureWrapV = 102,
    kQATagGL_TextureMagFilter = 103, kQATagGL_TextureMinFilter = 104, kQATagGL_ScissorXMin = 105,
    kQATagGL_ScissorYMin = 106, kQATagGL_ScissorXMax = 107, kQATagGL_ScissorYMax = 108,
    kQATagGL_BlendSrc = 109, kQATagGL_BlendDst = 110, kQATagGL_LinePattern = 111,
    kQATagGL_DepthBG = 112, kQATagGL_TextureBorder_a = 113, kQATagGL_AreaPattern0 = 117,
    kQATagGL_LinePatternFactor = 149, kQATagGL_TextureEnvColor_a = 150,
    kQATag_EngineSpecific_Minimum = 1000
};
enum { kQAZFunction_None = 0, kQAZFunction_LT, kQAZFunction_EQ, kQAZFunction_LE, kQAZFunction_GT,
       kQAZFunction_NE, kQAZFunction_GE, kQAZFunction_True, kQAZFunction_False };
enum { kQAAntiAlias_Off = 0, kQAAntiAlias_Fast, kQAAntiAlias_Mid, kQAAntiAlias_Best };
enum { kQABlend_PreMultiply = 0, kQABlend_Interpolate = 1, kQABlend_OpenGL = 2 };
enum { kQAPerspectiveZ_Off = 0, kQAPerspectiveZ_On = 1 };
enum { kQATextureFilter_Fast = 0, kQATextureFilter_Mid = 1, kQATextureFilter_Best = 2 };
enum { kQATextureOp_None = 0, kQATextureOp_Modulate = 1, kQATextureOp_Highlight = 2,
       kQATextureOp_Decal = 4, kQATextureOp_Shrink = 8, kQATextureOp_Blend = 16 };
enum { kQAGL_Repeat = 0, kQAGL_Clamp = 1 };
enum { kQAVertexMode_Point = 0, kQAVertexMode_Line, kQAVertexMode_Polyline, kQAVertexMode_Tri,
       kQAVertexMode_Strip, kQAVertexMode_Fan };
enum { kQATriFlags_None = 0, kQATriFlags_Backfacing = 1 };
enum { kQATexture_Lock = 1, kQATexture_Mipmap = 2, kQATexture_NoCopy = 32, kQATexture_FlipOrigin = 64 };
enum { kQABitmap_Lock = 2, kQABitmap_NoCopy = 32, kQABitmap_FlipOrigin = 64 };
enum { kQAContext_NoZBuffer = 1, kQAContext_DeepZ = 2, kQAContext_DoubleBuffer = 4,
       kQAContext_Cache = 8, kQAContext_NoDither = 16 };

enum {
    kQAGestalt_OptionalFeatures = 0, kQAGestalt_FastFeatures = 1, kQAGestalt_VendorID = 2,
    kQAGestalt_EngineID = 3, kQAGestalt_Revision = 4, kQAGestalt_ASCIINameLength = 5,
    kQAGestalt_ASCIIName = 6, kQAGestalt_TextureMemory = 7, kQAGestalt_FastTextureMemory = 8,
    kQAGestalt_DrawContextPixelTypesAllowed = 9, kQAGestalt_DrawContextPixelTypesPreferred = 10,
    kQAGestalt_TexturePixelTypesAllowed = 11, kQAGestalt_TexturePixelTypesPreferred = 12,
    kQAGestalt_BitmapPixelTypesAllowed = 13, kQAGestalt_BitmapPixelTypesPreferred = 14,
    kQAGestalt_OptionalFeatures2 = 15, kQAGestalt_MultiTextureMax = 16
};
enum {
    kQAOptional_DeepZ = 1 << 0, kQAOptional_Texture = 1 << 1, kQAOptional_TextureHQ = 1 << 2,
    kQAOptional_TextureColor = 1 << 3, kQAOptional_Blend = 1 << 4, kQAOptional_BlendAlpha = 1 << 5,
    kQAOptional_Antialias = 1 << 6, kQAOptional_ZSorted = 1 << 7, kQAOptional_PerspectiveZ = 1 << 8,
    kQAOptional_OpenGL = 1 << 9, kQAOptional_NoClear = 1 << 10, kQAOptional_CSG = 1 << 11,
    kQAOptional_BoundToDevice = 1 << 12, kQAOptional_CL4 = 1 << 13, kQAOptional_CL8 = 1 << 14,
    kQAOptional_BufferComposite = 1 << 15, kQAOptional_NoDither = 1 << 16,
    kQAOptional_FogAlpha = 1 << 17, kQAOptional_FogDepth = 1 << 18,
    kQAOptional_MultiTextures = 1 << 19, kQAOptional_MipmapBias = 1 << 20,
    kQAOptional_ChannelMask = 1 << 21, kQAOptional_ZBufferMask = 1 << 22,
    kQAOptional_AlphaTest = 1 << 23, kQAOptional_AccessTexture = 1 << 24,
    kQAOptional_AccessBitmap = 1 << 25, kQAOptional_AccessDrawBuffer = 1 << 26,
    kQAOptional_AccessZBuffer = 1 << 27, kQAOptional_ClearDrawBuffer = 1 << 28,
    kQAOptional_ClearZBuffer = 1 << 29, kQAOptional_OffscreenDrawContexts = 1 << 30
};
enum { kQAOptional2_Busy = 1 << 3, kQAOptional2_SwapBuffers = 1 << 4 };
enum {
    kQAFast_Line = 1 << 0, kQAFast_Gouraud = 1 << 1, kQAFast_Texture = 1 << 2,
    kQAFast_TextureHQ = 1 << 3, kQAFast_Blend = 1 << 4
};
enum { kQAVersion_1_6 = 4 };
enum { kQAMethod_RenderCompletion = 0, kQAMethod_DisplayModeChanged = 1, kQAMethod_ReloadTextures = 2,
       kQAMethod_BufferInitialize = 3, kQAMethod_BufferComposite = 4 };

typedef struct TQADrawContext TQADrawContext;

typedef void (*TQAStandardNoticeMethod)(const TQADrawContext *, void *refCon);
typedef void (*TQABufferNoticeMethod)(const TQADrawContext *, const TQADevice *, const TQARect *, void *refCon);
typedef union TQANoticeMethod {
    TQAStandardNoticeMethod standardNoticeMethod;
    TQABufferNoticeMethod bufferNoticeMethod;
} TQANoticeMethod;

struct TQADrawContext {
    TQADrawPrivate *drawPrivate;
    long version;
    void (*setFloat)(TQADrawContext *, long tag, float v);
    void (*setInt)(TQADrawContext *, long tag, unsigned long v);
    void (*setPtr)(TQADrawContext *, long tag, const void *v);
    float (*getFloat)(const TQADrawContext *, long tag);
    unsigned long (*getInt)(const TQADrawContext *, long tag);
    void *(*getPtr)(const TQADrawContext *, long tag);
    void (*drawPoint)(const TQADrawContext *, const TQAVGouraud *);
    void (*drawLine)(const TQADrawContext *, const TQAVGouraud *, const TQAVGouraud *);
    void (*drawTriGouraud)(const TQADrawContext *, const TQAVGouraud *, const TQAVGouraud *, const TQAVGouraud *, unsigned long);
    void (*drawTriTexture)(const TQADrawContext *, const TQAVTexture *, const TQAVTexture *, const TQAVTexture *, unsigned long);
    void (*drawVGouraud)(const TQADrawContext *, unsigned long n, long mode, const TQAVGouraud v[], const unsigned long flags[]);
    void (*drawVTexture)(const TQADrawContext *, unsigned long n, long mode, const TQAVTexture v[], const unsigned long flags[]);
    void (*drawBitmap)(const TQADrawContext *, const TQAVGouraud *, TQABitmap *);
    void (*renderStart)(const TQADrawContext *, const TQARect *dirty, const TQADrawContext *initial);
    TQAError (*renderEnd)(const TQADrawContext *, const TQARect *modified);
    TQAError (*renderAbort)(const TQADrawContext *);
    TQAError (*flush)(const TQADrawContext *);
    TQAError (*sync)(const TQADrawContext *);
    void (*submitVerticesGouraud)(const TQADrawContext *, unsigned long n, const TQAVGouraud *);
    void (*submitVerticesTexture)(const TQADrawContext *, unsigned long n, const TQAVTexture *);
    void (*drawTriMeshGouraud)(const TQADrawContext *, unsigned long n, const TQAIndexedTriangle *);
    void (*drawTriMeshTexture)(const TQADrawContext *, unsigned long n, const TQAIndexedTriangle *);
    TQAError (*setNoticeMethod)(const TQADrawContext *, long method, TQANoticeMethod cb, void *refCon);
    TQAError (*getNoticeMethod)(const TQADrawContext *, long method, TQANoticeMethod *cb, void **refCon);
    void (*submitMultiTextureParams)(const TQADrawContext *, unsigned long n, const TQAVMultiTexture *);
    TQAError (*accessDrawBuffer)(const TQADrawContext *, TQAPixelBuffer *);
    TQAError (*accessDrawBufferEnd)(const TQADrawContext *, const TQARect *);
    TQAError (*accessZBuffer)(const TQADrawContext *, TQAZBuffer *);
    TQAError (*accessZBufferEnd)(const TQADrawContext *, const TQARect *);
    TQAError (*clearDrawBuffer)(const TQADrawContext *, const TQARect *, const TQADrawContext *);
    TQAError (*clearZBuffer)(const TQADrawContext *, const TQARect *, const TQADrawContext *);
    TQAError (*textureFromContext)(const TQADrawContext *, unsigned long, TQATexture **);
    TQAError (*bitmapFromContext)(const TQADrawContext *, unsigned long, TQABitmap **);
    TQABoolean (*busy)(const TQADrawContext *);
    TQAError (*swapBuffers)(const TQADrawContext *, const TQARect *);
};

/* metódy enginu (RAVESystem.h) */
typedef union TQAEngineMethod {
    TQAError (*drawPrivateNew)(TQADrawContext *, const TQADevice *, const TQARect *, const TQAClip *, unsigned long flags);
    void (*drawPrivateDelete)(TQADrawPrivate *);
    TQAError (*engineCheckDevice)(const TQADevice *);
    TQAError (*engineGestalt)(long selector, void *response);
    TQAError (*textureNew)(unsigned long flags, TQAImagePixelType, const TQAImage images[], TQATexture **);
    TQAError (*textureDetach)(TQATexture *);
    void (*textureDelete)(TQATexture *);
    TQAError (*bitmapNew)(unsigned long flags, TQAImagePixelType, const TQAImage *, TQABitmap **);
    TQAError (*bitmapDetach)(TQABitmap *);
    void (*bitmapDelete)(TQABitmap *);
    TQAError (*colorTableNew)(TQAColorTableType, void *pixelData, long transparentIndex, TQAColorTable **);
    void (*colorTableDelete)(TQAColorTable *);
    TQAError (*textureBindColorTable)(TQATexture *, TQAColorTable *);
    TQAError (*bitmapBindColorTable)(TQABitmap *, TQAColorTable *);
    TQAError (*accessTexture)(TQATexture *, long mipmapLevel, long flags, TQAPixelBuffer *);
    TQAError (*accessTextureEnd)(TQATexture *, const TQARect *);
    TQAError (*accessBitmap)(TQABitmap *, long flags, TQAPixelBuffer *);
    TQAError (*accessBitmapEnd)(TQABitmap *, const TQARect *);
} TQAEngineMethod;
enum {
    kQADrawPrivateNew = 0, kQADrawPrivateDelete, kQAEngineCheckDevice, kQAEngineGestalt,
    kQATextureNew, kQATextureDetach, kQATextureDelete, kQABitmapNew, kQABitmapDetach,
    kQABitmapDelete, kQAColorTableNew, kQAColorTableDelete, kQATextureBindColorTable,
    kQABitmapBindColorTable, kQAAccessTexture, kQAAccessTextureEnd, kQAAccessBitmap, kQAAccessBitmapEnd
};
typedef TQAError (*TQAEngineGetMethod)(long methodTag, TQAEngineMethod *method);

/* knižnica „QuickDraw™ 3D Accelerator“ (libQuickDraw3DRAVELib) */
extern TQAError QARegisterEngine(TQAEngineGetMethod);
extern TQAError QARegisterEngineWithRefCon(TQAEngineGetMethod, long refCon);
extern TQAEngine *QADeviceGetFirstEngine(const TQADevice *);
extern TQAEngine *QADeviceGetNextEngine(const TQADevice *, TQAEngine *);
extern TQAError QAEngineCheckDevice(const TQAEngine *, const TQADevice *);
extern TQAError QAEngineGestalt(const TQAEngine *, long selector, void *response);
extern TQAError QADrawContextNew(const TQADevice *, const TQARect *, const TQAClip *, const TQAEngine *,
                                 unsigned long flags, TQADrawContext **);
extern void QADrawContextDelete(TQADrawContext *);
extern TQAError QATextureNew(const TQAEngine *, unsigned long flags, TQAImagePixelType, const TQAImage images[], TQATexture **);
extern TQAError QATextureDetach(const TQAEngine *, TQATexture *);
extern void QATextureDelete(const TQAEngine *, TQATexture *);
extern TQAError QABitmapNew(const TQAEngine *, unsigned long flags, TQAImagePixelType, const TQAImage *, TQABitmap **);
extern void QABitmapDelete(const TQAEngine *, TQABitmap *);

#endif
