/*
 * Minimal subset of Apple Universal Interfaces 3.4 for a native video driver
 * (Devices.h, Video.h, NameRegistry.h, DriverServices.h, VideoServices.h).
 *
 * Retro68's Multiversal Interfaces lack these headers, so they are rewritten here
 * by hand. Toolbox structures use mac68k alignment (= #pragma pack(2)).
 */
#ifndef NDRV_API_H
#define NDRV_API_H

typedef unsigned char       UInt8;
typedef signed char         SInt8;
typedef unsigned short      UInt16;
typedef signed short        SInt16;
typedef unsigned long       UInt32;
typedef signed long         SInt32;
typedef unsigned char       Boolean;
typedef SInt16              OSErr;
typedef SInt32              OSStatus;
typedef UInt32              OSType;
typedef char *              Ptr;
typedef UInt8 *             StringPtr;
typedef SInt32              Fixed;
typedef UInt8               Str31[32];
typedef SInt32              Duration;

#define FOURCC(a, b, c, d) (((UInt32)(a) << 24) | ((UInt32)(b) << 16) | ((UInt32)(c) << 8) | (UInt32)(d))

enum {
    noErr           = 0,
    controlErr      = -17,
    statusErr       = -18,
    paramErr        = -50,
    memFullErr      = -108,
    nrNotFoundErr   = -2539,
    kIOBusyStatus   = 1
};

#pragma pack(push, 2)

typedef struct NumVersion {
    UInt8 majorRev;
    UInt8 minorAndBugRev;
    UInt8 stage;
    UInt8 nonRelRev;
} NumVersion;

enum { developStage = 0x20, alphaStage = 0x40, betaStage = 0x60, finalStage = 0x80 };

typedef struct AbsoluteTime {
    UInt32 hi;
    UInt32 lo;
} AbsoluteTime;

/* ---- NameRegistry.h ---------------------------------------------------- */

typedef struct RegEntryID {
    UInt32 contents[4];
} RegEntryID;

typedef char        RegCStrPathName;
typedef UInt32      RegPathNameSize;
typedef char        RegPropertyName;
typedef UInt32      RegPropertyValueSize;
typedef UInt32      RegPropertyModifiers;

/* ---- Devices.h / DriverServices.h --------------------------------------- */

typedef SInt16                      DriverRefNum;
typedef UInt32                      AddressSpaceID;
typedef struct OpaqueIOCommandID *  IOCommandID;
typedef UInt32                      IOCommandCode;
typedef UInt32                      IOCommandKind;

enum {
    kOpenCommand        = 0,
    kCloseCommand       = 1,
    kReadCommand        = 2,
    kWriteCommand       = 3,
    kControlCommand     = 4,
    kStatusCommand      = 5,
    kKillIOCommand      = 6,
    kInitializeCommand  = 7,
    kFinalizeCommand    = 8,
    kReplaceCommand     = 9,
    kSupersededCommand  = 10,
    kSuspendCommand     = 11,
    kResumeCommand      = 12
};

enum {
    kSynchronousIOCommandKind   = 0x00000001,
    kAsynchronousIOCommandKind  = 0x00000002,
    kImmediateIOCommandKind     = 0x00000004
};

typedef struct DriverInitInfo {
    DriverRefNum refNum;
    RegEntryID   deviceEntry;
} DriverInitInfo, DriverReplaceInfo, DriverFinalInfo, DriverSupersededInfo;

typedef struct CntrlParam {
    void *      qLink;
    SInt16      qType;
    SInt16      ioTrap;
    Ptr         ioCmdAddr;
    void *      ioCompletion;
    OSErr       ioResult;
    StringPtr   ioNamePtr;
    SInt16      ioVRefNum;
    SInt16      ioCRefNum;
    SInt16      csCode;
    SInt16      csParam[11];
} CntrlParam;

/* Video csParam starts with a pointer to the record (VDPageInfo, ...) */
static inline void *CS_PARAM_PTR(void *pb)
{
    const SInt16 *w = ((CntrlParam *)pb)->csParam;
    return (void *)(((UInt32)(UInt16)w[0] << 16) | (UInt16)w[1]);
}

typedef struct DriverType {
    Str31       nameInfoStr;
    NumVersion  version;
} DriverType;

typedef struct DriverOSRuntime {
    UInt32  driverRuntime;
    Str31   driverName;
    UInt32  driverDescReserved[8];
} DriverOSRuntime;

typedef struct DriverServiceInfo {
    OSType      serviceCategory;
    OSType      serviceType;
    NumVersion  serviceVersion;
} DriverServiceInfo;

typedef struct DriverOSService {
    UInt32              nServices;
    DriverServiceInfo   service[1];
} DriverOSService;

typedef struct DriverDescription {
    OSType          driverDescSignature;
    UInt32          driverDescVersion;
    DriverType      driverType;
    DriverOSRuntime driverOSRuntime;
    DriverOSService driverServices;
} DriverDescription;

enum {
    kDriverIsLoadedUponDiscovery    = 0x00000001,
    kDriverIsOpenedUponLoad         = 0x00000002,
    kDriverIsUnderExpertControl     = 0x00000004,
    kDriverIsConcurrent             = 0x00000008,
    kDriverQueuesIOPB               = 0x00000010
};

#define kTheDescriptionSignature    FOURCC('m', 't', 'e', 'j')
#define kInitialDriverDescriptor    0
#define kServiceCategoryNdrvDriver  FOURCC('n', 'd', 'r', 'v')
#define kNdrvTypeIsVideo            FOURCC('v', 'i', 'd', 'o')

/* ---- Video.h ----------------------------------------------------------- */

enum {
    /* control */
    cscReset                    = 0,
    cscKillIO                   = 1,
    cscSetMode                  = 2,
    cscSetEntries               = 3,
    cscSetGamma                 = 4,
    cscGrayPage                 = 5,
    cscSetGray                  = 6,
    cscSetInterrupt             = 7,
    cscDirectSetEntries         = 8,
    cscSetDefaultMode           = 9,
    cscSwitchMode               = 10,
    cscSetSync                  = 11,
    cscSavePreferredConfiguration = 16,
    cscSetHardwareCursor        = 22,
    cscDrawHardwareCursor       = 23,
    cscSetPowerState            = 25,
    cscSetClutBehavior          = 29,
    /* status */
    cscGetMode                  = 2,
    cscGetEntries               = 3,
    cscGetPages                 = 4,
    cscGetBaseAddr              = 5,
    cscGetGray                  = 6,
    cscGetInterrupt             = 7,
    cscGetGamma                 = 8,
    cscGetDefaultMode           = 9,
    cscGetCurMode               = 10,
    cscGetSync                  = 11,
    cscGetConnection            = 12,
    cscGetModeTiming            = 13,
    cscGetModeBaseAddress       = 14,
    cscGetPreferredConfiguration = 16,
    cscGetNextResolution        = 17,
    cscGetVideoParameters       = 18,
    cscGetGammaInfoList         = 20,
    cscSupportsHardwareCursor   = 22,
    cscGetHardwareCursorDrawState = 23,
    cscGetPowerState            = 25,
    cscGetDDCBlock              = 27,
    cscGetClutBehavior          = 29
};

typedef UInt32 DisplayModeID;
typedef UInt16 DepthMode;

enum {
    kDisplayModeIDCurrent               = 0x00000000,
    kDisplayModeIDInvalid               = 0xFFFFFFFF,
    kDisplayModeIDFindFirstResolution   = 0xFFFFFFFE,
    kDisplayModeIDNoMoreResolutions     = 0xFFFFFFFD
};

enum {
    kDepthMode1 = 128,
    kDepthMode2 = 129,
    kDepthMode3 = 130
};

enum {
    clutType    = 0,
    fixedType   = 1,
    directType  = 2
};

enum {
    kModeValid      = 0,
    kModeSafe       = 1,
    kModeDefault    = 2,
    kModeShowNow    = 3,
    kModeNotResize  = 4,
    kModeBuiltIn    = 10,
    kModeStretched  = 11
};

#define kDeclROMtables          FOURCC('d', 'e', 'c', 'l')

enum {
    timingApple_FixedRateLCD    = 42,
    timingVESA_640x480_60hz     = 150,
    timingVESA_800x600_60hz     = 182,
    timingVESA_1024x768_60hz    = 190
};

enum {
    kPanelTFTConnect    = 2,
    kGenericLCD         = 20
};

enum {
    kAllModesValid          = 0,
    kAllModesSafe           = 1,
    kHasDirectConnection    = 3,
    kBuiltInConnection      = 11
};

enum {
    kAVPowerOff     = 0,
    kAVPowerStandby = 1,
    kAVPowerSuspend = 2,
    kAVPowerOn      = 3
};

typedef struct VDPageInfo {
    SInt16  csMode;
    SInt32  csData;
    SInt16  csPage;
    Ptr     csBaseAddr;
} VDPageInfo;

typedef struct ColorSpec {
    SInt16  value;
    UInt16  red;
    UInt16  green;
    UInt16  blue;
} ColorSpec;

typedef struct VDSetEntryRecord {
    ColorSpec * csTable;
    SInt16      csStart;
    SInt16      csCount;
} VDSetEntryRecord;

typedef struct VDGammaRecord {
    Ptr csGTable;
} VDGammaRecord;

typedef struct GammaTbl {
    SInt16 gVersion;
    SInt16 gType;
    SInt16 gFormulaSize;
    SInt16 gChanCnt;
    SInt16 gDataCnt;
    SInt16 gDataWidth;
    SInt16 gFormulaData[1];
} GammaTbl;

typedef struct VDGrayRecord {
    Boolean csMode;
    SInt8   filler;
} VDGrayRecord;

typedef struct VDFlagRecord {
    SInt8   csMode;
    SInt8   filler;
} VDFlagRecord;

typedef struct VDDefModeRec {
    SInt8   csID;
    SInt8   filler;
} VDDefModeRec;

typedef struct VDSyncInfoRec {
    UInt8   csMode;
    UInt8   csFlags;
} VDSyncInfoRec;

typedef struct VDSwitchInfoRec {
    UInt16  csMode;
    UInt32  csData;
    UInt16  csPage;
    Ptr     csBaseAddr;
    UInt32  csReserved;
} VDSwitchInfoRec;

typedef struct VDTimingInfoRec {
    UInt32  csTimingMode;
    UInt32  csTimingReserved;
    UInt32  csTimingFormat;
    UInt32  csTimingData;
    UInt32  csTimingFlags;
} VDTimingInfoRec;

typedef struct VDDisplayConnectInfoRec {
    UInt16  csDisplayType;
    UInt8   csConnectTaggedType;
    UInt8   csConnectTaggedData;
    UInt32  csConnectFlags;
    UInt32  csDisplayComponent;
    UInt32  csConnectReserved;
} VDDisplayConnectInfoRec;

typedef struct VDResolutionInfoRec {
    DisplayModeID   csPreviousDisplayModeID;
    DisplayModeID   csDisplayModeID;
    UInt32          csHorizontalPixels;
    UInt32          csVerticalLines;
    Fixed           csRefreshRate;
    DepthMode       csMaxDepthMode;
    UInt32          csResolutionReserved;
    UInt32          csReserved;
} VDResolutionInfoRec;

typedef struct Rect {
    SInt16 top, left, bottom, right;
} Rect;

typedef struct VPBlock {
    SInt32  vpBaseOffset;
    SInt16  vpRowBytes;
    Rect    vpBounds;
    SInt16  vpVersion;
    SInt16  vpPackType;
    SInt32  vpPackSize;
    Fixed   vpHRes;
    Fixed   vpVRes;
    SInt16  vpPixelType;
    SInt16  vpPixelSize;
    SInt16  vpCmpCount;
    SInt16  vpCmpSize;
    SInt32  vpPlaneBytes;
} VPBlock;

typedef struct VDVideoParametersInfoRec {
    DisplayModeID   csDisplayModeID;
    DepthMode       csDepthMode;
    VPBlock *       csVPBlockPtr;
    UInt32          csPageCount;
    UInt32          csDeviceType;
    UInt32          csDepthFlags;
} VDVideoParametersInfoRec;

typedef struct VDPowerStateRec {
    UInt32  powerState;
    UInt32  powerFlags;
    UInt32  powerReserved1;
    UInt32  powerReserved2;
} VDPowerStateRec;

typedef struct VDClutBehaviorRec {
    UInt32  csClutBehavior;
} VDClutBehaviorRec;

#pragma pack(pop)

/* Layout check against Universal Interfaces (mac68k alignment). */
#define NDRV_STATIC_ASSERT(c, n) typedef char ndrv_assert_##n[(c) ? 1 : -1]
NDRV_STATIC_ASSERT(sizeof(DriverInitInfo) == 18, DriverInitInfo);
NDRV_STATIC_ASSERT(sizeof(CntrlParam) == 50, CntrlParam);
NDRV_STATIC_ASSERT(sizeof(VDPageInfo) == 12, VDPageInfo);
NDRV_STATIC_ASSERT(sizeof(VDSwitchInfoRec) == 16, VDSwitchInfoRec);
NDRV_STATIC_ASSERT(sizeof(VDTimingInfoRec) == 20, VDTimingInfoRec);
NDRV_STATIC_ASSERT(sizeof(VDResolutionInfoRec) == 30, VDResolutionInfoRec);
NDRV_STATIC_ASSERT(sizeof(VPBlock) == 42, VPBlock);
NDRV_STATIC_ASSERT(sizeof(VDVideoParametersInfoRec) == 22, VDVideoParametersInfoRec);
NDRV_STATIC_ASSERT(sizeof(VDDisplayConnectInfoRec) == 16, VDDisplayConnectInfoRec);
NDRV_STATIC_ASSERT(sizeof(DriverDescription) == 128, DriverDescription);

/* ---- imports: DriverServicesLib ---------------------------------------- */

typedef OSStatus (*SecondaryInterruptHandler2)(void *p1, void *p2);
typedef struct OpaqueTimerID *TimerID;

enum { durationMillisecond = 1 };

extern OSStatus IOCommandIsComplete(IOCommandID theID, OSErr theResult);
extern void     SynchronizeIO(void);
extern AbsoluteTime UpTime(void);
extern AbsoluteTime AddDurationToAbsolute(Duration duration, AbsoluteTime absoluteTime);
extern OSStatus SetInterruptTimer(const AbsoluteTime *expirationTime, SecondaryInterruptHandler2 handler,
                                  void *p1, TimerID *theTimer);
extern OSStatus CancelTimer(TimerID theTimer, AbsoluteTime *timeRemaining);
extern OSStatus DelayForHardware(AbsoluteTime absoluteTime);
extern AbsoluteTime DurationToAbsolute(Duration duration);
extern void     BlockCopy(const void *srcPtr, void *destPtr, UInt32 byteCount);

typedef UInt32 ProcessorCacheMode;
enum {
    kProcessorCacheModeDefault      = 0,
    kProcessorCacheModeInhibited    = 1,
    kProcessorCacheModeWriteThrough = 2,
    kProcessorCacheModeCopyBack     = 3
};
#define kCurrentAddressSpaceID 0

extern void     SetProcessorCacheMode(AddressSpaceID spaceID, void *address, UInt32 length, ProcessorCacheMode cacheMode);

/* ---- imports: NameRegistryLib ------------------------------------------ */

extern OSStatus RegistryEntryIDCopy(const RegEntryID *src, RegEntryID *dst);
extern OSStatus RegistryEntryIDDispose(RegEntryID *id);
extern OSStatus RegistryPropertyGetSize(const RegEntryID *e, const RegPropertyName *name, RegPropertyValueSize *size);
extern OSStatus RegistryPropertyGet(const RegEntryID *e, const RegPropertyName *name, void *value, RegPropertyValueSize *size);
extern OSStatus RegistryPropertyCreate(const RegEntryID *e, const RegPropertyName *name, const void *value, RegPropertyValueSize size);
extern OSStatus RegistryPropertySet(const RegEntryID *e, const RegPropertyName *name, const void *value, RegPropertyValueSize size);
extern OSStatus RegistryEntryToPathSize(const RegEntryID *e, RegPathNameSize *size);
extern OSStatus RegistryCStrEntryToPath(const RegEntryID *e, RegCStrPathName *path, RegPathNameSize size);
extern OSStatus RegistryCStrEntryLookup(const RegEntryID *searchPoint, const RegCStrPathName *path, RegEntryID *found);

/* ---- imports: VideoServicesLib ----------------------------------------- */

typedef UInt32 InterruptServiceIDType;
#define kVBLInterruptServiceType FOURCC('v', 'b', 'l', ' ')

extern OSErr VSLNewInterruptService(RegEntryID *serviceDevice, OSType serviceType, InterruptServiceIDType *serviceID);
extern OSErr VSLDisposeInterruptService(InterruptServiceIDType serviceID);
extern OSErr VSLDoInterruptService(InterruptServiceIDType serviceID);

#endif
