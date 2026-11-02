/*
 * NV34 MiniGL — OpenGL 1.1 stavový stroj.
 *
 * glBegin/glEnd zbiera vrcholy v clip súradniciach (MVP na CPU), pri glEnd sa poskladajú
 * trojuholníky, orežú blízkou rovinou (a ochranným pásom), premietnu do okna, odvrátené sa
 * zahodia a výsledok ide do dávky. Dávka sa pošle GPU pri zmene stavu (každá funkcia meniaca
 * stav volá FLUSH) alebo pri plnej dávke — Quake 2 kreslí každý polygón zvlášť.
 */
#include <MacMemory.h>
#include <Files.h>
#include <Events.h>
#include <stdarg.h>
#include "minigl.h"

typedef SInt32 OSStatus;
typedef struct RegEntryID { UInt32 contents[4]; } RegEntryID;
extern OSStatus RegistryCStrEntryLookup(const RegEntryID *start, const char *path, RegEntryID *found);
extern OSStatus RegistryPropertyGet(const RegEntryID *e, const char *name, void *value, UInt32 *size);
extern OSStatus RegistryEntryIDDispose(RegEntryID *id);

NV34Shared *S;
GLCtx *C;

/* ---- bez libc ------------------------------------------------------------ */

void *memset(void *d, int c, unsigned long n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

/* opt-4: kompilátor ním kopíruje štruktúry vrcholov (40–44 B) → po slovách, ak sú zarovnané */
void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if (!(((unsigned long)p | (unsigned long)q) & 3)) {
        unsigned long *pw = (unsigned long *)p;
        const unsigned long *qw = (const unsigned long *)q;
        while (n >= 16) {
            pw[0] = qw[0]; pw[1] = qw[1]; pw[2] = qw[2]; pw[3] = qw[3];
            pw += 4; qw += 4; n -= 16;
        }
        while (n >= 4) {
            *pw++ = *qw++;
            n -= 4;
        }
        p = (unsigned char *)pw;
        q = (const unsigned char *)qw;
    }
    while (n--)
        *p++ = *q++;
    return d;
}


static double sqrt_(double x)
{
    double r;
    int i;
    if (x <= 0)
        return 0;
    r = x > 1 ? x : 1;
    for (i = 0; i < 40; i++)
        r = 0.5 * (r + x / r);
    return r;
}

/* sin/cos pre glRotate (Taylor po redukcii do [-pi, pi]) */
static double sin_(double x)
{
    const double PI = 3.14159265358979323846;
    double x2, t, s;
    int i;
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;
    x2 = x * x;
    t = x;
    s = x;
    for (i = 1; i < 12; i++) {
        t *= -x2 / ((2 * i) * (2 * i + 1));
        s += t;
    }
    return s;
}

static double cos_(double x) { return sin_(x + 1.57079632679489661923); }

/* ---- zdieľaný stav a log ---------------------------------------------------- */

int gDbg;
static float gGuard = 1.01f;                /* ochranný pás orezávania (wolf-5) */
static int gTexDbg, gTexDbgN;               /* q3-1: boot-args nv34gltex */
static int gStatsOn;                /* boot-args nv34glstats: časy každých 250 snímok do logu */
static int gMT = 1;                 /* q3-12: GL_ARB_multitexture (boot-args nv34glnomt = vypnuté) */
static int gNoFpg;                  /* max-4: boot-args nv34glnofpg = texenv bez fragment programov */
static int gTnl;                    /* q3-16: HW T&L v glDrawElements (q3-17 predvolené, nv34glnotnl = vypnuté) */

/* cfg-1: konfiguračný súbor „NV34 MiniGL config“ vedľa OpenGLLibrary — príznaky bez reštartu
 * a boot-args. Slová oddelené medzerou alebo novým riadkom, # = poznámka do konca riadku;
 * predpona nv34gl sa môže vynechať (showfps = nv34glshowfps). Platí boot-arg ALEBO config. */
#define CFG_MAX 2048
static char gCfg[CFG_MAX + 1];
static int gCfgLen = -1;

static int cfg_has(const char *word)
{
    int i = 0, j, k;
    if (gCfgLen <= 0)
        return 0;
    while (i < gCfgLen) {
        while (i < gCfgLen && (gCfg[i] == ' ' || gCfg[i] == '\t' || gCfg[i] == '\r' || gCfg[i] == '\n'))
            i++;
        if (i >= gCfgLen)
            break;
        if (gCfg[i] == '#') {
            while (i < gCfgLen && gCfg[i] != '\r' && gCfg[i] != '\n')
                i++;
            continue;
        }
        j = i;
        while (j < gCfgLen && gCfg[j] != ' ' && gCfg[j] != '\t' && gCfg[j] != '\r' && gCfg[j] != '\n' && gCfg[j] != '#')
            j++;
        for (k = 0; k < 2; k++) {                   /* celé slovo, potom bez predpony nv34gl */
            const char *w = word + (k ? 6 : 0);
            int n = 0;
            if (k && (word[0] != 'n' || word[1] != 'v' || word[2] != '3' || word[3] != '4' || word[4] != 'g' || word[5] != 'l'))
                break;
            while (w[n] && i + n < j && gCfg[i + n] == w[n])
                n++;
            if (!w[n] && i + n == j)
                return 1;
        }
        i = j;
    }
    return 0;
}

/* spec = súbor knižnice (z CFM init bloku); config hľadať v jeho priečinku */
void gl_cfg_load(const FSSpec *lib)
{
    FSSpec spec;
    short ref;
    long n = CFG_MAX;
    int i, o;
    char line[120];

    gCfgLen = 0;
    /* aj s príponou .txt (USB/FAT: File Exchange z nej urobí typ TEXT) */
    if (!lib || (FSMakeFSSpec(lib->vRefNum, lib->parID, "\pNV34 MiniGL config", &spec) != noErr &&
                 FSMakeFSSpec(lib->vRefNum, lib->parID, "\pNV34 MiniGL config.txt", &spec) != noErr)) {
        lg("config: subor \"NV34 MiniGL config\" vedla kniznice nie je, plati len boot-args");
        return;
    }
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) {
        lg("config: subor \"NV34 MiniGL config\" vedla kniznice nie je, plati len boot-args");
        return;
    }
    if (FSRead(ref, &n, gCfg) != noErr && n <= 0)
        n = 0;
    FSClose(ref);
    gCfgLen = (int)n;
    gCfg[gCfgLen] = 0;
    /* do logu len aktívne slová (bez poznámok) */
    for (i = 0, o = 0; i < gCfgLen && o < (int)sizeof(line) - 2; i++) {
        char c = gCfg[i];
        if (c == '#') {
            while (i < gCfgLen && gCfg[i] != '\r' && gCfg[i] != '\n')
                i++;
            c = ' ';
        }
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
        if (c != ' ' || (o && line[o - 1] != ' '))
            line[o++] = c;
    }
    line[o] = 0;
    lg("config: %s", o ? line : "(prazdny)");
}

static int boot_arg_reg(const char *word)
{
    RegEntryID opt;
    char args[256];
    UInt32 size = sizeof(args) - 1;
    int found = 0, i, j;

    if (RegistryCStrEntryLookup(0, "Devices:device-tree:options", &opt) != 0)
        return 0;
    if (RegistryPropertyGet(&opt, "boot-args", args, &size) == 0) {
        args[size] = 0;
        for (i = 0; args[i] && !found; i++) {
            for (j = 0; word[j] && args[i + j] == word[j]; j++)
                ;
            found = !word[j];
        }
    }
    RegistryEntryIDDispose(&opt);
    return found;
}

static int boot_arg(const char *word)
{
    return cfg_has(word) || boot_arg_reg(word);
}

int gl_boot_arg(const char *word)
{
    return boot_arg(word);
}

void gl_stats_init(void)
{
    gStatsOn = boot_arg("nv34glstats");
    gDbg = boot_arg("nv34gldebug");
    gTexDbg = boot_arg("nv34gltex");
    if (boot_arg("nv34glguard2"))
        gGuard = 2.0f;
    if (boot_arg("nv34glguard4"))
        gGuard = 4.0f;
    gMT = !boot_arg("nv34glnomt");
    gTnl = !boot_arg("nv34glnotnl");
    gNoFpg = boot_arg("nv34glnofpg");
}

NV34Shared *shared(void)
{
    RegEntryID e;
    UInt32 addr = 0, size = 4;

    if (S)
        return S;
    if (RegistryCStrEntryLookup(0, "Devices:device-tree:pci:NVDA,Parent:NVDA,Display-A", &e) != 0)
        return 0;
    if (RegistryPropertyGet(&e, "NV34ACCEL,shared", &addr, &size) == 0 && size == 4 && addr) {
        NV34Shared *s = (NV34Shared *)addr;
        if (s->magic == NV34_SHARED_MAGIC && s->version == NV34_SHARED_VERSION)
            S = s;
    }
    RegistryEntryIDDispose(&e);
    return S;
}

NVU32 vram_total(void)
{
    return shared() ? S->a->vramSize : 32u << 20;
}

/* Systémová zložka:Preferences:NV34 MiniGL log.txt — každý riadok hneď na disk (prežije pád) */
static int gLogLines, gLogFresh = 1;

static void file_put(const char *s, long n)
{
    short vref, ref;
    long dir;
    FSSpec spec;
    OSErr e;

    if (gLogLines >= (gDbg ? 30000 : 12000))
        return;
    gLogLines++;
    if (FindFolder(-32768, kPreferencesFolderType, true, &vref, &dir) != noErr)
        return;
    e = FSMakeFSSpec(vref, dir, "\pNV34 MiniGL log.txt", &spec);
    if (e == fnfErr)
        FSpCreate(&spec, 'ttxt', 'TEXT', 0);
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr)
        return;
    if (gLogFresh) {
        /* hl-7: nemazať pri každom načítaní (Half-Life knižnicu načíta viackrát) — len keď je veľký */
        long eof = 0;
        if (GetEOF(ref, &eof) == noErr && eof > 256 * 1024)
            SetEOF(ref, 0);
        gLogFresh = 0;
    }
    SetFPos(ref, fsFromLEOF, 0);
    FSWrite(ref, &n, s);
    FSClose(ref);
    FlushVol(0, vref);
}

static void fmt_num(char **o, NVU32 v, int base, int width, int neg)
{
    char pad = base == 16 || width < 0 ? '0' : ' ';
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    if (width < 0)
        width = -width;
    if (neg)
        *(*o)++ = '-';
    while (width-- > n)
        *(*o)++ = pad;
    while (n)
        *(*o)++ = tmp[--n];
}

/* %s %d %u %x %c, šírka, %f = 3 desatinné miesta */
void lg(const char *fmt, ...)
{
    char buf[800], *o = buf, *end = buf + sizeof(buf) - 24;
    va_list ap;

    va_start(ap, fmt);
    while (*fmt && o < end) {
        int width = 0;
        if (*fmt != '%') {
            *o++ = *fmt++;
            continue;
        }
        fmt++;
        {
            int zero = *fmt == '0';
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
            if (zero && width)
                width = -width;                     /* %02u: nuly */
        }
        switch (*fmt++) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && o < end)
                *o++ = *s++;
            break;
        }
        case 'd': {
            long v = va_arg(ap, long);
            fmt_num(&o, (NVU32)(v < 0 ? -v : v), 10, width, v < 0);
            break;
        }
        case 'u':
            fmt_num(&o, va_arg(ap, unsigned long), 10, width, 0);
            break;
        case 'x':
            fmt_num(&o, va_arg(ap, unsigned long), 16, width, 0);
            break;
        case 'c':
            *o++ = (char)va_arg(ap, int);
            break;
        case 'f': {
            double d = va_arg(ap, double);
            long m = (long)(d * 1000.0 + (d < 0 ? -0.5 : 0.5));
            NVU32 a = (NVU32)(m < 0 ? -m : m);
            fmt_num(&o, a / 1000, 10, 0, m < 0);
            *o++ = '.';
            fmt_num(&o, a % 1000, 10, -3, 0);
            break;
        }
        default:
            *o++ = '?';
        }
    }
    va_end(ap);
    *o++ = '\r';
    file_put(buf, o - buf);
}

#define SEEN_MAX 512
static NVU32 gSeen[SEEN_MAX];
static int gSeenCount;

int first_time(NVU32 key)
{
    int i;
    for (i = 0; i < gSeenCount; i++)
        if (gSeen[i] == key)
            return 0;
    if (gSeenCount < SEEN_MAX)
        gSeen[gSeenCount++] = key;
    return 1;
}

/* volané z generovaných stubov (stubs.c) */
void stub_hit(const char *name, NVU32 idx)
{
    if (first_time(0x51000000 | idx))
        lg("nepodporovane: %s", name);
}

#define ONCE(key, ...) do { if (first_time(key)) lg(__VA_ARGS__); } while (0)

/* max-9: podrobný výpis celej snímky (cutscény s letterboxom) — gDump = počet snímok */
static int gDump, gDumpN, gDumpLast, gFrVpOdd;
#define DLOG(...) do { if (gDump) lg(__VA_ARGS__); } while (0)
/* q3-8: celkový čas vo funkciách MiniGL (vnorené volania sa nerátajú dvakrát) — atribút cleanup
 * zavolá gl_leave pri každom výstupe z funkcie */
static int gGlDepth;
static NVU32 gGlT0, gTGL;
static inline NVU32 gl_enter(void) { if (gGlDepth++ == 0) gGlT0 = tb(); return 0; }
static inline void gl_leave(NVU32 *u) { (void)u; if (--gGlDepth == 0) gTGL += tb() - gGlT0; }
#define GLT NVU32 __attribute__((cleanup(gl_leave))) _glt = gl_enter()
#define CHECK() GLT; if (!C) return
#define FLUSH() gl_flush_batch()

/* max-13: prekrytia cez celú obrazovku (blend, hĺbka ALWAYS) a chyby GL za snímku */
static NVU32 gFrOvlN, gFrOvlC, gFrOvlMax, gFrErr, gFrErrN;

static void set_err(GLenum e)
{
    ONCE(0x5C500000 | (e & 0xffff), "chyba GL %x (prvykrat)", e);
    gFrErr = e;
    gFrErrN++;
    if (C && C->err == GL_NO_ERROR)
        C->err = e;
}

/* ---- matice (stĺpcové poradie ako GL) ------------------------------------------- */

static void mat_ident(float *m)
{
    int i;
    for (i = 0; i < 16; i++)
        m[i] = (i % 5) == 0 ? 1.0f : 0.0f;
}

static void mat_mul(float *out, const float *a, const float *b)
{
    float r[16];
    int c, k, i;
    for (c = 0; c < 4; c++)
        for (i = 0; i < 4; i++) {
            float s = 0;
            for (k = 0; k < 4; k++)
                s += a[k * 4 + i] * b[c * 4 + k];
            r[c * 4 + i] = s;
        }
    memcpy(out, r, sizeof(r));
}

static int mode_idx(GLenum m)
{
    return m == GL_PROJECTION ? 1 : m == GL_TEXTURE ? 2 : 0;
}

static float *top(void)
{
    int i = mode_idx(C->matrixMode);
    return C->mstack[i][C->msp[i]];
}

static NVU32 gXformGen = 1;                   /* opt-8: zmena matíc / viewportu → neplatná cache vrcholov */

static void mat_changed(void)
{
    if (C->matrixMode == GL_TEXTURE) {
        const float *m = top();
        int i;
        C->texIdentity = 1;
        for (i = 0; i < 16; i++)
            if (m[i] != ((i % 5) == 0 ? 1.0f : 0.0f))
                C->texIdentity = 0;
    } else {
        C->mvpDirty = 1;
    }
    gXformGen++;
}

static void mult(const float *m)
{
    float *t = top();
    mat_mul(t, t, m);
    mat_changed();
}

void glMatrixMode(GLenum mode)
{
    CHECK();
    DLOG("  glMatrixMode %x", mode);
    C->matrixMode = mode;
}

void glLoadIdentity(void)
{
    CHECK();
    DLOG("  glLoadIdentity");
    mat_ident(top());
    mat_changed();
}

void glLoadMatrixf(const GLfloat *m)
{
    CHECK();
    DLOG("  glLoadMatrixf %f %f %f %f / %f %f %f %f", (double)m[0], (double)m[5], (double)m[10], (double)m[11],
         (double)m[12], (double)m[13], (double)m[14], (double)m[15]);
    memcpy(top(), m, 64);
    mat_changed();
}

void glLoadMatrixd(const GLdouble *m)
{
    float *t;
    int i;
    CHECK();
    t = top();
    for (i = 0; i < 16; i++)
        t[i] = (float)m[i];
    mat_changed();
}

void glMultMatrixf(const GLfloat *m)
{
    CHECK();
    mult(m);
}

void glMultMatrixd(const GLdouble *m)
{
    float f[16];
    int i;
    CHECK();
    for (i = 0; i < 16; i++)
        f[i] = (float)m[i];
    mult(f);
}

void glPushMatrix(void)
{
    int i;
    CHECK();
    i = mode_idx(C->matrixMode);
    if (C->msp[i] + 1 >= STACK_DEPTH) {
        set_err(GL_STACK_OVERFLOW);
        return;
    }
    memcpy(C->mstack[i][C->msp[i] + 1], C->mstack[i][C->msp[i]], 64);
    C->msp[i]++;
}

void glPopMatrix(void)
{
    int i;
    CHECK();
    i = mode_idx(C->matrixMode);
    if (C->msp[i] == 0) {
        set_err(GL_STACK_UNDERFLOW);
        return;
    }
    C->msp[i]--;
    mat_changed();
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    float m[16];
    CHECK();
    mat_ident(m);
    m[12] = x; m[13] = y; m[14] = z;
    mult(m);
}

void glTranslated(GLdouble x, GLdouble y, GLdouble z) { glTranslatef((float)x, (float)y, (float)z); }

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    float m[16];
    CHECK();
    mat_ident(m);
    m[0] = x; m[5] = y; m[10] = z;
    mult(m);
}

void glScaled(GLdouble x, GLdouble y, GLdouble z) { glScalef((float)x, (float)y, (float)z); }

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    float m[16], s, c, t, len;
    double a;
    CHECK();
    len = (float)sqrt_((double)x * x + (double)y * y + (double)z * z);
    if (len == 0)
        return;
    x /= len; y /= len; z /= len;
    a = angle * (3.14159265358979323846 / 180.0);
    s = (float)sin_(a);
    c = (float)cos_(a);
    t = 1 - c;
    mat_ident(m);
    m[0] = x * x * t + c;     m[4] = x * y * t - z * s; m[8] = x * z * t + y * s;
    m[1] = y * x * t + z * s; m[5] = y * y * t + c;     m[9] = y * z * t - x * s;
    m[2] = x * z * t - y * s; m[6] = y * z * t + x * s; m[10] = z * z * t + c;
    mult(m);
}

void glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) { glRotatef((float)a, (float)x, (float)y, (float)z); }

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16];
    CHECK();
    DLOG("  glFrustum %f %f %f %f %f %f", l, r, b, t, n, f);
    memset(m, 0, sizeof(m));
    m[0] = (float)(2 * n / (r - l));
    m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l));
    m[9] = (float)((t + b) / (t - b));
    m[10] = (float)(-(f + n) / (f - n));
    m[11] = -1;
    m[14] = (float)(-2 * f * n / (f - n));
    mult(m);
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16];
    CHECK();
    DLOG("  glOrtho %f %f %f %f %f %f", l, r, b, t, n, f);
    mat_ident(m);
    m[0] = (float)(2 / (r - l));
    m[5] = (float)(2 / (t - b));
    m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l));
    m[13] = (float)(-(t + b) / (t - b));
    m[14] = (float)(-(f + n) / (f - n));
    mult(m);
}

/* ---- textúry ------------------------------------------------------------------ */

#define TEX_HASH 256
static TexObj *gTex[TEX_HASH];
static GLuint gNextName = 1;

static TexObj *tex_find(GLuint name)
{
    TexObj *t;
    for (t = gTex[name & (TEX_HASH - 1)]; t; t = t->next)
        if (t->name == name)
            return t;
    return 0;
}

static void tex_init(TexObj *t, GLuint name)
{
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->minFilter = GL_NEAREST_MIPMAP_LINEAR;
    t->magFilter = GL_LINEAR;
    t->wrapS = t->wrapT = GL_REPEAT;
}

static TexObj *tex_get(GLuint name)
{
    TexObj *t;
    if (name == 0)
        return &C->def;
    t = tex_find(name);
    if (t)
        return t;
    t = (TexObj *)NewPtrClear(sizeof(TexObj));
    if (!t)
        t = (TexObj *)NewPtrSysClear(sizeof(TexObj));
    if (!t) {
        ONCE(0x56200000, "tex %u: malo pamate na textúrový objekt -> textura 0", name);
        return &C->def;
    }
    tex_init(t, name);
    t->next = gTex[name & (TEX_HASH - 1)];
    gTex[name & (TEX_HASH - 1)] = t;
    if (name >= gNextName)
        gNextName = name + 1;
    return t;
}

/* textúra naviazaná na aktívnu jednotku (nahrávanie, parametre) */
static TexObj *cur_tex(void)
{
    return C->unit && C->bound1 ? C->bound1 : C->bound;
}

/* q3-7: kópie textúr v RAM — halda aplikácie, potom dočasná pamäť systému (Quake III si
 * zaberie takmer celú svoju haldu), až nakoniec systémová halda. Pred dátami 16 B hlavička. */
extern pascal Handle TempNewHandle(Size logicalSize, OSErr *resultCode);

enum { MEM_PTR = 1, MEM_TEMP = 2 };

/* wolf-1: veľké bloky NIKDY do systémovej haldy — jej nafúknutie Mac OS 9 zamrazí (Return to
 * Castle Wolfenstein pri načítaní mapy). Poradie: dočasná pamäť s rezervou 16 MB pre systém,
 * potom halda aplikácie s rezervou 2 MB pre hru; inak textúra nebude (log), Mac nezamrzne. */
static long gMemTemp, gMemApp, gMemFails;

/* každých 16 MB textúr v RAM riadok do logu (pred prípadným zamrznutím vidno spotrebu) */
static long gMemMark = 16L * 1024 * 1024;

static void mem_note(void)
{
    if (gMemTemp + gMemApp >= gMemMark) {
        gMemMark += 16L * 1024 * 1024;
        lg("textury v RAM %d KB (docasna %d, aplikacia %d), volna docasna %d KB, aplikacia %d KB",
           (gMemTemp + gMemApp) >> 10, gMemTemp >> 10, gMemApp >> 10, TempFreeMem() >> 10, FreeMem() >> 10);
    }
}

static void *mem_alloc(long size)
{
    long *p;
    if (TempFreeMem() > size + 16L * 1024 * 1024) {
        OSErr e;
        Handle h = TempNewHandle(size + 16, &e);
        if (h && !e) {
            HLock(h);
            p = (long *)*h;
            p[0] = MEM_TEMP;
            p[1] = (long)h;
            p[2] = size;
            gMemTemp += size;
            mem_note();
            return p + 4;
        }
    }
    if (FreeMem() > size + 2L * 1024 * 1024) {
        p = (long *)NewPtr(size + 16);
        if (p) {
            p[0] = MEM_PTR;
            p[2] = size;
            gMemApp += size;
            mem_note();
            return p + 4;
        }
    }
    if (gMemFails++ < 5)
        lg("malo pamate na textúru %d KB: docasna volna %d KB, aplikacia %d KB; textury v RAM: docasna %d KB, aplikacia %d KB",
           size >> 10, TempFreeMem() >> 10, FreeMem() >> 10, gMemTemp >> 10, gMemApp >> 10);
    return 0;
}

void gl_mem_report(void)
{
    lg("textury v RAM: docasna pamat %d KB, halda aplikacie %d KB, zlyhani %d (volna docasna %d KB)",
       gMemTemp >> 10, gMemApp >> 10, gMemFails, TempFreeMem() >> 10);
}

static void mem_free(void *q)
{
    long *p = (long *)q - 4;
    if (!q)
        return;
    if (p[0] == MEM_TEMP)
        gMemTemp -= p[2];
    else
        gMemApp -= p[2];
    if (p[0] == MEM_TEMP)
        DisposeHandle((Handle)p[1]);
    else
        DisposePtr((Ptr)p);
}

static void tex_free_levels(TexObj *t)
{
    int i;
    for (i = 0; i < MAX_LEVELS; i++) {
        if (t->lv[i])
            mem_free(t->lv[i]);
        t->lv[i] = 0;
        t->w[i] = t->h[i] = 0;
    }
    if (t->hw)
        hw_tex_free(t->hw);
    t->hw = 0;
}

void glGenTextures(GLsizei n, GLuint *names)
{
    int i;
    for (i = 0; i < n; i++) {
        while (tex_find(gNextName) || gNextName == 0)
            gNextName++;
        names[i] = gNextName++;
    }
}

void glDeleteTextures(GLsizei n, const GLuint *names)
{
    int i;
    CHECK();
    if (gTexDbg && gTexDbgN < 3000 && n > 0) {
        static int nDel;
        if (++nDel <= 40) {
            gTexDbgN++;
            lg("glDeleteTextures %d (prva %u, naviazana %u)", n, names[0], C->bound ? C->bound->name : 0);
        }
    }
    FLUSH();
    for (i = 0; i < n; i++) {
        TexObj **pp, *t;
        if (!names[i])
            continue;
        for (pp = &gTex[names[i] & (TEX_HASH - 1)]; (t = *pp) != 0; pp = &t->next)
            if (t->name == names[i]) {
                *pp = t->next;
                if (C->bound == t)
                    C->bound = &C->def;
                if (C->bound1 == t)
                    C->bound1 = &C->def;
                tex_free_levels(t);
                DisposePtr((Ptr)t);
                break;
            }
    }
}

GLboolean glIsTexture(GLuint name)
{
    return name && tex_find(name) ? GL_TRUE : GL_FALSE;
}

GLboolean glAreTexturesResident(GLsizei n, const GLuint *names, GLboolean *res)
{
    int i;
    for (i = 0; i < n; i++)
        res[i] = GL_TRUE;
    return GL_TRUE;
}

void glPrioritizeTextures(GLsizei n, const GLuint *names, const GLclampf *pr)
{
    (void)n; (void)names; (void)pr;
}

void glBindTexture(GLenum target, GLuint name)
{
    CHECK();
    if (gTexDbg && gTexDbgN < 3000 && first_time(0x5D000000 ^ name ^ ((NVU32)C->unit << 20))) {
        gTexDbgN++;
        lg("glBindTexture %u jednotka %d", name, C->unit);
    }
    if (target != GL_TEXTURE_2D) {
        ONCE(0x52000000 | target, "glBindTexture: ciel %x", target);
        return;
    }
    if (C->unit) {
        C->bound1 = tex_get(name);
        return;
    }
    FLUSH();
    C->bound = tex_get(name);
}

/* vnútorný formát → základný */
static GLenum base_of(GLint f)
{
    switch (f) {
    case 1: case GL_LUMINANCE: case 0x803F: case 0x8040: case 0x8041: case 0x8042:
        return GL_LUMINANCE;
    case 2: case GL_LUMINANCE_ALPHA: case 0x8043: case 0x8044: case 0x8045: case 0x8046: case 0x8047: case 0x8048:
        return GL_LUMINANCE_ALPHA;
    case GL_ALPHA: case 0x803B: case 0x803C: case 0x803D: case 0x803E:
        return GL_ALPHA;
    case GL_INTENSITY: case 0x804A: case 0x804B: case 0x804C: case 0x804D:
        return GL_INTENSITY;
    case 3: case GL_RGB: case 0x2A10: case 0x804F: case 0x8050: case 0x8051: case 0x8052: case 0x8053: case 0x8054:
        return GL_RGB;
    default:
        return GL_RGBA;
    }
}

static int comps_of(GLenum format)
{
    switch (format) {
    case GL_RGBA: case GL_BGRA_EXT: return 4;
    case GL_RGB: case 0x80E0: return 3;            /* GL_BGR_EXT */
    case GL_LUMINANCE_ALPHA: return 2;
    default: return 1;
    }
}

/* riadok zdroja → ARGB32 podľa formátu a základného vnútorného formátu */
static void convert_row8(NVU32 *dst, const GLubyte *src, int w, GLenum format, GLenum base)
{
    int x;
    for (x = 0; x < w; x++) {
        NVU32 r, g, b, a = 255;
        switch (format) {
        case GL_RGBA:     r = src[0]; g = src[1]; b = src[2]; a = src[3]; src += 4; break;
        case GL_RGB:      r = src[0]; g = src[1]; b = src[2]; src += 3; break;
        case GL_BGRA_EXT: b = src[0]; g = src[1]; r = src[2]; a = src[3]; src += 4; break;
        case 0x80E0:      b = src[0]; g = src[1]; r = src[2]; src += 3; break;
        case GL_LUMINANCE: r = g = b = src[0]; src += 1; break;
        case GL_LUMINANCE_ALPHA: r = g = b = src[0]; a = src[1]; src += 2; break;
        case GL_ALPHA:    r = g = b = 0; a = src[0]; src += 1; break;
        case 0x1903:      r = src[0]; g = b = 0; src += 1; break;     /* GL_RED */
        case 0x1904:      g = src[0]; r = b = 0; src += 1; break;
        case 0x1905:      b = src[0]; r = g = 0; src += 1; break;
        default:          r = g = b = src[0]; src += 1; break;       /* COLOR_INDEX: odtieň */
        }
        switch (base) {
        case GL_RGB:             a = 255; break;
        case GL_ALPHA:           r = g = b = 255; break;
        case GL_LUMINANCE:       g = b = r; a = 255; break;
        case GL_LUMINANCE_ALPHA: g = b = r; break;
        case GL_INTENSITY:       g = b = a = r; break;
        }
        dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

/* max-1: Max Payne posiela textúry s typom GL_UNSIGNED_SHORT (0x1403) → biele. Ďalšie typy sa
 * rozbalia na bajty v poradí zložiek formátu a prevedú ako GL_UNSIGNED_BYTE. */
static int packed_bytes(GLenum type)
{
    switch (type) {
    case 0x8033: case 0x8034: case 0x8363: case 0x8364: case 0x8365: case 0x8366: return 2;
    case 0x8035: case 0x8036: case 0x8367: case 0x8368: return 4;
    }
    return 0;
}

static int type_bytes(GLenum type)
{
    switch (type) {
    case 0x1402: case 0x1403: return 2;                 /* (UNSIGNED_)SHORT */
    case 0x1404: case 0x1405: case 0x1406: return 4;    /* (UNSIGNED_)INT, FLOAT */
    }
    return 1;
}

static int type_ok(GLenum type)
{
    return type == GL_UNSIGNED_BYTE || type == 0x1400 || packed_bytes(type) || type_bytes(type) > 1;
}

/* bajty na pixel zdroja */
static int pix_bytes(GLenum format, GLenum type)
{
    int p = packed_bytes(type);
    return p ? p : comps_of(format) * type_bytes(type);
}

static GLubyte gUnpack[2048 * 4];

/* zbalený pixel v → zložky c[0..n-1] v poradí formátu; šírky wd[] v poradí zložiek, c0 v najvyšších
 * bitoch (rev = v najnižších, typy *_REV) */
static void unpack_bits(NVU32 v, int n, const int *wd, int rev, GLubyte *c)
{
    int i, sh = 0;
    if (!rev)
        for (i = 0; i < n; i++)
            sh += wd[i];
    for (i = 0; i < n; i++) {
        NVU32 m = (1u << wd[i]) - 1;
        if (!rev)
            sh -= wd[i];
        c[i] = (GLubyte)(((v >> sh) & m) * 255 / m);
        if (rev)
            sh += wd[i];
    }
}

static void convert_row(NVU32 *dst, const GLubyte *src, int w, GLenum format, GLenum type, GLenum base)
{
    static const int w4444[4] = { 4, 4, 4, 4 }, w5551[4] = { 5, 5, 5, 1 }, w565[3] = { 5, 6, 5 },
                     w8888[4] = { 8, 8, 8, 8 }, w1010102[4] = { 10, 10, 10, 2 };
    int x, i, n = comps_of(format);
    GLubyte *o = gUnpack;
    if (type == GL_UNSIGNED_BYTE || type == 0x1400 || w > 2048) {
        convert_row8(dst, src, w, format, base);
        return;
    }
    for (x = 0; x < w; x++, o += n) {
        switch (type) {
        case 0x1402: case 0x1403:                       /* 16-bit zložky (BE): horný bajt */
            for (i = 0; i < n; i++)
                o[i] = src[i * 2];
            src += n * 2;
            break;
        case 0x1404: case 0x1405:
            for (i = 0; i < n; i++)
                o[i] = src[i * 4];
            src += n * 4;
            break;
        case 0x1406:
            for (i = 0; i < n; i++) {
                float f = ((const float *)src)[i];
                o[i] = f <= 0 ? 0 : f >= 1 ? 255 : (GLubyte)(f * 255.0f + 0.5f);
            }
            src += n * 4;
            break;
        default: {
            NVU32 v = packed_bytes(type) == 2 ? ((NVU32)src[0] << 8 | src[1]) :
                      ((NVU32)src[0] << 24 | (NVU32)src[1] << 16 | (NVU32)src[2] << 8 | src[3]);
            src += packed_bytes(type);
            switch (type) {
            case 0x8033: unpack_bits(v, 4, w4444, 0, o); break;     /* 4_4_4_4 */
            case 0x8365: unpack_bits(v, 4, w4444, 1, o); break;     /* 4_4_4_4_REV */
            case 0x8034: unpack_bits(v, 4, w5551, 0, o); break;     /* 5_5_5_1 */
            case 0x8366: unpack_bits(v, 4, w5551, 1, o); break;     /* 1_5_5_5_REV */
            case 0x8363: unpack_bits(v, 3, w565, 0, o); break;      /* 5_6_5 */
            case 0x8364: unpack_bits(v, 3, w565, 1, o); break;      /* 5_6_5_REV */
            case 0x8035: unpack_bits(v, 4, w8888, 0, o); break;     /* 8_8_8_8 */
            case 0x8367: unpack_bits(v, 4, w8888, 1, o); break;     /* 8_8_8_8_REV */
            case 0x8036: unpack_bits(v, 4, w1010102, 0, o); break;
            case 0x8368: unpack_bits(v, 4, w1010102, 1, o); break;
            }
            break;
        }
        }
    }
    convert_row8(dst, gUnpack, w, format, base);
}

static int mip_filter(GLenum f);

static long src_stride(int w, GLenum format, GLenum type)
{
    long rl = C->unpackRowLength > 0 ? C->unpackRowLength : w;
    long bytes = rl * pix_bytes(format, type);
    long al = C->unpackAlign > 0 ? C->unpackAlign : 1;
    return (bytes + al - 1) / al * al;
}

static int is_pow2(int v) { return v > 0 && !(v & (v - 1)); }

/* wolf-2: pri 16-bit obrazovke kópia textúry v RAM rovno 16-bit (polovica pamäte, bez konverzie
 * pri nahrávaní do VRAM). Formát úrovne 0: bez alfy A1R5G5B5, s alfou 0/255 tiež, inak A4R4G4B4;
 * ostatné úrovne podľa úrovne 0. */
static NVU32 gRow[2048];

static int store_format(TexObj *t, int level, GLenum base, const GLvoid *pixels, int w, int h, GLenum format,
                        GLenum type)
{
    int y, x;
    if (!C || !C->hasRT || C->rt.bpp != 16)
        return 0x06;
    if (level > 0 && t->lv[0])
        return t->sfmt[0];
    if (base == GL_RGB || base == GL_LUMINANCE || !pixels)
        return 0x02;
    {
        long stride = src_stride(w, format, type);
        for (y = 0; y < h; y++) {
            convert_row(gRow, (const GLubyte *)pixels + y * stride, w, format, type, base);
            for (x = 0; x < w; x++) {
                NVU32 a = gRow[x] >> 24;
                if (a != 0 && a != 255)
                    return 0x04;
            }
        }
    }
    return 0x02;
}

/* riadok zdroja do kópie úrovne `level` na (x0, y) */
static void store_row(TexObj *t, int level, int x0, int y, int w, const GLubyte *src, GLenum format, GLenum type,
                      GLenum base)
{
    int sf = t->sfmt[level], x;
    long off = (long)y * t->w[level] + x0;
    if (sf == 0x06) {
        convert_row((NVU32 *)t->lv[level] + off, src, w, format, type, base);
        return;
    }
    convert_row(gRow, src, w, format, type, base);
    {
        NVU16 *d = (NVU16 *)t->lv[level] + off;
        for (x = 0; x < w; x++)
            d[x] = tx16(gRow[x], sf);
    }
}

static GLint gProxyW, gProxyH, gProxyFmt;

/* max-14: čas glTexImage2D/glTexSubImage2D (prevod do RAM kópie) za snímku */
static NVU32 gTConv, gFrTexImgN, gFrTexImgB;

static void tex_image(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
                      GLenum format, GLenum type, const GLvoid *pixels);
static void tex_sub_image(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h,
                          GLenum format, GLenum type, const GLvoid *pixels);

void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels)
{
    NVU32 t0 = tb();
    tex_image(target, level, internal, w, h, border, format, type, pixels);
    gTConv += tb() - t0;
    gFrTexImgN++;
    gFrTexImgB += (NVU32)w * h * 4;
}

void glTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h,
                     GLenum format, GLenum type, const GLvoid *pixels)
{
    NVU32 t0 = tb();
    tex_sub_image(target, level, xo, yo, w, h, format, type, pixels);
    gTConv += tb() - t0;
}

static void tex_image(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
                      GLenum format, GLenum type, const GLvoid *pixels)
{
    TexObj *t;
    GLenum base;
    int y, sfmt, bpp;

    CHECK();
    if (target == 0x8064) {                         /* GL_PROXY_TEXTURE_2D: len test veľkosti */
        int ok = level >= 0 && level < MAX_LEVELS && w > 0 && h > 0 && w <= 2048 && h <= 2048;
        gProxyW = ok ? w : 0;
        gProxyH = ok ? h : 0;
        gProxyFmt = ok ? internal : 0;
        return;
    }
    if (target != GL_TEXTURE_2D || level < 0 || level >= MAX_LEVELS || w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        ONCE(0x53000000 | (target & 0xffff), "glTexImage2D: ciel %x level %d %dx%d nepodporovane", target, level, w, h);
        set_err(GL_INVALID_VALUE);
        return;
    }
    if (type != GL_UNSIGNED_BYTE)
        ONCE(0x53100000 ^ (type & 0xffff) ^ ((format & 0xfff) << 16), "glTexImage2D: typ %x, format %x, vnutorny %x, %dx%d%s",
             type, format, internal, w, h, type_ok(type) ? " (prevod)" : " (nepodporovane, biela)");
    if (border)
        ONCE(0x53200000, "glTexImage2D: okraj %d ignorovany", border);
    if (!is_pow2(w) || !is_pow2(h))
        ONCE(0x53300000, "glTexImage2D: rozmer %dx%d nie je mocnina 2", w, h);
    FLUSH();
    t = cur_tex();
    base = base_of(internal);
    if (level == 0)
        t->hasAlpha = base != GL_RGB && base != GL_LUMINANCE;
    sfmt = store_format(t, level, base, pixels && type_ok(type) ? pixels : 0, w, h, format, type);
    bpp = sfmt == 0x06 ? 4 : 2;
    if (t->lv[level] && (t->w[level] != w || t->h[level] != h || t->sfmt[level] != sfmt)) {
        mem_free(t->lv[level]);
        t->lv[level] = 0;
    }
    if (!t->lv[level]) {
        t->lv[level] = mem_alloc((long)w * h * bpp);
        if (!t->lv[level]) {
            lg("glTexImage2D: malo pamate (%dx%d)", w, h);
            set_err(GL_OUT_OF_MEMORY);
            return;
        }
    }
    t->w[level] = w;
    t->h[level] = h;
    t->sfmt[level] = sfmt;
    static int nTI;
    if (gTexDbg && gTexDbgN < 3000 && level == 0 && ++nTI <= 60) {
        gTexDbgN++;
        lg("TexImage2D tex %u lvl %d %dx%d int %x fmt %x typ %x pix %x -> RAM fmt %x", t->name, level, w, h, internal,
           format, type, (NVU32)pixels, sfmt);
    }
    if (!pixels || !type_ok(type)) {
        memset(t->lv[level], 0xff, (long)w * h * bpp);
    } else {
        long stride = src_stride(w, format, type);
        for (y = 0; y < h; y++)
            store_row(t, level, 0, y, w, (const GLubyte *)pixels + y * stride, format, type, base);
    }
    t->dirty = 1;
}

static void tex_sub_image(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h,
                          GLenum format, GLenum type, const GLvoid *pixels)
{
    TexObj *t;
    GLenum base;
    long stride;
    int y;

    CHECK();
    t = cur_tex();
    if (target != GL_TEXTURE_2D || level < 0 || level >= MAX_LEVELS || !t->lv[level] || !pixels ||
        xo < 0 || yo < 0 || xo + w > t->w[level] || yo + h > t->h[level]) {
        set_err(GL_INVALID_VALUE);
        return;
    }
    if (!type_ok(type)) {
        ONCE(0x53400000 | (type & 0xffff), "glTexSubImage2D: typ %x nepodporovany", type);
        return;
    }
    if (gTexDbg && gTexDbgN < 3000 && first_time(0x5F000000 ^ t->name)) {
        gTexDbgN++;
        lg("TexSubImage2D tex %u lvl %d (%d,%d) %dx%d fmt %x", t->name, level, xo, yo, w, h, format);
    }
    FLUSH();
    base = t->hasAlpha ? GL_RGBA : GL_RGB;
    stride = src_stride(w, format, type);
    if (w > 2048)
        return;
    for (y = 0; y < h; y++)
        store_row(t, level, xo, yo + y, w, (const GLubyte *)pixels + y * stride, format, type, base);
    if (level != 0 || t->dirty || !t->hw || !hw_tex_resident(t->hw) || mip_filter(t->minFilter) ||
        !hw_tex_subupload(t->hw, xo, yo, w, h,
                          t->sfmt[0] == 0x06 ? (const void *)((const NVU32 *)t->lv[0] + (long)yo * t->w[0] + xo)
                                             : (const void *)((const NVU16 *)t->lv[0] + (long)yo * t->w[0] + xo),
                          t->w[0], t->sfmt[0]))
        t->dirty = 1;
}

void glTexImage1D(GLenum target, GLint level, GLint internal, GLsizei w, GLint border, GLenum format,
                  GLenum type, const GLvoid *pixels)
{
    ONCE(0x53500000, "glTexImage1D nepodporovane");
    (void)target; (void)level; (void)internal; (void)w; (void)border; (void)format; (void)type; (void)pixels;
}

/* ---- doplnky pre Half-Life (Xash3D) ------------------------------------------- */

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *idx);

void glGetTexLevelParameteriv(GLenum target, GLenum level_, GLenum pname, GLint *v)
{
    GLint level = (GLint)level_, w = 0, h = 0, fmt = 0;
    TexObj *t;
    CHECK();
    if (target == 0x8064) {
        w = gProxyW; h = gProxyH; fmt = gProxyFmt;
    } else if (level >= 0 && level < MAX_LEVELS) {
        t = cur_tex();
        if (t->lv[level]) {
            w = t->w[level];
            h = t->h[level];
            fmt = t->hasAlpha ? GL_RGBA : GL_RGB;
        }
    }
    switch (pname) {
    case 0x1000: *v = w; break;                     /* GL_TEXTURE_WIDTH */
    case 0x1001: *v = h; break;                     /* GL_TEXTURE_HEIGHT */
    case 0x1003: *v = fmt; break;                   /* GL_TEXTURE_INTERNAL_FORMAT */
    case 0x1005: *v = 0; break;                     /* GL_TEXTURE_BORDER */
    case 0x805C: case 0x805D: case 0x805E: *v = w ? 8 : 0; break;   /* RED/GREEN/BLUE_SIZE */
    case 0x805F: *v = w && fmt == GL_RGBA ? 8 : 0; break;          /* ALPHA_SIZE */
    default:
        ONCE(0x54100000 | (pname & 0xffff), "glGetTexLevelParameter %x", pname);
        *v = 0;
    }
}

void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *v)
{
    GLint i = 0;
    glGetTexLevelParameteriv(target, (GLenum)level, pname, &i);
    *v = (GLfloat)i;
}

/* max-8: priebeh snímok pre cutscény — glClear/glFlush/glFinish/glDrawBuffer za snímku,
 * zápis do logu len pri zmene vzoru (frame_trace) */
static NVU32 gFrClears, gFrClearMask, gFrClearArgb, gFrFlush, gFrFinish, gFrRead, gFrDrawBuf = GL_BACK;
static NVU32 gFrTris0, gTrSig = 0xffffffff;
static int gTrN;

/* stencil buffer nemáme (AGL_STENCIL_SIZE 0) — bez účinku */
void glClearStencil(GLint s) { ONCE(0x5C000001, "glClearStencil %d (bez stencilu)", s); }
void glStencilMask(GLuint m) { ONCE(0x5C000002, "glStencilMask %x (bez stencilu)", m); }
void glStencilFunc(GLenum f, GLint r, GLuint m) { ONCE(0x5C000003, "glStencilFunc %x %d %x (bez stencilu)", f, r, m); }
void glStencilOp(GLenum a, GLenum b, GLenum c) { ONCE(0x5C000004, "glStencilOp %x %x %x (bez stencilu)", a, b, c); }

void glBlendEquation(GLenum m)
{
    if (m != 0x8006)                                /* GL_FUNC_ADD */
        ONCE(0x59300000 | (m & 0xffff), "glBlendEquation %x (len ADD)", m);
}

void glBlendEquationEXT(GLenum m) { glBlendEquation(m); }

void glTexGeni(GLenum c, GLenum p, GLint v)
{
    ONCE(0x59400000, "glTexGen ignorovane (%x %x %d)", c, p, v);
}

void glTexGenf(GLenum c, GLenum p, GLfloat v) { glTexGeni(c, p, (GLint)v); }
void glTexGenfv(GLenum c, GLenum p, const GLfloat *v) { glTexGeni(c, p, (GLint)v[0]); }

void glClipPlane(GLenum p, const GLdouble *eq)
{
    (void)eq;
    ONCE(0x59500000, "glClipPlane %x ignorovane", p);
}

void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *idx)
{
    (void)start; (void)end;
    glDrawElements(mode, count, type, idx);
}

static int mip_filter(GLenum f)
{
    return f != GL_NEAREST && f != GL_LINEAR;
}

static void tex_param(GLenum target, GLenum pname, GLint v)
{
    TexObj *t;
    CHECK();
    if (target != GL_TEXTURE_2D)
        return;
    FLUSH();
    t = cur_tex();
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        if (mip_filter(v) != mip_filter(t->minFilter))
            t->dirty = 1;                           /* iný počet úrovní vo VRAM */
        t->minFilter = v;
        break;
    case GL_TEXTURE_MAG_FILTER: t->magFilter = v; break;
    case GL_TEXTURE_WRAP_S:     t->wrapS = v; break;
    case GL_TEXTURE_WRAP_T:     t->wrapT = v; break;
    default:
        ONCE(0x54000000 | (pname & 0xffff), "glTexParameter %x = %d ignorovane", pname, v);
    }
}

void glTexParameteri(GLenum target, GLenum pname, GLint v) { tex_param(target, pname, v); }
void glTexParameterf(GLenum target, GLenum pname, GLfloat v) { tex_param(target, pname, (GLint)v); }
void glTexParameteriv(GLenum target, GLenum pname, const GLint *v) { tex_param(target, pname, v[0]); }
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *v) { tex_param(target, pname, (GLint)v[0]); }

void glGetTexParameteriv(GLenum target, GLenum pname, GLint *v)
{
    TexObj *t;
    CHECK();
    (void)target;
    t = cur_tex();
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: *v = t->minFilter; break;
    case GL_TEXTURE_MAG_FILTER: *v = t->magFilter; break;
    case GL_TEXTURE_WRAP_S:     *v = t->wrapS; break;
    case GL_TEXTURE_WRAP_T:     *v = t->wrapT; break;
    default: *v = 0;
    }
}

void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *v)
{
    GLint i = 0;
    glGetTexParameteriv(target, pname, &i);
    *v = (GLfloat)i;
}

#define GL_COMBINE_ARB      0x8570

/* max-4: glTexEnv jednotky C->unit do C->env[]: režim aj GL_COMBINE (COMBINE_RGB/ALPHA, SOURCE0–2,
 * OPERAND0–2, RGB/ALPHA_SCALE, TEXTURE_ENV_COLOR). Kreslí fragment program (hw.c, fpg_*). */
static void tex_env_v(GLenum target, GLenum pname, const GLfloat *f)
{
    HwEnvUnit n, *e;
    GLint v = (GLint)f[0];
    int k;
    CHECK();
    if (target != GL_TEXTURE_ENV || C->unit > 1) {
        ONCE(0x55000000 | (pname & 0xffff), "glTexEnv %x %x = %x ignorovane", target, pname, v);
        return;
    }
    n = C->env[C->unit];
    e = &n;
    if (C->unit && pname != GL_TEXTURE_ENV_MODE)
        ONCE(0x5C400000 | ((pname & 0xff) << 16) | (v & 0xffff), "glTexEnv jednotky 1: %x = %x", pname, v);
    switch (pname) {
    case GL_TEXTURE_ENV_MODE:
        if (v != GL_MODULATE && v != GL_REPLACE && v != GL_DECAL && v != GL_BLEND && v != 0x0104 &&
            v != GL_COMBINE_ARB) {
            ONCE(0x55100000 | (v & 0xffff), "glTexEnv mode %x (ako MODULATE)", v);
            v = GL_MODULATE;
        }
        e->mode = v;
        break;
    case 0x8571: e->comb[0] = v; break;                         /* COMBINE_RGB */
    case 0x8572: e->comb[1] = v; break;                         /* COMBINE_ALPHA */
    case 0x8573: e->scale[0] = f[0]; break;                     /* RGB_SCALE */
    case 0x0D1C: e->scale[1] = f[0]; break;                     /* ALPHA_SCALE */
    case 0x2201:                                                /* TEXTURE_ENV_COLOR */
        for (k = 0; k < 4; k++)
            e->color[k] = f[k];
        break;
    default:
        if (pname >= 0x8580 && pname <= 0x8582) e->src[0][pname - 0x8580] = v;
        else if (pname >= 0x8588 && pname <= 0x858A) e->src[1][pname - 0x8588] = v;
        else if (pname >= 0x8590 && pname <= 0x8592) e->op[0][pname - 0x8590] = v;
        else if (pname >= 0x8598 && pname <= 0x859A) e->op[1][pname - 0x8598] = v;
        else {
            ONCE(0x55000000 | (pname & 0xffff), "glTexEnv %x %x = %x ignorovane", target, pname, v);
            return;
        }
    }
    {
        const unsigned char *a = (const unsigned char *)&n, *b = (const unsigned char *)&C->env[C->unit];
        for (k = 0; k < (int)sizeof(n) && a[k] == b[k]; k++)
            ;
        if (k == (int)sizeof(n))
            return;                                 /* bez zmeny: dávka pokračuje */
    }
    FLUSH();
    C->env[C->unit] = n;
    if (!C->unit)
        C->s.texEnv = n.mode;
}

static void tex_env(GLenum target, GLenum pname, GLint v)
{
    GLfloat f[4] = { (GLfloat)v, 0, 0, 0 };
    if (pname == 0x2201)                            /* farba len cez fv/iv */
        return;
    tex_env_v(target, pname, f);
}

static void env_init(HwEnvUnit *e)
{
    int k;
    memset(e, 0, sizeof(*e));
    e->mode = e->comb[0] = e->comb[1] = GL_MODULATE;
    for (k = 0; k < 2; k++) {
        e->src[k][0] = GL_TEXTURE;
        e->src[k][1] = 0x8578;                      /* PREVIOUS */
        e->src[k][2] = 0x8576;                      /* CONSTANT */
        e->op[k][0] = e->op[k][1] = k ? GL_SRC_ALPHA : GL_SRC_COLOR;
        e->op[k][2] = GL_SRC_ALPHA;
        e->scale[k] = 1.0f;
    }
}

void glTexEnvi(GLenum target, GLenum pname, GLint v) { tex_env(target, pname, v); }
void glTexEnvf(GLenum target, GLenum pname, GLfloat v) { GLfloat f[4] = { v, 0, 0, 0 }; tex_env_v(target, pname, f); }
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *v) { if (v) tex_env_v(target, pname, v); }
void glTexEnviv(GLenum target, GLenum pname, const GLint *v)
{
    GLfloat f[4];
    int k;
    if (!v)
        return;
    for (k = 0; k < 4; k++)                         /* farba: celé čísla → 0…1 */
        f[k] = pname == 0x2201 ? (GLfloat)((double)v[k] / 2147483647.0) : (GLfloat)v[k];
    tex_env_v(target, pname, f);
}

void glGetTexEnviv(GLenum target, GLenum pname, GLint *v)
{
    (void)target; (void)pname;
    *v = C ? (GLint)C->s.texEnv : GL_MODULATE;
}

void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *v)
{
    (void)target; (void)pname;
    *v = C ? (GLfloat)C->s.texEnv : GL_MODULATE;
}

/* textúra pre kreslenie: nahrať do VRAM, ak sa zmenila alebo bola vyhodená */
static TexObj *active_tex(void)
{
    TexObj *t = C->bound;
    return C->s.tex2D && t && t->lv[0] ? t : 0;
}

/* q3-12: druhá jednotka — len s textúrou na jednotke 0 (lightmapy Quake III) */
static TexObj *active_tex1(void)
{
    TexObj *t = C->bound1;
    if (!gMT || !C->tex2D1 || !t || !t->lv[0] || !active_tex())
        return 0;
    return t;                                       /* max-4: iný režim než MODULATE kreslí fragment program */
}

/* hl-9: pri 16-bit obrazovke 16-bit textúry (polovica VRAM; Half-Life pri 1024×768 minul VRAM) */
static int tex_format(TexObj *t)
{
    const NVU32 *p;
    long i, n;
    int partial = 0;
    if (t->sfmt[0] == 0x02 || t->sfmt[0] == 0x04)
        return t->sfmt[0];                          /* kópia v RAM už 16-bit */
    if (!C || C->rt.bpp != 16)
        return 0x06;
    if (!t->hasAlpha)
        return 0x02;
    p = (const NVU32 *)t->lv[0];
    n = (long)t->w[0] * t->h[0];
    for (i = 0; i < n; i++) {
        NVU32 a = p[i] >> 24;
        if (a != 0 && a != 255) {
            partial = 1;
            break;
        }
    }
    return partial ? 0x04 : 0x02;
}

/* 1 = rezidentná, 0 = nekompletná (kresliť bez textúry), -1 = málo VRAM (nekresliť) */
static int tex_prepare(TexObj *t)
{
    void *pix[MAX_LEVELS];
    int n = 1, i, fmt;

    if (!t->hw) {
        t->hw = hw_tex_new();
        if (!t->hw)
            return 0;
        t->dirty = 1;
    }
    if (!t->dirty && hw_tex_resident(t->hw))
        return 1;
    pix[0] = t->lv[0];
    if (mip_filter(t->minFilter))
        for (i = 1; i < MAX_LEVELS && t->lv[i]; i++) {
            int ew = t->w[0] >> i, eh = t->h[0] >> i;
            if (ew < 1) ew = 1;
            if (eh < 1) eh = 1;
            if (t->w[i] != ew || t->h[i] != eh || t->sfmt[i] != t->sfmt[0])
                break;
            pix[n++] = t->lv[i];
            if (ew == 1 && eh == 1)
                break;
        }
    if (!is_pow2(t->w[0]) || !is_pow2(t->h[0])) {
        if (gTexDbg)
            ONCE(0x56100000 ^ t->name, "tex %u %dx%d nie je mocnina 2", t->name, t->w[0], t->h[0]);
        return 0;
    }
    if (gTexDbg && gTexDbgN < 3000) {
        gTexDbgN++;
        lg("prepare tex %u %dx%d urovne %d/%s min %x fmt %x (RAM %x)", t->name, t->w[0], t->h[0], n,
           mip_filter(t->minFilter) ? "mip" : "-", t->minFilter, tex_format(t), t->sfmt[0]);
    }
    fmt = tex_format(t);
    if (!hw_tex_upload(t->hw, t->w[0], t->h[0], n, pix, fmt, t->sfmt[0])) {
        ONCE(0x56000000, "textura %u %dx%d: nahratie do VRAM zlyhalo", t->name, t->w[0], t->h[0]);
        return -1;
    }
    t->dirty = 0;
    return 1;
}

/* ---- dávka ------------------------------------------------------------------- */

#define PB_MAX 1536
static HwVtx gPB[PB_MAX];
static int gPBn, gPBprim;
static TexObj *gPBtex, *gPBtex1;
static int gPBtnl;                              /* q3-16: dávka v súradniciach objektu (HW T&L) */
static float gTnlMat[16];                       /* zložená matica okno × MVP po riadkoch */
static float gTnlMV[16];                        /* hmla: MODELVIEW (po riadkoch) pre vzdialenosť v oku */
static NVU32 gTnlGen, gTnlXf;
static int gTnlRtH;

static void fill_state(HwState *st, TexObj *tex)
{
    GLS *s = &C->s;
    int vx = s->viewport[0], vy = s->viewport[1], vw = s->viewport[2], vh = s->viewport[3];

    st->blend = s->blend && !(s->blendSrc == GL_ONE && s->blendDst == GL_ZERO);
    st->blendSrc = s->blendSrc;
    st->blendDst = s->blendDst;
    st->alphaTest = s->alphaTest && s->alphaFunc != GL_ALWAYS;
    st->alphaFunc = s->alphaFunc;
    st->alphaRef = s->alphaRef;
    st->depthTest = s->depthTest && C->depthBits > 0;
    st->depthFunc = s->depthFunc;
    st->depthWrite = s->depthMask;
    st->colorMask = (s->cmask[3] ? 0x01000000 : 0) | (s->cmask[0] ? 0x00010000 : 0) |
                    (s->cmask[1] ? 0x00000100 : 0) | (s->cmask[2] ? 0x00000001 : 0);
    /* orezanie na viewport (GL orezáva primitívy objemom pohľadu) ∩ scissor */
    if (s->scissorTest) {
        int x0 = s->scissor[0], y0 = s->scissor[1], x1 = x0 + s->scissor[2], y1 = y0 + s->scissor[3];
        if (vx > x0) x0 = vx;
        if (vy > y0) y0 = vy;
        if (vx + vw < x1) x1 = vx + vw;
        if (vy + vh < y1) y1 = vy + vh;
        vx = x0; vy = y0; vw = x1 - x0; vh = y1 - y0;
        if (vw < 0) vw = 0;
        if (vh < 0) vh = 0;
    }
    st->scissor = 1;
    st->sx = vx;
    st->sy = C->rt.h - (vy + vh);
    st->sw = vw;
    st->sh = vh;
    st->tex = tex ? tex->hw : 0;
    st->texEnv = s->texEnv;
    if (tex) {
        st->minFilter = tex->minFilter;
        st->magFilter = tex->magFilter;
        st->wrapS = tex->wrapS;
        st->wrapT = tex->wrapT;
    }
    /* max-4: iné prostredie než MODULATE/REPLACE/DECAL na jednotke 0 alebo iné než MODULATE na
     * jednotke 1 → fragment program */
    if (tex && !gNoFpg && ((C->env[0].mode != GL_MODULATE && C->env[0].mode != GL_REPLACE &&
                            C->env[0].mode != GL_DECAL) || (gPBtex1 && C->env[1].mode != GL_MODULATE)))
        st->env = C->env;
    if (tex && gPBtex1) {
        st->tex1 = gPBtex1->hw;
        st->minFilter1 = gPBtex1->minFilter;
        st->magFilter1 = gPBtex1->magFilter;
        st->wrapS1 = gPBtex1->wrapS;
        st->wrapT1 = gPBtex1->wrapT;
    }
    if (s->fog) {
        float d;
        st->fog = 1;
        st->fogMode = s->fogMode;
        switch (s->fogMode) {                       /* f = k0 + k1 · vzdialenosť − 1 (nouveau) */
        case GL_LINEAR:
            d = s->fogEnd - s->fogStart;
            if (d > -1e-6f && d < 1e-6f)
                d = 1e-6f;
            st->fogK[0] = 2.0f + s->fogStart / d;
            st->fogK[1] = -1.0f / d;
            break;
        case GL_EXP2:
            st->fogK[0] = 1.5f;
            st->fogK[1] = -0.21f * s->fogDensity;
            break;
        default:
            st->fogMode = GL_EXP;
            st->fogK[0] = 1.5f;
            st->fogK[1] = -0.09f * s->fogDensity;
            break;
        }
        st->fogColor = pack_rgba(s->fogColor[3], s->fogColor[2], s->fogColor[1], s->fogColor[0]);
    }
    if (gPBtnl) {
        st->tnl = 1;
        st->mat = gTnlMat;
        st->mv = st->fog ? gTnlMV : 0;
        st->matGen = gTnlGen;
        st->cull = s->cull;
        st->cullFace = s->cullFace;
        st->frontFace = s->frontFace;
    }
}

static long gStatTris, gStatBatches, gStatMT, gStatTnl, gStatVB, gStatVBv, gStatFog;
static long gStatDE, gStatDEslow;             /* glDrawElements rýchla / pomalá cesta */
static NVU32 gTVerts;                        /* čas pomalej cesty (glBegin/glArrayElement) */

/* stav dávky (gPBtex, gPBtex1, gPBtnl) do GPU: textúry do VRAM, hw_state. -1 = nekresliť */
static int batch_prepare(HwState *st)
{
    if (!C->hasRT || !hw_bind(&C->rt))
        return -1;
    if (gPBtex) {
        int r = tex_prepare(gPBtex);
        if (r < 0)                                  /* hl-9: bez VRAM radšej nekresliť (čierne štvorce) */
            return -1;
        if (!r)
            gPBtex = 0;
    }
    if (gPBtex && gPBtex1) {                        /* q3-12: nahratie jednotky 1 nesmie vyhodiť jednotku 0 */
        int r;
        gTexPin = gPBtex->hw;
        r = tex_prepare(gPBtex1);
        gTexPin = 0;
        if (r < 0)
            return -1;
        if (!r)
            gPBtex1 = 0;
    }
    memset(st, 0, sizeof(*st));
    fill_state(st, gPBtex);
    if (gDump) {
        static NVU32 lastGen;
        if (st->tnl && st->matGen != lastGen) {
            const float *m = st->mat;
            lastGen = st->matGen;
            lg("  T&L matica %f %f %f %f / %f %f %f %f / %f %f %f %f / %f %f %f %f", (double)m[0], (double)m[1],
               (double)m[2], (double)m[3], (double)m[4], (double)m[5], (double)m[6], (double)m[7], (double)m[8],
               (double)m[9], (double)m[10], (double)m[11], (double)m[12], (double)m[13], (double)m[14], (double)m[15]);
        }
        NVU32 k = (NVU32)st->blend ^ st->blendSrc << 1 ^ st->blendDst << 5 ^ (NVU32)st->alphaTest << 9 ^
                  (NVU32)st->depthTest << 10 ^ (NVU32)st->depthWrite << 11 ^ st->depthFunc << 12 ^ st->colorMask ^
                  (NVU32)st->sx << 3 ^ (NVU32)st->sy << 13 ^ (NVU32)st->sw << 7 ^ (NVU32)st->sh << 17 ^
                  (NVU32)st->tnl << 30 ^ (NVU32)(gPBn > 0) << 31 ^ (NVU32)(gPBtex != 0) << 29 ^ C->s.texEnv;
        static NVU32 lastK;
        static int same;
        if (k == lastK) {
            same++;
            return hw_state(&C->rt, st);
        }
        if (same)
            lg("  (+%d rovnakych davok)", same);
        same = 0;
        lastK = k;
        lg("  davka n %d prim %d tnl %d tex %u/%u env %x blend %d %x/%x alfa %d %x %f z %d/%d %x maska %x orez %d %d %d %d "
           "farba0 %f %f %f %f",
           gPBn, gPBprim, st->tnl, gPBtex ? gPBtex->name : 0, gPBtex1 ? gPBtex1->name : 0, C->s.texEnv, st->blend,
           st->blendSrc, st->blendDst, st->alphaTest, st->alphaFunc, (double)st->alphaRef, st->depthTest,
           st->depthWrite, st->depthFunc, st->colorMask, st->sx, st->sy, st->sw, st->sh, (double)gPB[0].r,
           (double)gPB[0].g, (double)gPB[0].b, (double)gPB[0].a);
    }
    return hw_state(&C->rt, st);
}

void gl_flush_batch(void)
{
    HwState st;
    int textured;

    NVU32 t0;
    if (!gPBn || !C)
        return;
    t0 = tb();
    textured = batch_prepare(&st);
    if (textured < 0) {
        gPBn = 0;
        return;
    }
    if (gTexDbg && gTexDbgN < 3000 && first_time(0x5E000000 ^ (gPBtex ? gPBtex->name : 0) ^ ((NVU32)textured << 20) ^
                                                  ((NVU32)C->s.texEnv << 8) ^ ((NVU32)st.blend << 21))) {
        gTexDbgN++;
        lg("  (naviazana %u lv0 %x tex2D %d unit %d)", C->bound ? C->bound->name : 0xffffffff,
           C->bound ? (NVU32)C->bound->lv[0] : 0, C->s.tex2D, C->unit);
        lg("davka %d vrch.: tex %u (hw %d) env %x blend %d %x/%x alfa %d %x z %d/%d farba0 %f %f %f %f st %f %f q %f",
           gPBn, gPBtex ? gPBtex->name : 0, textured, C->s.texEnv, st.blend, st.blendSrc, st.blendDst, st.alphaTest,
           st.alphaFunc, st.depthTest, st.depthWrite, (double)gPB[0].r, (double)gPB[0].g, (double)gPB[0].b,
           (double)gPB[0].a, (double)gPB[0].sq, (double)gPB[0].tq, (double)gPB[0].q);
    }
    hw_draw(gPBprim, gPB, gPBn, textured);
    gStatTris += gPBn / 3;
    if (textured == 2)
        gStatMT += gPBn / 3;
    if (gPBtnl)
        gStatTnl += gPBn / 3;
    if (st.fog)
        gStatFog += gPBn / 3;
    gStatBatches++;
    gPBn = 0;
    gTFlush += tb() - t0;
}

/* ---- primitívy ---------------------------------------------------------------- */

/* minigl-4: pri páse 4× mal záblesk Quake 2 (štvoruholník ±100 pred kamerou) vrcholy okolo
 * -960…1600 px a na spoločnej hrane trojuholníkov čiernu čiaru (interpolácia farby obrích
 * trojuholníkov) → orezávať tesne za okrajom viewportu */
#define GUARD gGuard
/* wolf-5: šírka pásu nastaviteľná (boot-args nv34glguard2 / nv34glguard4) — pri 1,01 ide každý
 * trojuholník presahujúci okraj obrazovky cez úplné orezanie na CPU */
static long gStatClip;
static NVU32 gTClip;

/* bity mimo: 0 blízka, 1 w≤0, 2..5 ochranný pás; 8..13 frustum (na zahodenie) */
static NVU32 outcode(const V *v)
{
    NVU32 c = 0;
    float gw = GUARD * v->w;
    if (v->z < -v->w) c |= 1;
    if (v->w < 1e-6f) c |= 2;
    if (v->x > gw) c |= 4;
    if (v->x < -gw) c |= 8;
    if (v->y > gw) c |= 16;
    if (v->y < -gw) c |= 32;
    if (v->x > v->w) c |= 0x100;
    if (v->x < -v->w) c |= 0x200;
    if (v->y > v->w) c |= 0x400;
    if (v->y < -v->w) c |= 0x800;
    if (v->z > v->w) c |= 0x1000;
    if (v->z < -v->w) c |= 0x2000;
    return c;
}

static float plane_dist(const V *v, int p)
{
    switch (p) {
    case 0: return v->z + v->w;
    case 1: return v->w - 1e-6f;
    case 2: return GUARD * v->w - v->x;
    case 3: return GUARD * v->w + v->x;
    case 4: return GUARD * v->w - v->y;
    default: return GUARD * v->w + v->y;
    }
}

static void lerp(V *o, const V *a, const V *b, float t)
{
    const float *pa = &a->x, *pb = &b->x;
    float *po = &o->x;
    int i;
    for (i = 0; i < V_FLOATS; i++)
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

/* Sutherland–Hodgman; in/out polia max 3 + 6 */
static int clip_poly(V *in, int n, V *out, NVU32 planes)
{
    V tmp[16], *src = in, *dst = tmp;
    int p, i;

    for (p = 0; p < 6; p++) {
        int m = 0;
        if (!(planes & (1u << p)))
            continue;
        for (i = 0; i < n; i++) {
            const V *a = &src[i], *b = &src[(i + 1) % n];
            float da = plane_dist(a, p), db = plane_dist(b, p);
            if (da >= 0)
                dst[m++] = *a;
            if ((da >= 0) != (db >= 0) && m < 15)
                lerp(&dst[m++], a, b, da / (da - db));
        }
        n = m;
        if (n < 3)
            return 0;
        src = dst;
        dst = dst == tmp ? out : tmp;
    }
    if (src != out)
        memcpy(out, src, n * sizeof(V));
    return n;
}

static void project(HwVtx *o, const V *v, int texActive)
{
    GLS *s = &C->s;
    float iw = 1.0f / v->w;
    float nx = v->x * iw, ny = v->y * iw, nz = v->z * iw;
    float z;

    o->x = s->viewport[0] + (nx + 1.0f) * 0.5f * s->viewport[2];
    o->y = (float)C->rt.h - (s->viewport[1] + (ny + 1.0f) * 0.5f * s->viewport[3]);
    z = (float)(s->depthNear + (s->depthFar - s->depthNear) * (nz + 1.0f) * 0.5f);
    o->z = z < 0 ? 0 : z;
    if (texActive && (s->texEnv == GL_REPLACE || s->texEnv == GL_DECAL)) {
        o->r = o->g = o->b = 1.0f;
        o->a = (s->texEnv == GL_REPLACE && C->bound->hasAlpha) ? 1.0f : v->a;
    } else {
        o->r = v->r; o->g = v->g; o->b = v->b; o->a = v->a;
    }
    o->sq = v->s * iw;
    o->tq = v->t * iw;
    o->q = v->q * iw;
    o->sq1 = v->s1 * iw;
    o->tq1 = v->t1 * iw;
    o->q1 = v->q1 * iw;
    o->c = pack_rgba(o->r, o->g, o->b, o->a);
}

static void batch_begin(int prim, int need)
{
    TexObj *tex = active_tex(), *tex1 = active_tex1();
    if (gPBn && (gPBprim != prim || gPBtex != tex || gPBtex1 != tex1 || gPBtnl || gPBn + need > PB_MAX))
        gl_flush_batch();
    gPBtnl = 0;
    gPBprim = prim;
    gPBtex = tex;
    gPBtex1 = tex1;
}

static float poly_offset(const HwVtx *a, const HwVtx *b, const HwVtx *c);
static void apply_offset(HwVtx *v, int n, float off);

static void emit_poly(V *p, int n)
{
    HwVtx w[16];
    GLS *s = &C->s;
    float area = 0;
    /* minigl-3: podľa aktuálnej textúry, nie dávky (tá sa prepne až v batch_begin) — inak
     * záblesk Quake 2 po svete s GL_REPLACE dostal bielu nepriehľadnú farbu */
    int i, texActive = active_tex() != 0;

    for (i = 0; i < n; i++)
        project(&w[i], &p[i], texActive);
    for (i = 0; i < n; i++) {
        const HwVtx *a = &w[i], *b = &w[(i + 1) % n];
        area += a->x * b->y - b->x * a->y;          /* y nadol → CCW v GL má záporné */
    }
    if (area == 0)
        return;
    if (s->cull) {
        int front = (s->frontFace == GL_CCW) ? area < 0 : area > 0;
        if (s->cullFace == GL_FRONT_AND_BACK || (s->cullFace == GL_BACK && !front) ||
            (s->cullFace == GL_FRONT && front))
            return;
    }
    if (s->polyOffsetFill && (s->polyFactor != 0 || s->polyUnits != 0))
        apply_offset(w, n, poly_offset(&w[0], &w[1], &w[2]));
    batch_begin(5, (n - 2) * 3);
    for (i = 1; i + 1 < n; i++) {
        gPB[gPBn++] = w[0];
        gPB[gPBn++] = w[i];
        gPB[gPBn++] = w[i + 1];
    }
}

static void tri(const V *a, const V *b, const V *c, const V *prov)
{
    V in[3], out[16];
    NVU32 c0, c1, c2;
    int n;

    in[0] = *a; in[1] = *b; in[2] = *c;
    if (C->s.shade == GL_FLAT) {
        int i;
        for (i = 0; i < 3; i++) {
            in[i].r = prov->r; in[i].g = prov->g; in[i].b = prov->b; in[i].a = prov->a;
        }
    }
    c0 = outcode(&in[0]);
    c1 = outcode(&in[1]);
    c2 = outcode(&in[2]);
    if (c0 & c1 & c2 & 0x3f00)
        return;                                     /* celý mimo jednej roviny frustumu */
    if (!((c0 | c1 | c2) & 0x3f)) {
        emit_poly(in, 3);
        return;
    }
    {
        NVU32 t0 = tb();
        n = clip_poly(in, 3, out, (c0 | c1 | c2) & 0x3f);
        if (n >= 3)
            emit_poly(out, n);
        gStatClip++;
        gTClip += tb() - t0;
    }
}

static void line(const V *a, const V *b)
{
    V p[2];
    HwVtx w[2];
    float da, db;
    int texActive;

    p[0] = *a; p[1] = *b;
    da = p[0].z + p[0].w;
    db = p[1].z + p[1].w;
    if (da < 0 && db < 0)
        return;
    if (da < 0) lerp(&p[0], &p[0], &p[1], da / (da - db));
    else if (db < 0) lerp(&p[1], &p[1], &p[0], db / (db - da));
    if (p[0].w <= 0 || p[1].w <= 0)
        return;
    batch_begin(2, 2);
    texActive = gPBtex != 0;
    project(&w[0], &p[0], texActive);
    project(&w[1], &p[1], texActive);
    gPB[gPBn++] = w[0];
    gPB[gPBn++] = w[1];
}

static void point(const V *a)
{
    if (a->w <= 0 || (outcode(a) & 0x3f00))
        return;
    batch_begin(1, 1);
    project(&gPB[gPBn++], a, gPBtex != 0);
}

/* opt-3: rýchla cesta — celý primitív v ochrannom páse a plynulé tieňovanie: každý vrchol
 * sa premietne raz (polygóny Quake II sa rozkladajú na vejár, spoločné vrcholy sa predtým
 * premietali a testovali pri každom trojuholníku znova) */
static HwVtx gPV[VB_MAX];
static NVU32 gOC[VB_MAX];

static int fast_prepare(int n)
{
    NVU32 orAll = 0;
    int i, texActive;
    if (C->s.shade == GL_FLAT || n < 3)
        return 0;
    for (i = 0; i < n; i++) {
        gOC[i] = outcode(&C->vb[i]);
        orAll |= gOC[i];
    }
    if (orAll & 0x3f)
        return 0;
    texActive = active_tex() != 0;
    for (i = 0; i < n; i++)
        project(&gPV[i], &C->vb[i], texActive);
    return 1;
}

static void tri_fast(int a, int b, int c)
{
    const HwVtx *pa = &gPV[a], *pb = &gPV[b], *pc = &gPV[c];
    GLS *s = &C->s;
    float area;

    if (gOC[a] & gOC[b] & gOC[c] & 0x3f00)
        return;
    area = (pb->x - pa->x) * (pc->y - pa->y) - (pc->x - pa->x) * (pb->y - pa->y);
    if (area == 0)
        return;
    if (s->cull) {
        int front = (s->frontFace == GL_CCW) ? area < 0 : area > 0;
        if (s->cullFace == GL_FRONT_AND_BACK || (s->cullFace == GL_BACK && !front) ||
            (s->cullFace == GL_FRONT && front))
            return;
    }
    batch_begin(5, 3);
    gPB[gPBn++] = *pa;
    gPB[gPBn++] = *pb;
    gPB[gPBn++] = *pc;
    if (s->polyOffsetFill && (s->polyFactor != 0 || s->polyUnits != 0))
        apply_offset(&gPB[gPBn - 3], 3, poly_offset(pa, pb, pc));
}

/* spracuje vb; final = glEnd. Pri plnom vb nechá potrebné vrcholy (pásy, vejáre). */
static void assemble(int final)
{
    V *v = C->vb;
    int n = C->nv, i, keep = 0;
    int fast = C->prim >= GL_TRIANGLES && fast_prepare(n);
#define TRI(a, b, c, p) do { if (fast) tri_fast(a, b, c); else tri(&v[a], &v[b], &v[c], &v[p]); } while (0)

    switch (C->prim) {
    case GL_POINTS:
        for (i = 0; i < n; i++)
            point(&v[i]);
        break;
    case GL_LINES:
        for (i = 0; i + 1 < n; i += 2)
            line(&v[i], &v[i + 1]);
        keep = n & 1;
        break;
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        if (C->prim == GL_LINE_LOOP && !C->loopFirstValid && n) {
            C->loopFirst = v[0];
            C->loopFirstValid = 1;
        }
        for (i = 0; i + 1 < n; i++)
            line(&v[i], &v[i + 1]);
        if (final && C->prim == GL_LINE_LOOP && n > 1)
            line(&v[n - 1], &C->loopFirst);
        keep = n ? 1 : 0;
        break;
    case GL_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            TRI(i, i + 1, i + 2, i + 2);
        keep = n % 3;
        break;
    case GL_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if ((i + C->stripOdd) & 1)
                TRI(i + 1, i, i + 2, i + 2);
            else
                TRI(i, i + 1, i + 2, i + 2);
        }
        if (n >= 2) {
            C->stripOdd = (C->stripOdd + n - 2) & 1;
            keep = 2;
        } else {
            keep = n;
        }
        break;
    case GL_TRIANGLE_FAN:
    case GL_POLYGON:
        for (i = 1; i + 1 < n; i++)
            TRI(0, i, i + 1, C->prim == GL_POLYGON ? 0 : i + 1);
        if (!final && n >= 2) {                     /* prvý a posledný */
            v[1] = v[n - 1];
            C->nv = 2;
            return;
        }
        break;
    case GL_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            TRI(i, i + 1, i + 2, i + 3);
            TRI(i, i + 2, i + 3, i + 3);
        }
        keep = n & 3;
        break;
    case GL_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            TRI(i, i + 1, i + 3, i + 3);
            TRI(i, i + 3, i + 2, i + 3);
        }
        keep = n >= 2 ? 2 + (n & 1) : n;
        break;
    }
    if (final) {
        C->nv = 0;
        return;
    }
    for (i = 0; i < keep; i++)
        v[i] = v[n - keep + i];
    C->nv = keep;
}
#undef TRI

void glBegin(GLenum mode)
{
    CHECK();
    if (C->inBegin || mode > GL_POLYGON) {
        set_err(GL_INVALID_OPERATION);
        return;
    }
    if (C->mvpDirty) {
        mat_mul(C->mvp, C->mstack[1][C->msp[1]], C->mstack[0][C->msp[0]]);
        C->mvpDirty = 0;
    }
    C->inBegin = 1;
    C->prim = mode;
    C->nv = 0;
    C->stripOdd = 0;
    C->loopFirstValid = 0;
}

void glEnd(void)
{
    CHECK();
    if (!C->inBegin) {
        set_err(GL_INVALID_OPERATION);
        return;
    }
    {
        NVU32 t0 = tb();
        assemble(1);
        gTEnd += tb() - t0;
    }
    C->inBegin = 0;
}

static void vertex4(float x, float y, float z, float w)
{
    GLT;
    const float *m;
    V *v;

    if (!C || !C->inBegin)
        return;
    if (C->nv >= VB_MAX)
        assemble(0);
    m = C->mvp;
    v = &C->vb[C->nv++];
    v->x = m[0] * x + m[4] * y + m[8] * z + m[12] * w;
    v->y = m[1] * x + m[5] * y + m[9] * z + m[13] * w;
    v->z = m[2] * x + m[6] * y + m[10] * z + m[14] * w;
    v->w = m[3] * x + m[7] * y + m[11] * z + m[15] * w;
    v->r = C->cur[0];
    v->g = C->cur[1];
    v->b = C->cur[2];
    v->a = C->cur[3];
    if (C->texIdentity) {
        v->s = C->curTex[0];
        v->t = C->curTex[1];
        v->q = C->curTex[3];
    } else {
        const float *t = C->mstack[2][C->msp[2]], *c = C->curTex;
        v->s = t[0] * c[0] + t[4] * c[1] + t[8] * c[2] + t[12] * c[3];
        v->t = t[1] * c[0] + t[5] * c[1] + t[9] * c[2] + t[13] * c[3];
        v->q = t[3] * c[0] + t[7] * c[1] + t[11] * c[2] + t[15] * c[3];
    }
    v->s1 = C->curTex1[0];
    v->t1 = C->curTex1[1];
    v->q1 = C->curTex1[3];
}

/* ---- vrcholy, farby, súradnice textúr (všetky varianty) ------------------------- */

static void color4(float r, float g, float b, float a)
{
    GLT;
    if (!C)
        return;
    C->cur[0] = r; C->cur[1] = g; C->cur[2] = b; C->cur[3] = a;
}

static void texc(float s, float t, float r, float q)
{
    GLT;
    if (!C)
        return;
    C->curTex[0] = s; C->curTex[1] = t; C->curTex[2] = r; C->curTex[3] = q;
}

#define CB(x)  ((2.0f * (x) + 1.0f) / 255.0f)
#define CS(x)  ((2.0f * (x) + 1.0f) / 65535.0f)
#define CI(x)  ((float)((2.0 * (x) + 1.0) / 4294967295.0))
#define CUB(x) ((x) * (1.0f / 255.0f))
#define CUS(x) ((x) * (1.0f / 65535.0f))
#define CUI(x) ((float)((x) / 4294967295.0))
#define CF(x)  ((float)(x))

#define COLOR_FN(suf, T, cv) \
    void glColor3##suf(T r, T g, T b) { color4(cv(r), cv(g), cv(b), 1.0f); } \
    void glColor4##suf(T r, T g, T b, T a) { color4(cv(r), cv(g), cv(b), cv(a)); } \
    void glColor3##suf##v(const T *v) { color4(cv(v[0]), cv(v[1]), cv(v[2]), 1.0f); } \
    void glColor4##suf##v(const T *v) { color4(cv(v[0]), cv(v[1]), cv(v[2]), cv(v[3])); }

COLOR_FN(b, GLbyte, CB)
COLOR_FN(s, GLshort, CS)
COLOR_FN(i, GLint, CI)
COLOR_FN(ub, GLubyte, CUB)
COLOR_FN(us, GLushort, CUS)
COLOR_FN(ui, GLuint, CUI)
COLOR_FN(f, GLfloat, CF)
COLOR_FN(d, GLdouble, CF)

#define VERTEX_FN(suf, T) \
    void glVertex2##suf(T x, T y) { vertex4(CF(x), CF(y), 0.0f, 1.0f); } \
    void glVertex3##suf(T x, T y, T z) { vertex4(CF(x), CF(y), CF(z), 1.0f); } \
    void glVertex4##suf(T x, T y, T z, T w) { vertex4(CF(x), CF(y), CF(z), CF(w)); } \
    void glVertex2##suf##v(const T *v) { vertex4(CF(v[0]), CF(v[1]), 0.0f, 1.0f); } \
    void glVertex3##suf##v(const T *v) { vertex4(CF(v[0]), CF(v[1]), CF(v[2]), 1.0f); } \
    void glVertex4##suf##v(const T *v) { vertex4(CF(v[0]), CF(v[1]), CF(v[2]), CF(v[3])); } \
    void glTexCoord1##suf(T s) { texc(CF(s), 0.0f, 0.0f, 1.0f); } \
    void glTexCoord2##suf(T s, T t) { texc(CF(s), CF(t), 0.0f, 1.0f); } \
    void glTexCoord3##suf(T s, T t, T r) { texc(CF(s), CF(t), CF(r), 1.0f); } \
    void glTexCoord4##suf(T s, T t, T r, T q) { texc(CF(s), CF(t), CF(r), CF(q)); } \
    void glTexCoord1##suf##v(const T *v) { texc(CF(v[0]), 0.0f, 0.0f, 1.0f); } \
    void glTexCoord2##suf##v(const T *v) { texc(CF(v[0]), CF(v[1]), 0.0f, 1.0f); } \
    void glTexCoord3##suf##v(const T *v) { texc(CF(v[0]), CF(v[1]), CF(v[2]), 1.0f); } \
    void glTexCoord4##suf##v(const T *v) { texc(CF(v[0]), CF(v[1]), CF(v[2]), CF(v[3])); } \
    void glRect##suf(T x1, T y1, T x2, T y2) { rect(CF(x1), CF(y1), CF(x2), CF(y2)); } \
    void glRect##suf##v(const T *a, const T *b) { rect(CF(a[0]), CF(a[1]), CF(b[0]), CF(b[1])); }

static void rect(float x1, float y1, float x2, float y2)
{
    glBegin(GL_POLYGON);
    vertex4(x1, y1, 0, 1);
    vertex4(x2, y1, 0, 1);
    vertex4(x2, y2, 0, 1);
    vertex4(x1, y2, 0, 1);
    glEnd();
}

VERTEX_FN(s, GLshort)
VERTEX_FN(i, GLint)
VERTEX_FN(f, GLfloat)
VERTEX_FN(d, GLdouble)

/* normály a hrany: bez osvetlenia nepotrebné */
#define NORMAL_FN(suf, T) \
    void glNormal3##suf(T x, T y, T z) { (void)x; (void)y; (void)z; } \
    void glNormal3##suf##v(const T *v) { (void)v; }
NORMAL_FN(b, GLbyte)
NORMAL_FN(s, GLshort)
NORMAL_FN(i, GLint)
NORMAL_FN(f, GLfloat)
NORMAL_FN(d, GLdouble)

void glEdgeFlag(GLboolean f) { (void)f; }
void glEdgeFlagv(const GLboolean *f) { (void)f; }


/* ---- jednotky textúr (q3-4) ------------------------------------------------------- */

void glActiveTextureARB(GLenum u)
{
    CHECK();
    if (gTexDbg && gTexDbgN < 3000 && (int)(u - 0x84C0) != C->unit) {
        gTexDbgN++;
        lg("glActiveTextureARB %x", u);
    }
    ONCE(0x5C200000 | (u & 0xff), "glActiveTextureARB %x (prvykrat)", u);
    C->unit = u >= 0x84C0 && u < 0x84C0 + 32 ? (int)(u - 0x84C0) : 0;
    if (C->unit && !C->bound1)
        C->bound1 = &C->def;
}

void glClientActiveTextureARB(GLenum u)
{
    CHECK();
    if (gTexDbg && gTexDbgN < 3000 && (int)(u - 0x84C0) != C->cunit) {
        gTexDbgN++;
        lg("glClientActiveTextureARB %x", u);
    }
    ONCE(0x5C300000 | (u & 0xff), "glClientActiveTextureARB %x (prvykrat)", u);
    C->cunit = u >= 0x84C0 && u < 0x84C0 + 32 ? (int)(u - 0x84C0) : 0;
}

static void texc1(float s, float t, float r, float q)
{
    if (!C)
        return;
    ONCE(0x5C600000, "glMultiTexCoord jednotky 1 (prvykrat)");
    C->curTex1[0] = s; C->curTex1[1] = t; C->curTex1[2] = r; C->curTex1[3] = q;
}

#define MTC(u, s, t, r, q) do { if ((u) == 0x84C0) texc((s), (t), (r), (q)); \
                                else if ((u) == 0x84C1) texc1((s), (t), (r), (q)); } while (0)
void glMultiTexCoord1fARB(GLenum u, GLfloat s) { MTC(u, s, 0, 0, 1); }
void glMultiTexCoord2fARB(GLenum u, GLfloat s, GLfloat t) { MTC(u, s, t, 0, 1); }
void glMultiTexCoord3fARB(GLenum u, GLfloat s, GLfloat t, GLfloat r) { MTC(u, s, t, r, 1); }
void glMultiTexCoord4fARB(GLenum u, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { MTC(u, s, t, r, q); }
void glMultiTexCoord1fvARB(GLenum u, const GLfloat *v) { MTC(u, v[0], 0, 0, 1); }
void glMultiTexCoord2fvARB(GLenum u, const GLfloat *v) { MTC(u, v[0], v[1], 0, 1); }
void glMultiTexCoord3fvARB(GLenum u, const GLfloat *v) { MTC(u, v[0], v[1], v[2], 1); }
void glMultiTexCoord4fvARB(GLenum u, const GLfloat *v) { MTC(u, v[0], v[1], v[2], v[3]); }
void glMultiTexCoord2dARB(GLenum u, GLdouble s, GLdouble t) { MTC(u, (float)s, (float)t, 0, 1); }
void glMultiTexCoord2dvARB(GLenum u, const GLdouble *v) { MTC(u, (float)v[0], (float)v[1], 0, 1); }
void glMultiTexCoord2iARB(GLenum u, GLint s, GLint t) { MTC(u, (float)s, (float)t, 0, 1); }
void glMultiTexCoord2sARB(GLenum u, GLshort s, GLshort t) { MTC(u, (float)s, (float)t, 0, 1); }

/* ---- polia vrcholov ------------------------------------------------------------ */

static Arr *arr_of(GLenum a)
{
    switch (a) {
    case GL_VERTEX_ARRAY:        return &C->va;
    case GL_COLOR_ARRAY:         return &C->ca;
    case GL_TEXTURE_COORD_ARRAY: return C->cunit ? &C->ta1 : &C->ta;
    case GL_NORMAL_ARRAY:        return &C->na;
    }
    return 0;
}

void glEnableClientState(GLenum a)
{
    Arr *p;
    CHECK();
    if (gTexDbg)
        ONCE(0x57100000 | (a & 0xffff), "glEnableClientState %x", a);
    if ((p = arr_of(a)) != 0)
        p->on = 1;
    else
        ONCE(0x57000000 | (a & 0xffff), "glEnableClientState %x", a);
}

void glDisableClientState(GLenum a)
{
    Arr *p;
    CHECK();
    if ((p = arr_of(a)) != 0)
        p->on = 0;
}

static void set_arr(Arr *a, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    a->size = size;
    a->type = type;
    a->stride = stride;
    a->ptr = ptr;
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { if (C) set_arr(&C->va, size, type, stride, p); }
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { if (C) set_arr(&C->ca, size, type, stride, p); }
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { if (C) set_arr(C->cunit ? &C->ta1 : &C->ta, size, type, stride, p); }
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p) { if (C) set_arr(&C->na, 3, type, stride, p); }

static int type_size(GLenum t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_DOUBLE: return 8;
    default: return 4;
    }
}

/* prvok poľa → float[4] (farba z celých typov normalizovaná) */
static void fetch(const Arr *a, GLint i, float *out, int normalize)
{
    const char *p = (const char *)a->ptr + (long)i * (a->stride ? a->stride : a->size * type_size(a->type));
    int k;
    for (k = 0; k < a->size && k < 4; k++) {
        switch (a->type) {
        case GL_FLOAT:          out[k] = ((const float *)p)[k]; break;
        case GL_DOUBLE:         out[k] = (float)((const double *)p)[k]; break;
        case GL_SHORT:          out[k] = normalize ? CS(((const short *)p)[k]) : ((const short *)p)[k]; break;
        case GL_INT:            out[k] = normalize ? CI(((const int *)p)[k]) : ((const int *)p)[k]; break;
        case GL_UNSIGNED_BYTE:  out[k] = normalize ? CUB(((const GLubyte *)p)[k]) : ((const GLubyte *)p)[k]; break;
        case GL_BYTE:           out[k] = normalize ? CB(((const GLbyte *)p)[k]) : ((const GLbyte *)p)[k]; break;
        case GL_UNSIGNED_SHORT: out[k] = normalize ? CUS(((const GLushort *)p)[k]) : ((const GLushort *)p)[k]; break;
        case GL_UNSIGNED_INT:   out[k] = normalize ? CUI(((const GLuint *)p)[k]) : ((const GLuint *)p)[k]; break;
        default:                out[k] = 0;
        }
    }
}

void glArrayElement(GLint i)
{
    float f[4];
    CHECK();
    if (C->ca.on && C->ca.ptr) {
        f[3] = 1.0f;
        fetch(&C->ca, i, f, 1);
        color4(f[0], f[1], f[2], f[3]);
    }
    if (C->ta.on && C->ta.ptr) {
        f[1] = f[2] = 0.0f;
        f[3] = 1.0f;
        fetch(&C->ta, i, f, 0);
        texc(f[0], f[1], f[2], f[3]);
    }
    if (C->ta1.on && C->ta1.ptr) {
        f[1] = f[2] = 0.0f;
        f[3] = 1.0f;
        fetch(&C->ta1, i, f, 0);
        texc1(f[0], f[1], f[2], f[3]);
    }
    if (C->va.on && C->va.ptr) {
        f[2] = 0.0f;
        f[3] = 1.0f;
        fetch(&C->va, i, f, 0);
        vertex4(f[0], f[1], f[2], f[3]);
    }
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    GLint i;
    CHECK();
    glBegin(mode);
    for (i = 0; i < count; i++)
        glArrayElement(first + i);
    glEnd();
}

/* ---- opt-8: GL_EXT_compiled_vertex_array + rýchle glDrawElements(GL_TRIANGLES) -------------
 * Pozície vrcholov sa transformujú, otestujú a premietnu raz na vrchol (nie na index); pri
 * uzamknutých poliach (Quake III) aj raz pre všetky prechody povrchu. Farby a súradnice textúr
 * sa čítajú pri každom glDrawElements (Quake III ich medzi prechodmi mení). */
#define LC_MAX 2048
static float gLC[LC_MAX][4];                    /* clip x, y, z, w */
static float gLW[LC_MAX][4];                    /* okno x, y, z, 1/w */
static NVU32 gLO[LC_MAX];                       /* outcode */
static HwVtx gLV[LC_MAX];                       /* wolf-3: pripravený vrchol (okno, s/q, farba) */
static int gLockOn, gLockFirst, gLockCount;
static int gLcBase = -1, gLcCount, gLcLocked;   /* obsah cache */
static NVU32 gLcGen;
static int gLcRtH, gLcTnl;
static const void *gLcPtr;

static int pos_fetch(GLint i, float *o)
{
    const Arr *a = &C->va;
    if (a->type == GL_FLOAT) {
        const float *p = (const float *)((const char *)a->ptr + (long)i * (a->stride ? a->stride : a->size * 4));
        o[0] = p[0]; o[1] = p[1];
        o[2] = a->size > 2 ? p[2] : 0.0f;
        o[3] = a->size > 3 ? p[3] : 1.0f;
    } else {
        o[2] = 0.0f; o[3] = 1.0f;
        fetch(a, i, o, 0);
    }
    return 1;
}

/* wolf-4: vrchol sa transformuje až keď ho trojuholník použije (Wolfenstein kreslí malé kúsky
 * z veľkého poľa — predtým sa pri každom volaní prepočítal celý rozsah indexov) */
static NVU32 gPStamp[LC_MAX], gAStamp[LC_MAX], gPGen = 1, gAGen = 1;
static float gVpX, gVpY, gHw, gHh, gDn, gDd, gRtH;

static void lc_build(int base, int n, int locked)
{
    GLS *s = &C->s;
    if (C->mvpDirty) {
        mat_mul(C->mvp, C->mstack[1][C->msp[1]], C->mstack[0][C->msp[0]]);
        C->mvpDirty = 0;
    }
    gVpX = (float)s->viewport[0];
    gVpY = (float)s->viewport[1];
    gHw = s->viewport[2] * 0.5f;
    gHh = s->viewport[3] * 0.5f;
    gDn = (float)s->depthNear;
    gDd = (float)((s->depthFar - s->depthNear) * 0.5);
    gRtH = (float)C->rt.h;
    if (++gPGen == 0) {                             /* pretečenie: značky vynulovať */
        memset(gPStamp, 0, sizeof(gPStamp));
        gPGen = 1;
    }
    gLcBase = base;
    gLcCount = n;
    gLcLocked = locked;
    gLcGen = gXformGen;
    gLcRtH = C->rt.h;
    gLcPtr = C->va.ptr;
}

/* q3-16: pri HW T&L ostáva vrchol v súradniciach objektu (gLW = x, y, z, 1) */
static void lc_pos_tnl(int ci)
{
    float v[4], *w = gLW[ci];
    pos_fetch(gLcBase + ci, v);
    w[0] = v[0]; w[1] = v[1]; w[2] = v[2]; w[3] = 1.0f;
    gLO[ci] = 0;
    gPStamp[ci] = gPGen;
}

/* zložená matica pre GPU: riadky okna (y dole, hĺbka depthNear…depthFar) × MVP */
static void tnl_matrix(void)
{
    GLS *s = &C->s;
    const float *m, *mv;
    float hw, hh, tx, ty, dd, tz;
    int c;
    if (gTnlGen && gTnlXf == gXformGen && gTnlRtH == C->rt.h)
        return;
    if (gPBn && gPBtnl)                             /* dávka ešte so starou maticou */
        gl_flush_batch();
    if (C->mvpDirty) {
        mat_mul(C->mvp, C->mstack[1][C->msp[1]], C->mstack[0][C->msp[0]]);
        C->mvpDirty = 0;
    }
    m = C->mvp;
    hw = s->viewport[2] * 0.5f;
    hh = s->viewport[3] * 0.5f;
    tx = s->viewport[0] + hw;
    ty = (float)C->rt.h - s->viewport[1] - hh;
    dd = (float)((s->depthFar - s->depthNear) * 0.5);
    tz = (float)s->depthNear + dd;
    for (c = 0; c < 4; c++) {
        const float *col = m + c * 4;
        gTnlMat[c] = hw * col[0] + tx * col[3];
        gTnlMat[4 + c] = -hh * col[1] + ty * col[3];
        gTnlMat[8 + c] = dd * col[2] + tz * col[3];
        gTnlMat[12 + c] = col[3];
    }
    mv = C->mstack[0][C->msp[0]];
    for (c = 0; c < 4; c++) {
        gTnlMV[c] = mv[c * 4];
        gTnlMV[4 + c] = mv[c * 4 + 1];
        gTnlMV[8 + c] = mv[c * 4 + 2];
        gTnlMV[12 + c] = mv[c * 4 + 3];
    }
    if (!gTnlGen)
        lg("HW T&L: prva matica %f %f %f %f / %f %f %f %f / %f %f %f %f / %f %f %f %f",
           (double)gTnlMat[0], (double)gTnlMat[1], (double)gTnlMat[2], (double)gTnlMat[3], (double)gTnlMat[4],
           (double)gTnlMat[5], (double)gTnlMat[6], (double)gTnlMat[7], (double)gTnlMat[8], (double)gTnlMat[9],
           (double)gTnlMat[10], (double)gTnlMat[11], (double)gTnlMat[12], (double)gTnlMat[13], (double)gTnlMat[14],
           (double)gTnlMat[15]);
    gTnlXf = gXformGen;
    gTnlRtH = C->rt.h;
    if (++gTnlGen == 0)
        gTnlGen = 1;
}

static void lc_pos(int ci)
{
    const float *m = C->mvp;
    float v[4], *c = gLC[ci];
    V tmp;
    pos_fetch(gLcBase + ci, v);
    c[0] = m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12] * v[3];
    c[1] = m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13] * v[3];
    c[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] * v[3];
    c[3] = m[3] * v[0] + m[7] * v[1] + m[11] * v[2] + m[15] * v[3];
    tmp.x = c[0]; tmp.y = c[1]; tmp.z = c[2]; tmp.w = c[3];
    gLO[ci] = outcode(&tmp);
    if (!(gLO[ci] & 0x3f)) {
        float iw = 1.0f / c[3], z;
        gLW[ci][0] = gVpX + (c[0] * iw + 1.0f) * gHw;
        gLW[ci][1] = gRtH - (gVpY + (c[1] * iw + 1.0f) * gHh);
        z = gDn + gDd * (c[2] * iw + 1.0f);
        gLW[ci][2] = z < 0 ? 0 : z;
        gLW[ci][3] = iw;
    }
    gPStamp[ci] = gPGen;
}

void glLockArraysEXT(GLint first, GLsizei count)
{
    CHECK();
    gLockOn = 1;
    gLockFirst = first;
    gLockCount = count;
    gLcBase = -1;
}

void glUnlockArraysEXT(void)
{
    CHECK();
    gLockOn = 0;
    gLcBase = -1;
}

/* farba a súradnica textúry indexu k (polia alebo aktuálne hodnoty) */
static void attr_fetch(GLint k, float *col, float *st)
{
    if (C->ca.on && C->ca.ptr) {
        if (C->ca.type == GL_UNSIGNED_BYTE && C->ca.size == 4) {
            const GLubyte *b = (const GLubyte *)C->ca.ptr + (long)k * (C->ca.stride ? C->ca.stride : 4);
            col[0] = b[0] * (1.0f / 255.0f); col[1] = b[1] * (1.0f / 255.0f);
            col[2] = b[2] * (1.0f / 255.0f); col[3] = b[3] * (1.0f / 255.0f);
        } else {
            col[3] = 1.0f;
            fetch(&C->ca, k, col, 1);
        }
    } else {
        col[0] = C->cur[0]; col[1] = C->cur[1]; col[2] = C->cur[2]; col[3] = C->cur[3];
    }
    if (C->ta.on && C->ta.ptr) {
        if (C->ta.type == GL_FLOAT && C->ta.size == 2) {
            const float *p = (const float *)((const char *)C->ta.ptr + (long)k * (C->ta.stride ? C->ta.stride : 8));
            st[0] = p[0]; st[1] = p[1]; st[2] = 1.0f;
        } else {
            float f[4] = { 0, 0, 0, 1 };
            fetch(&C->ta, k, f, 0);
            st[0] = f[0]; st[1] = f[1]; st[2] = f[3];
        }
    } else {
        st[0] = C->curTex[0]; st[1] = C->curTex[1]; st[2] = C->curTex[3];
    }
    if (C->ta1.on && C->ta1.ptr) {
        float f[4] = { 0, 0, 0, 1 };
        fetch(&C->ta1, k, f, 0);
        st[3] = f[0]; st[4] = f[1]; st[5] = f[3];
    } else {
        st[3] = C->curTex1[0]; st[4] = C->curTex1[1]; st[5] = C->curTex1[3];
    }
}

static struct {
    const GLubyte *cb; long cstride;
    const float *tp; long tstride;
    const float *tp1; long tstride1;           /* q3-12: jednotka 1 (len float × 2) */
    int tex1;
    NVU32 ccur;
    int rep, repA;
} gPrep;

static void lc_attr(int ci)
{
    GLint k = gLcBase + ci;
    HwVtx *w = &gLV[ci];
    const float *p = gLW[ci];
    NVU32 c;
    float sv, tv, qv = 1.0f;
    if (gPrep.cb) {
        const GLubyte *b = gPrep.cb + (long)k * gPrep.cstride;
        c = ((NVU32)b[0] << 24) | ((NVU32)b[1] << 16) | ((NVU32)b[2] << 8) | b[3];
    } else if (C->ca.on && C->ca.ptr) {
        float col[4] = { 0, 0, 0, 1 };
        fetch(&C->ca, k, col, 1);
        c = pack_rgba(col[0], col[1], col[2], col[3]);
    } else {
        c = gPrep.ccur;
    }
    if (gPrep.rep)
        c = 0xFFFFFF00 | (gPrep.repA ? 0xFF : (c & 0xFF));
    if (gPrep.tp) {
        const float *q = (const float *)((const char *)gPrep.tp + (long)k * gPrep.tstride);
        sv = q[0];
        tv = q[1];
    } else if (C->ta.on && C->ta.ptr) {
        float f[4] = { 0, 0, 0, 1 };
        fetch(&C->ta, k, f, 0);
        sv = f[0]; tv = f[1]; qv = f[3];
    } else {
        sv = C->curTex[0]; tv = C->curTex[1]; qv = C->curTex[3];
    }
    w->x = p[0]; w->y = p[1]; w->z = p[2];
    w->sq = sv * p[3];
    w->tq = tv * p[3];
    w->q = qv * p[3];
    w->c = c;
    if (gPrep.tex1) {
        float s1, t1, q1 = 1.0f;
        if (gPrep.tp1) {
            const float *q = (const float *)((const char *)gPrep.tp1 + (long)k * gPrep.tstride1);
            s1 = q[0];
            t1 = q[1];
        } else if (C->ta1.on && C->ta1.ptr) {
            float f[4] = { 0, 0, 0, 1 };
            fetch(&C->ta1, k, f, 0);
            s1 = f[0]; t1 = f[1]; q1 = f[3];
        } else {
            s1 = C->curTex1[0]; t1 = C->curTex1[1]; q1 = C->curTex1[3];
        }
        w->sq1 = s1 * p[3];
        w->tq1 = t1 * p[3];
        w->q1 = q1 * p[3];
    }
    gAStamp[ci] = gAGen;
}

/* q3-11: počty v rýchlej ceste (q3-12: bez meračov času vo vnútornej slučke) */
static NVU32 gNPos, gNAttr, gNFastTri;

/* q3-18: T&L s vrcholmi vo VRAM — každý použitý vrchol volania sa zapíše raz (slot podľa prvého
 * použitia), trojuholníky idú ako 16-bit indexy (len boot-args nv34glvb, GPU ich v q3-19 odmietol;
 * nv34glvbbatch = vrcholy v poradí trojuholníkov bez indexov).
 * q3-20 predvolene: inline vrcholy priamo z gLV podľa indexov, bez kópie do dávky.
 * 0 = nedá sa (stará cesta cez dávku). */
#define IDX_MAX 32768
static NVU16 gIdx[IDX_MAX];
static NVU16 gVSlot[LC_MAX];

static int draw_tris_vb(GLsizei count, GLenum type, const GLvoid *idx, TexObj *tx0, TexObj *tx1)
{
    HwState st;
    volatile NVU32 *vb = 0;
    NVU32 off, t0;
    int textured, wpv, nv = 0, useVb, batch;
    GLsizei i;

    count -= count % 3;
    if (count > IDX_MAX)
        return 0;
    useVb = hw_vb_ready();
    batch = useVb && hw_vb_batch();
    if (gPBn)
        gl_flush_batch();
    t0 = tb();
    gPBtex = tx0;
    gPBtex1 = tx1;
    gPBtnl = 1;
    gPBprim = 5;
    textured = batch_prepare(&st);
    if (textured < 0)
        DLOG("  ^ DrawElements %d indexov sa nekresli (textura)", (int)count);
    if (textured < 0)
        return 1;
    if (!useVb && !hw_gather_ok())
        return 0;
    wpv = textured == 2 ? 8 : textured ? 6 : 4;
    if (useVb) {
        int maxv = batch ? count : count < gLcCount ? count : gLcCount;
        vb = hw_vb_begin((NVU32)maxv * wpv * 4);
        if (!vb)
            return 0;
    }
    gTFlush += tb() - t0;
    for (i = 0; i < count; i++) {
        int k = (type == GL_UNSIGNED_BYTE ? ((const GLubyte *)idx)[i] :
                 type == GL_UNSIGNED_SHORT ? ((const GLushort *)idx)[i] : (GLint)((const GLuint *)idx)[i]) - gLcBase;
        if (gPStamp[k] != gPGen) {
            lc_pos_tnl(k);
            gNPos++;
        }
        if (gAStamp[k] != gAGen) {
            lc_attr(k);
            gNAttr++;
            if (vb && !batch) {
                gVSlot[k] = (NVU16)nv;
                hw_vb_put(vb + nv * wpv, &gLV[k], textured);
                nv++;
            }
        }
        if (batch) {
            hw_vb_put(vb + i * wpv, &gLV[k], textured);
            nv++;
        } else {
            gIdx[i] = vb ? gVSlot[k] : (NVU16)k;
        }
    }
    if (st.blend && st.depthFunc == GL_ALWAYS && count > 0) {
        int k = (type == GL_UNSIGNED_BYTE ? ((const GLubyte *)idx)[0] :
                 type == GL_UNSIGNED_SHORT ? ((const GLushort *)idx)[0] : (GLint)((const GLuint *)idx)[0]) - gLcBase;
        gFrOvlN++;
        if ((gLV[k].c & 0xff) >= (gFrOvlMax & 0xff)) {
            gFrOvlMax = gLV[k].c;
            gFrOvlC = gPBtex ? gPBtex->name : 0;
        }
    }
    if (gDump && st.blend && count > 0) {
        int k = (type == GL_UNSIGNED_BYTE ? ((const GLubyte *)idx)[0] :
                 type == GL_UNSIGNED_SHORT ? ((const GLushort *)idx)[0] : (GLint)((const GLuint *)idx)[0]) - gLcBase;
        const HwVtx *w = &gLV[k];
        const HwEnvUnit *e = &C->env[0];
        lg("    %d idx, v0 farba %x (pole %d, aktualna %f %f %f %f) poz %f %f %f st %f %f; j0 %x %x/%x z %x %x %x / %x %x %x "
           "alfa z %x %x %x / %x %x %x",
           (int)count, w->c, C->ca.on, (double)C->cur[0], (double)C->cur[1], (double)C->cur[2], (double)C->cur[3],
           (double)w->x, (double)w->y, (double)w->z, (double)w->sq, (double)w->tq, e->mode, e->comb[0], e->comb[1],
           e->src[0][0], e->src[0][1], e->src[0][2], e->op[0][0], e->op[0][1], e->op[0][2], e->src[1][0], e->src[1][1],
           e->src[1][2], e->op[1][0], e->op[1][1], e->op[1][2]);
    }
    t0 = tb();
    if (vb) {
        off = hw_vb_end((NVU32)nv * wpv * 4);
        hw_draw_indexed(5, off, textured, batch ? 0 : gIdx, count);
        gStatVB += count / 3;
        gStatVBv += nv;
    } else {
        hw_draw_gather(5, gLV, gIdx, count, textured);
    }
    gTFlush += tb() - t0;
    gNFastTri += count / 3;
    gStatTris += count / 3;
    gStatTnl += count / 3;
    if (textured == 2)
        gStatMT += count / 3;
    if (st.fog)
        gStatFog += count / 3;
    gStatBatches++;
    return 1;
}

static int draw_tris_fast(GLsizei count, GLenum type, const GLvoid *idx)
{
    GLS *s = &C->s;
    GLint mn = 0x7fffffff, mx = -1, k;
    GLsizei i;
    int texActive, rep, repA, offs, tnl;
    TexObj *tx0, *tx1;

    for (i = 0; i < count; i++) {
        k = type == GL_UNSIGNED_BYTE ? ((const GLubyte *)idx)[i] :
            type == GL_UNSIGNED_SHORT ? ((const GLushort *)idx)[i] : (GLint)((const GLuint *)idx)[i];
        if (k < mn) mn = k;
        if (k > mx) mx = k;
    }
    if (mx < 0)
        return 1;
    /* q3-16: HW T&L (bez polygon offsetu — ten počíta CPU v súradniciach okna) */
    offs = s->polyOffsetFill && (s->polyFactor != 0 || s->polyUnits != 0);
    tnl = gTnl && !offs && C->va.size <= 3;
    if (gLockOn && mn >= gLockFirst && mx < gLockFirst + gLockCount && gLockCount <= LC_MAX) {
        if (!gLcLocked || gLcBase != gLockFirst || gLcCount != gLockCount || gLcGen != gXformGen || gLcRtH != C->rt.h ||
            gLcPtr != C->va.ptr || gLcTnl != tnl)
            lc_build(gLockFirst, gLockCount, 1);
    } else if (mx - mn + 1 <= LC_MAX) {
        lc_build(mn, mx - mn + 1, 0);
    } else {
        return 0;
    }
    gLcTnl = tnl;
    if (tnl)
        tnl_matrix();
    tx0 = active_tex();
    tx1 = active_tex1();
    texActive = tx0 != 0;
    rep = texActive && (s->texEnv == GL_REPLACE || s->texEnv == GL_DECAL);
    repA = s->texEnv == GL_REPLACE && C->bound->hasAlpha;
    /* wolf-3/4: vrchol (okno, s/q, zabalená farba) pripravený raz za volanie, až pri prvom použití */
    {
        int fastc = C->ca.on && C->ca.ptr && C->ca.type == GL_UNSIGNED_BYTE && C->ca.size == 4;
        int fastt = C->ta.on && C->ta.ptr && C->ta.type == GL_FLOAT && C->ta.size == 2;
        gPrep.cb = fastc ? (const GLubyte *)C->ca.ptr : 0;
        gPrep.cstride = fastc ? (C->ca.stride ? C->ca.stride : 4) : 0;
        gPrep.tp = fastt ? (const float *)C->ta.ptr : 0;
        gPrep.tstride = fastt ? (C->ta.stride ? C->ta.stride : 8) : 0;
        gPrep.ccur = pack_rgba(C->cur[0], C->cur[1], C->cur[2], C->cur[3]);
        gPrep.rep = rep;
        gPrep.repA = repA;
        gPrep.tex1 = tx1 != 0;
        fastt = gPrep.tex1 && C->ta1.on && C->ta1.ptr && C->ta1.type == GL_FLOAT && C->ta1.size == 2;
        gPrep.tp1 = fastt ? (const float *)C->ta1.ptr : 0;
        gPrep.tstride1 = fastt ? (C->ta1.stride ? C->ta1.stride : 8) : 0;
        if (++gAGen == 0) {
            memset(gAStamp, 0, sizeof(gAStamp));
            gAGen = 1;
        }
    }
    if (tnl && draw_tris_vb(count, type, idx, tx0, tx1))
        return 1;
    if (tnl) {                                      /* orezanie a odvrátené steny robí GPU */
        for (i = 0; i + 2 < count; i += 3) {
            int j, c[3];
            for (j = 0; j < 3; j++) {
                GLsizei q = i + j;
                c[j] = (type == GL_UNSIGNED_BYTE ? ((const GLubyte *)idx)[q] :
                        type == GL_UNSIGNED_SHORT ? ((const GLushort *)idx)[q] : (GLint)((const GLuint *)idx)[q]) -
                       gLcBase;
                if (gPStamp[c[j]] != gPGen) {
                    lc_pos_tnl(c[j]);
                    gNPos++;
                }
                if (gAStamp[c[j]] != gAGen) {
                    lc_attr(c[j]);
                    gNAttr++;
                }
            }
            gNFastTri++;
            if (gPBn && (gPBprim != 5 || gPBtex != tx0 || gPBtex1 != tx1 || !gPBtnl || gPBn + 3 > PB_MAX))
                gl_flush_batch();
            gPBtnl = 1;
            gPBprim = 5;
            gPBtex = tx0;
            gPBtex1 = tx1;
            gPB[gPBn] = gLV[c[0]];
            gPB[gPBn + 1] = gLV[c[1]];
            gPB[gPBn + 2] = gLV[c[2]];
            gPBn += 3;
        }
        return 1;
    }
    for (i = 0; i + 2 < count; i += 3) {
        GLint id[3];
        int j, c[3];
        NVU32 oa, ob, oc;
        for (j = 0; j < 3; j++) {
            GLsizei q = i + j;
            id[j] = type == GL_UNSIGNED_BYTE ? ((const GLubyte *)idx)[q] :
                    type == GL_UNSIGNED_SHORT ? ((const GLushort *)idx)[q] : (GLint)((const GLuint *)idx)[q];
            c[j] = id[j] - gLcBase;
            if (gPStamp[c[j]] != gPGen) {
                lc_pos(c[j]);
                gNPos++;
            }
        }
        oa = gLO[c[0]]; ob = gLO[c[1]]; oc = gLO[c[2]];
        if (oa & ob & oc & 0x3f00)
            continue;
        if ((oa | ob | oc) & 0x3f) {
            V v[3];
            for (j = 0; j < 3; j++) {
                float col[4], st[6];
                attr_fetch(id[j], col, st);
                v[j].x = gLC[c[j]][0]; v[j].y = gLC[c[j]][1]; v[j].z = gLC[c[j]][2]; v[j].w = gLC[c[j]][3];
                v[j].r = col[0]; v[j].g = col[1]; v[j].b = col[2]; v[j].a = col[3];
                v[j].s = st[0]; v[j].t = st[1]; v[j].q = st[2];
                v[j].s1 = st[3]; v[j].t1 = st[4]; v[j].q1 = st[5];
            }
            tri(&v[0], &v[1], &v[2], &v[2]);
            continue;
        }
        for (j = 0; j < 3; j++)
            if (gAStamp[c[j]] != gAGen) {
                lc_attr(c[j]);
                gNAttr++;
            }
        gNFastTri++;
        {
            const HwVtx *pa = &gLV[c[0]], *pb = &gLV[c[1]], *pc = &gLV[c[2]];
            float area = (pb->x - pa->x) * (pc->y - pa->y) - (pc->x - pa->x) * (pb->y - pa->y);
            if (area == 0)
                continue;
            if (s->cull) {
                int front = (s->frontFace == GL_CCW) ? area < 0 : area > 0;
                if (s->cullFace == GL_FRONT_AND_BACK || (s->cullFace == GL_BACK && !front) ||
                    (s->cullFace == GL_FRONT && front))
                    continue;
            }
            if (gPBn && (gPBprim != 5 || gPBtex != tx0 || gPBtex1 != tx1 || gPBtnl || gPBn + 3 > PB_MAX))
                gl_flush_batch();
            gPBtnl = 0;
            gPBprim = 5;
            gPBtex = tx0;
            gPBtex1 = tx1;
            gPB[gPBn] = *pa;
            gPB[gPBn + 1] = *pb;
            gPB[gPBn + 2] = *pc;
            gPBn += 3;
            if (offs)
                apply_offset(&gPB[gPBn - 3], 3, poly_offset(pa, pb, pc));
        }
    }
    return 1;
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *idx)
{
    GLsizei i;
    CHECK();
    static int nDE;
    if (gTexDbg && gTexDbgN < 3000 && nDE < 20) {
        gTexDbgN++;
        nDE++;
        lg("DrawElements mode %x n %d typ %x; v %d/%x/%d t %d/%d/%x/%d c %d/%d/%x/%d", mode, count, type,
           C->va.on, C->va.type, C->va.stride, C->ta.on, C->ta.size, C->ta.type, C->ta.stride, C->ca.on, C->ca.size,
           C->ca.type, C->ca.stride);
    }
    {
        /* wolf-2: čas rýchlej cesty aj glArrayElement do štatistiky (glEnd) */
        NVU32 t0 = tb(), fl0 = gTFlush;
        if (mode == GL_TRIANGLES && !C->inBegin && C->va.on && C->va.ptr && C->s.shade != GL_FLAT && C->texIdentity &&
            count >= 3 && draw_tris_fast(count, type, idx)) {
            gTEnd += tb() - t0 - (gTFlush - fl0);
            gStatDE++;
            return;
        }
        gStatDEslow++;
        glBegin(mode);
        for (i = 0; i < count; i++) {
            GLint k = type == GL_UNSIGNED_BYTE ? ((const GLubyte *)idx)[i] :
                      type == GL_UNSIGNED_SHORT ? ((const GLushort *)idx)[i] : (GLint)((const GLuint *)idx)[i];
            glArrayElement(k);
        }
        glEnd();
        gTVerts += tb() - t0 - (gTFlush - fl0);
    }
}

void glGetPointerv(GLenum pname, GLvoid **p)
{
    Arr *a;
    *p = 0;
    if (!C)
        return;
    switch (pname) {
    case 0x808E: a = &C->va; break;             /* GL_VERTEX_ARRAY_POINTER */
    case 0x8090: a = &C->ca; break;
    case 0x8092: a = &C->ta; break;
    case 0x808F: a = &C->na; break;
    default: return;
    }
    *p = (GLvoid *)a->ptr;
}

/* ---- stav ------------------------------------------------------------------- */

static int *cap_ptr(GLenum cap)
{
    GLS *s = &C->s;
    switch (cap) {
    case GL_BLEND:        return &s->blend;
    case GL_ALPHA_TEST:   return &s->alphaTest;
    case GL_DEPTH_TEST:   return &s->depthTest;
    case GL_CULL_FACE:    return &s->cull;
    case GL_SCISSOR_TEST: return &s->scissorTest;
    case GL_TEXTURE_2D:   return &s->tex2D;
    case GL_FOG:          return &s->fog;
    case GL_DITHER:       return &s->dither;
    case GL_POLYGON_OFFSET_FILL: return &s->polyOffsetFill;
    }
    return 0;
}

static void enable(GLenum cap, int on)
{
    int *p;
    CHECK();
    DLOG("  %s %x", on ? "glEnable" : "glDisable", cap);
    if (gTexDbg && gTexDbgN < 3000 && cap == GL_TEXTURE_2D) {
        static int last[2] = { -1, -1 };
        int u = C->unit ? 1 : 0;
        if (last[u] != on) {
            last[u] = on;
            gTexDbgN++;
            lg("%s(GL_TEXTURE_2D) jednotka %d", on ? "glEnable" : "glDisable", C->unit);
        }
    }
    if (C->unit && cap == GL_TEXTURE_2D) {
        ONCE(0x5C500000 | on, "%s(GL_TEXTURE_2D) na jednotke %d (prvykrat)", on ? "glEnable" : "glDisable", C->unit);
        C->tex2D1 = on;                             /* dávka sa prepne v batch_begin (active_tex1) */
        return;
    }
    p = cap_ptr(cap);
    if (!p) {
        if (on)
            ONCE(0x58000000 | (cap & 0xffff), "glEnable %x ignorovane", cap);
        return;
    }
    if (*p == on)
        return;
    FLUSH();
    *p = on;
    if (cap == GL_FOG && on)
        ONCE(0x58100000, "glEnable GL_FOG (prvykrat): rezim %x, hustota %f, %f…%f, farba %f %f %f",
             C->s.fogMode, (double)C->s.fogDensity, (double)C->s.fogStart, (double)C->s.fogEnd,
             (double)C->s.fogColor[0], (double)C->s.fogColor[1], (double)C->s.fogColor[2]);
}

void glEnable(GLenum cap) { enable(cap, 1); }
void glDisable(GLenum cap) { enable(cap, 0); }

GLboolean glIsEnabled(GLenum cap)
{
    int *p;
    if (!C)
        return GL_FALSE;
    if ((p = cap_ptr(cap)) != 0)
        return *p ? GL_TRUE : GL_FALSE;
    {
        Arr *a = arr_of(cap);
        if (a)
            return a->on ? GL_TRUE : GL_FALSE;
    }
    return GL_FALSE;
}

void glBlendFunc(GLenum src, GLenum dst)
{
    CHECK();
    if (C->s.blendSrc == src && C->s.blendDst == dst)
        return;
    FLUSH();
    C->s.blendSrc = src;
    C->s.blendDst = dst;
}

void glAlphaFunc(GLenum func, GLclampf ref)
{
    CHECK();
    FLUSH();
    C->s.alphaFunc = func;
    C->s.alphaRef = ref;
}

void glDepthFunc(GLenum func)
{
    CHECK();
    if (C->s.depthFunc == func)
        return;
    FLUSH();
    C->s.depthFunc = func;
}

void glDepthMask(GLboolean m)
{
    CHECK();
    if (C->s.depthMask == (m != 0))
        return;
    FLUSH();
    C->s.depthMask = m != 0;
}

void glDepthRange(GLclampd n, GLclampd f)
{
    CHECK();
    FLUSH();
    C->s.depthNear = n < 0 ? 0 : n > 1 ? 1 : n;
    C->s.depthFar = f < 0 ? 0 : f > 1 ? 1 : f;
    gXformGen++;
}

void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    CHECK();
    ONCE(0x5C100000 | (r << 3) | (g << 2) | (b << 1) | a, "glColorMask %d %d %d %d", r, g, b, a);
    FLUSH();
    C->s.cmask[0] = r; C->s.cmask[1] = g; C->s.cmask[2] = b; C->s.cmask[3] = a;
}

void glCullFace(GLenum m)
{
    CHECK();
    FLUSH();
    C->s.cullFace = m;
}

void glFrontFace(GLenum m)
{
    CHECK();
    FLUSH();
    C->s.frontFace = m;
}

void glShadeModel(GLenum m)
{
    CHECK();
    C->s.shade = m;
}

void glPolygonMode(GLenum face, GLenum mode)
{
    CHECK();
    (void)face;
    if (mode != GL_FILL)
        ONCE(0x59000000 | (mode & 0xffff), "glPolygonMode %x (kreslim plne)", mode);
    C->s.polyMode = mode;
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    CHECK();
    if (gDbg && (C->s.viewport[0] != x || C->s.viewport[1] != y || C->s.viewport[2] != w || C->s.viewport[3] != h))
        lg("glViewport %d %d %d %d", x, y, w, h);
    FLUSH();
    DLOG("  glViewport %d %d %d %d", x, y, w, h);
    if (C->hasRT && h != C->rt.h && h > 0)
        gFrVpOdd = 1;
    C->s.viewport[0] = x; C->s.viewport[1] = y; C->s.viewport[2] = w; C->s.viewport[3] = h;
    C->viewportSet = 1;
    gXformGen++;
}

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    CHECK();
    FLUSH();
    DLOG("  glScissor %d %d %d %d", x, y, w, h);
    C->s.scissor[0] = x; C->s.scissor[1] = y; C->s.scissor[2] = w; C->s.scissor[3] = h;
}

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    CHECK();
    C->s.clearColor[0] = r; C->s.clearColor[1] = g; C->s.clearColor[2] = b; C->s.clearColor[3] = a;
}

void glClearDepth(GLclampd d)
{
    CHECK();
    C->s.clearDepth = d < 0 ? 0 : d > 1 ? 1 : d;
}

static NVU32 ub(float f)
{
    if (!(f > 0))
        return 0;
    if (f >= 1)
        return 255;
    return (NVU32)(f * 255.0f + 0.5f);
}

static int gClrHw = -1;

void glClear(GLbitfield mask)
{
    HwState st;
    NVU32 argb, z;
    int color;
    GLS *s;

    CHECK();
    FLUSH();
    if (!C->hasRT)
        return;
    s = &C->s;
    memset(&st, 0, sizeof(st));
    fill_state(&st, 0);
    st.scissor = s->scissorTest;
    if (st.scissor) {
        st.sx = s->scissor[0];
        st.sy = C->rt.h - (s->scissor[1] + s->scissor[3]);
        st.sw = s->scissor[2];
        st.sh = s->scissor[3];
    }
    argb = (ub(s->clearColor[3]) << 24) | (ub(s->clearColor[0]) << 16) | (ub(s->clearColor[1]) << 8) | ub(s->clearColor[2]);
    gFrClears++;
    DLOG("  glClear %x farba %x z %x scissor %d (%d %d %d %d) viewport %d %d %d %d maska %x", mask, argb,
         (NVU32)(s->clearDepth * 1000), s->scissorTest, s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3],
         s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3], st.colorMask);
    gFrClearMask |= mask;
    gFrClearArgb = argb;
    if (C->rt.z16) {
        NVU32 v = (NVU32)(s->clearDepth * 65535.0 + 0.5);
        z = (v << 16) | v;
    } else {
        z = (NVU32)(s->clearDepth * 16777215.0 + 0.5) << 8;
    }
    color = (mask & GL_COLOR_BUFFER_BIT) != 0;
    /* max-16: farebný clear so scissorom (letterbox Max Payne) ako obdĺžnik — CLEAR_BUFFERS so scissorom
     * po niektorých dávkach zmazal celú obrazovku (cutscény čierne). nv34glclrhw = pôvodný clear */
    if (gClrHw < 0)
        gClrHw = gl_boot_arg("nv34glclrhw");
    if (color && st.scissor && !gClrHw) {
        if (st.sw > 0 && st.sh > 0 && st.colorMask) {
            HwState q = st;
            HwVtx v[6];
            float x0 = (float)st.sx, y0 = (float)st.sy, x1 = x0 + st.sw, y1 = y0 + st.sh;
            NVU32 c = pack_rgba(s->clearColor[0], s->clearColor[1], s->clearColor[2], s->clearColor[3]);
            int i;
            q.tex = q.tex1 = 0;
            q.env = 0;
            q.blend = q.alphaTest = q.depthTest = q.depthWrite = q.fog = q.tnl = q.cull = 0;
            q.mv = 0;
            memset(v, 0, sizeof(v));
            v[0].x = x0; v[0].y = y0;
            v[1].x = x1; v[1].y = y0;
            v[2].x = x1; v[2].y = y1;
            v[3].x = x0; v[3].y = y0;
            v[4].x = x1; v[4].y = y1;
            v[5].x = x0; v[5].y = y1;
            for (i = 0; i < 6; i++) {
                v[i].r = s->clearColor[0]; v[i].g = s->clearColor[1]; v[i].b = s->clearColor[2];
                v[i].a = s->clearColor[3]; v[i].q = 1.0f; v[i].c = c;
            }
            if (hw_state(&C->rt, &q) >= 0)
                hw_draw(5, v, 6, 0);
        }
        color = 0;
        ONCE(0x5C700000, "glClear farby so scissorom: kreslim obdlznik (nv34glclrhw = hardverovy clear)");
    }
    hw_clear(&C->rt, color, (mask & GL_DEPTH_BUFFER_BIT) && C->depthBits > 0 && s->depthMask, argb, z, &st);
}

/* fps-4: počítadlo fps v ľavom hornom rohu (nv34glshowfps) — číslice 3×5 z obdĺžnikov
 * na čiernom pozadí, kreslené do zadného bufferu tesne pred výmenou */
static const unsigned short gDigit[10] = {      /* 5 riadkov po 3 bity, horný riadok v bitoch 14..12 */
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF
};
static int gShowFps = -1;
static NVU32 gFpsShown, gFpsTb, gFpsFrames;

static int fps_quad(HwVtx *v, float x0, float y0, float x1, float y1, NVU32 c)
{
    float k = (c & 0xFF000000) ? 1.0f : 0.0f;
    int i;
    memset(v, 0, 6 * sizeof(HwVtx));
    v[0].x = x0; v[0].y = y0;
    v[1].x = x1; v[1].y = y0;
    v[2].x = x1; v[2].y = y1;
    v[3].x = x0; v[3].y = y0;
    v[4].x = x1; v[4].y = y1;
    v[5].x = x0; v[5].y = y1;
    for (i = 0; i < 6; i++) {
        v[i].r = v[i].g = v[i].b = k;
        v[i].a = 1.0f; v[i].q = 1.0f; v[i].c = c;
    }
    return 6;
}

void gl_draw_fps(void)
{
    enum { PX = 2, MAXD = 4 };
    static HwVtx v[6 + MAXD * 15 * 6];
    HwState q;
    char d[MAXD];
    NVU32 now = tb(), f;
    int nd = 0, n = 0, i, row, col;

    if (gShowFps < 0) {
        gShowFps = gl_boot_arg("nv34glshowfps");
        if (gShowFps)
            lg("pocitadlo fps zapnute (nv34glshowfps)");
    }
    if (!gShowFps || !C || !C->hasRT)
        return;
    gFpsFrames++;
    if (!gFpsTb || now - gFpsTb > 18432u * 2000) {          /* prvé volanie alebo dlhá pauza */
        gFpsTb = now;
        gFpsFrames = 0;
    } else if (now - gFpsTb >= 18432u * 500) {              /* obnova každú pol sekundu */
        gFpsShown = (NVU32)((double)gFpsFrames * 18432000.0 / (now - gFpsTb) + 0.5);
        gFpsTb = now;
        gFpsFrames = 0;
    }
    f = gFpsShown > 9999 ? 9999 : gFpsShown;
    do {
        d[nd++] = (char)(f % 10);
        f /= 10;
    } while (f && nd < MAXD);

    memset(&q, 0, sizeof(q));
    fill_state(&q, 0);
    q.tex = q.tex1 = 0;
    q.env = 0;
    q.blend = q.alphaTest = q.depthTest = q.depthWrite = q.fog = q.tnl = q.cull = 0;
    q.mv = 0;
    q.scissor = 0;
    q.colorMask = 0x01010101;
    /* pozadie: okraj 2 px okolo číslic */
    n += fps_quad(v + n, 2, 2, (float)(2 + 2 + nd * 4 * PX - PX + 2), (float)(2 + 2 + 5 * PX + 2), 0x000000FF);
    for (i = 0; i < nd; i++) {
        unsigned bits = gDigit[(int)d[nd - 1 - i]];
        float x = (float)(4 + i * 4 * PX);
        for (row = 0; row < 5; row++)
            for (col = 0; col < 3; col++)
                if (bits & (1u << (14 - row * 3 - col)))
                    n += fps_quad(v + n, x + col * PX, (float)(4 + row * PX),
                                  x + (col + 1) * PX, (float)(4 + (row + 1) * PX), 0xFFFFFFFF);
    }
    if (hw_state(&C->rt, &q) >= 0)
        hw_draw(5, v, n, 0);
}

void glHint(GLenum target, GLenum mode) { (void)target; (void)mode; }
void glDrawBuffer(GLenum mode)
{
    ONCE(0x5C200000 | (mode & 0xffff), "glDrawBuffer %x (ignorovane)", mode);
    gFrDrawBuf = mode;
}
void glReadBuffer(GLenum mode) { ONCE(0x5C300000 | (mode & 0xffff), "glReadBuffer %x (ignorovane)", mode); }
void glPointSize(GLfloat s) { (void)s; }
void glLineWidth(GLfloat w) { (void)w; }

void glPolygonOffset(GLfloat f, GLfloat u)
{
    CHECK();
    C->s.polyFactor = f;
    C->s.polyUnits = u;
}

/* hl-9: GL polygon offset na CPU — Δz = m·factor + r·units, m = max |∂z/∂x|, |∂z/∂y|
 * (okno), r = najmenší rozlíšiteľný krok; decaly Half-Life (-1, -2) inak blikali */
#define POLY_R (1.0f / 1048576.0f)

static float poly_offset(const HwVtx *a, const HwVtx *b, const HwVtx *c)
{
    GLS *s = &C->s;
    float x1 = b->x - a->x, y1 = b->y - a->y, z1 = b->z - a->z;
    float x2 = c->x - a->x, y2 = c->y - a->y, z2 = c->z - a->z;
    float d = x1 * y2 - x2 * y1, m = 0, dzdx, dzdy;
    if (d != 0) {
        dzdx = (z1 * y2 - z2 * y1) / d;
        dzdy = (x1 * z2 - x2 * z1) / d;
        if (dzdx < 0) dzdx = -dzdx;
        if (dzdy < 0) dzdy = -dzdy;
        m = dzdx > dzdy ? dzdx : dzdy;
    }
    return m * s->polyFactor + (C->rt.z16 ? 1.0f / 32768.0f : POLY_R) * s->polyUnits;
}

static void apply_offset(HwVtx *v, int n, float off)
{
    int i;
    for (i = 0; i < n; i++) {
        float z = v[i].z + off;
        v[i].z = z < 0 ? 0 : z;
    }
}

void glPixelStorei(GLenum pname, GLint v)
{
    CHECK();
    switch (pname) {
    case 0x0CF5: C->unpackAlign = v; break;       /* GL_UNPACK_ALIGNMENT */
    case 0x0CF2: C->unpackRowLength = v; break;   /* GL_UNPACK_ROW_LENGTH */
    case 0x0D05: C->packAlign = v; break;         /* GL_PACK_ALIGNMENT */
    default:
        if (v)
            ONCE(0x59200000 | (pname & 0xffff), "glPixelStore %x = %d ignorovane", pname, v);
    }
}

void glPixelStoref(GLenum pname, GLfloat v) { glPixelStorei(pname, (GLint)v); }

/* hmla: len stav; GPU ju robí cez final combiner (hw_state) */
static void fog_param(GLenum p, const GLfloat *v)
{
    GLS *s;
    CHECK();
    s = &C->s;
    ONCE(0x58110000 | (p & 0xffff), "glFog %x = %f (prvykrat)", p, (double)v[0]);
    switch (p) {
    case GL_FOG_MODE:
        if (s->fogMode != (GLenum)v[0]) {
            FLUSH();
            s->fogMode = (GLenum)v[0];
        }
        break;
    case GL_FOG_DENSITY:
        if (s->fogDensity != v[0]) {
            FLUSH();
            s->fogDensity = v[0];
        }
        break;
    case GL_FOG_START:
        if (s->fogStart != v[0]) {
            FLUSH();
            s->fogStart = v[0];
        }
        break;
    case GL_FOG_END:
        if (s->fogEnd != v[0]) {
            FLUSH();
            s->fogEnd = v[0];
        }
        break;
    case GL_FOG_COLOR:
        if (s->fogColor[0] != v[0] || s->fogColor[1] != v[1] || s->fogColor[2] != v[2] ||
            s->fogColor[3] != v[3]) {
            FLUSH();
            memcpy(s->fogColor, v, sizeof(s->fogColor));
        }
        break;
    }
}

void glFogf(GLenum p, GLfloat v) { fog_param(p, &v); }
void glFogi(GLenum p, GLint v) { GLfloat f = (GLfloat)v; fog_param(p, &f); }
void glFogfv(GLenum p, const GLfloat *v) { if (v) fog_param(p, v); }
void glFogiv(GLenum p, const GLint *v)
{
    GLfloat f[4];
    int i;
    if (!v)
        return;
    if (p == GL_FOG_COLOR) {                        /* celé čísla → −1…1 */
        for (i = 0; i < 4; i++)
            f[i] = (GLfloat)((double)v[i] / 2147483647.0);
    } else {
        f[0] = (GLfloat)v[0];
    }
    fog_param(p, f);
}

void glPushAttrib(GLbitfield mask)
{
    CHECK();
    (void)mask;
    if (C->attrSp >= 8) {
        set_err(GL_STACK_OVERFLOW);
        return;
    }
    C->attrStack[C->attrSp++] = C->s;
}

void glPopAttrib(void)
{
    CHECK();
    if (!C->attrSp) {
        set_err(GL_STACK_UNDERFLOW);
        return;
    }
    FLUSH();
    C->s = C->attrStack[--C->attrSp];
}

void glFlush(void)
{
    CHECK();
    gFrFlush++;
    FLUSH();
    hw_kick();
}

void glFinish(void)
{
    CHECK();
    gFrFinish++;
    if (gDbg)
        lg("glFinish");
    FLUSH();
    hw_finish();
}

GLenum glGetError(void)
{
    GLenum e;
    if (!C)
        return GL_NO_ERROR;
    e = C->err;
    C->err = GL_NO_ERROR;
    return e;
}

const GLubyte *glGetString(GLenum name)
{
    ONCE(0x5C000000 | (name & 0xffff), "glGetString %x", name);    /* wolf-6: čo hra zisťuje */
    switch (name) {
    case GL_VENDOR:     return (const GLubyte *)"NVIDIA Corporation (NV34 MiniGL)";
    case GL_RENDERER:   return (const GLubyte *)"NV34 MiniGL GeForce FX Go5200";
    case GL_VERSION:    return (const GLubyte *)"1.1 NV34 MiniGL 1";
    case GL_EXTENSIONS: return (const GLubyte *)(gMT ? "GL_ARB_multitexture GL_EXT_compiled_vertex_array" :
                                                         "GL_EXT_compiled_vertex_array");
    }
    return 0;
}

/* ---- glGet ----------------------------------------------------------------------- */

static int get_values(GLenum p, double *o)
{
    GLS *s = &C->s;
    int i;
    switch (p) {
    case GL_MODELVIEW_MATRIX:
    case GL_PROJECTION_MATRIX:
    case GL_TEXTURE_MATRIX: {
        int k = p == GL_MODELVIEW_MATRIX ? 0 : p == GL_PROJECTION_MATRIX ? 1 : 2;
        for (i = 0; i < 16; i++)
            o[i] = C->mstack[k][C->msp[k]][i];
        return 16;
    }
    case GL_VIEWPORT:      for (i = 0; i < 4; i++) o[i] = s->viewport[i]; return 4;
    case 0x0C10:           for (i = 0; i < 4; i++) o[i] = s->scissor[i]; return 4;   /* SCISSOR_BOX */
    case 0x0C22:           for (i = 0; i < 4; i++) o[i] = s->clearColor[i]; return 4;
    case 0x0B00:           for (i = 0; i < 4; i++) o[i] = C->cur[i]; return 4;        /* CURRENT_COLOR */
    case 0x0B03:           for (i = 0; i < 4; i++) o[i] = C->curTex[i]; return 4;
    case 0x0C23:           for (i = 0; i < 4; i++) o[i] = s->cmask[i]; return 4;      /* COLOR_WRITEMASK */
    case 0x0B70:           o[0] = s->depthNear; o[1] = s->depthFar; return 2;
    case 0x0D3A:           o[0] = o[1] = 2048; return 2;                               /* MAX_VIEWPORT_DIMS */
    case GL_MAX_TEXTURE_SIZE: o[0] = 2048; return 1;
    case GL_MAX_MODELVIEW_STACK_DEPTH: case 0x0D38: case 0x0D39: o[0] = STACK_DEPTH; return 1;
    case 0x0D35:           o[0] = 8; return 1;                                         /* MAX_ATTRIB_STACK_DEPTH */
    case 0x84E2:                                                                              /* MAX_TEXTURE_UNITS_ARB */
        ONCE(0x5C100000, "glGet MAX_TEXTURE_UNITS_ARB = %d", gMT ? 2 : 1);
        o[0] = gMT ? 2 : 1;
        return 1;
    case 0x84E0:           o[0] = 0x84C0 + C->unit; return 1;                          /* ACTIVE_TEXTURE_ARB */
    case 0x84E1:           o[0] = 0x84C0 + C->cunit; return 1;                         /* CLIENT_ACTIVE_TEXTURE_ARB */
    case GL_DEPTH_BITS:    o[0] = C->depthBits ? (C->rt.z16 ? 16 : 24) : 0; return 1;
    case GL_RED_BITS: case 0x0D53: case 0x0D54: o[0] = C->rt.bpp == 16 ? 5 : 8; return 1;
    case GL_ALPHA_BITS:    o[0] = C->rt.bpp == 16 ? 0 : 8; return 1;
    case GL_STENCIL_BITS:  o[0] = 0; return 1;
    case GL_MATRIX_MODE:   o[0] = C->matrixMode; return 1;
    case 0x0BA3:           o[0] = C->msp[0] + 1; return 1;                             /* MODELVIEW_STACK_DEPTH */
    case 0x0BA4:           o[0] = C->msp[1] + 1; return 1;
    case 0x0BA5:           o[0] = C->msp[2] + 1; return 1;
    case GL_TEXTURE_BINDING_2D: o[0] = C->bound ? C->bound->name : 0; return 1;
    case 0x0B74:           o[0] = s->depthFunc; return 1;
    case 0x0B72:           o[0] = s->depthMask; return 1;
    case 0x0B73:           o[0] = s->clearDepth; return 1;
    case 0x0BE1:           o[0] = s->blendSrc; return 1;
    case 0x0BE0:           o[0] = s->blendDst; return 1;
    case 0x0BC1:           o[0] = s->alphaFunc; return 1;
    case 0x0BC2:           o[0] = s->alphaRef; return 1;
    case 0x0B45:           o[0] = s->cullFace; return 1;
    case 0x0B46:           o[0] = s->frontFace; return 1;
    case 0x0B54:           o[0] = s->shade; return 1;
    case 0x0CF5:           o[0] = C->unpackAlign; return 1;
    case 0x0D05:           o[0] = C->packAlign; return 1;
    case 0x0C32:           o[0] = C->dbl; return 1;                                    /* DOUBLEBUFFER */
    case 0x0D50:           o[0] = 4; return 1;                                         /* SUBPIXEL_BITS */
    }
    {
        int *cp = cap_ptr(p);
        Arr *a = arr_of(p);
        if (cp) { o[0] = *cp; return 1; }
        if (a) { o[0] = a->on; return 1; }
    }
    ONCE(0x5A000000 | (p & 0xffff), "glGet %x nepodporovane", p);
    o[0] = 0;
    return 1;
}

void glGetDoublev(GLenum p, GLdouble *v)
{
    double o[16];
    int n, i;
    CHECK();
    n = get_values(p, o);
    for (i = 0; i < n; i++)
        v[i] = o[i];
}

/* wolf-7: konštanty aj bez aktuálneho kontextu (Wolfenstein ukázal GL_MAX_ACTIVE_TEXTURES_ARB 0,
 * hoci glGet 0x84E2 do get_values neprišiel) */
static int get_noctx(GLenum p, double *o)
{
    ONCE(0x5D000000 | (p & 0xffff), "glGet %x bez kontextu", p);
    o[0] = p == 0x84E2 ? (gMT ? 2 : 1) : p == GL_MAX_TEXTURE_SIZE ? 2048 : 0;
    return 1;
}

static int get_any(GLenum p, double *o)
{
    if (!C)
        return get_noctx(p, o);
    ONCE(0x5E000000 | (p & 0xffff), "glGet %x (prvykrat)", p);
    return get_values(p, o);
}

void glGetFloatv(GLenum p, GLfloat *v)
{
    double o[16];
    int n, i;
    GLT;
    n = get_any(p, o);
    for (i = 0; i < n; i++)
        v[i] = (GLfloat)o[i];
}

void glGetIntegerv(GLenum p, GLint *v)
{
    double o[16];
    int n, i;
    GLT;
    n = get_any(p, o);
    for (i = 0; i < n; i++)
        v[i] = (GLint)(o[i] + (o[i] < 0 ? -0.5 : 0.5));
}

void glGetBooleanv(GLenum p, GLboolean *v)
{
    double o[16];
    int n, i;
    GLT;
    n = get_any(p, o);
    for (i = 0; i < n; i++)
        v[i] = o[i] != 0;
}

/* ---- čítanie pixelov (snímky obrazovky) ----------------------------------------- */

void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *pixels)
{
    if (gDbg)
        lg("glReadPixels %d %d %dx%d", x, y, w, h);
    ONCE(0x5C400000, "glReadPixels %d %d %dx%d (prvykrat)", x, y, w, h);
    gFrRead++;
    int comps = comps_of(format), i, j;
    long al, stride;
    GLubyte *row;

    CHECK();
    if (!C->hasRT || type != GL_UNSIGNED_BYTE || (format != GL_RGB && format != GL_RGBA && format != GL_BGRA_EXT)) {
        ONCE(0x5B000000 | (format & 0xffff), "glReadPixels format %x typ %x nepodporovane", format, type);
        return;
    }
    FLUSH();
    hw_sync();
    al = C->packAlign > 0 ? C->packAlign : 1;
    stride = ((long)w * comps + al - 1) / al * al;
    for (j = 0; j < h; j++) {
        int ry = C->rt.h - 1 - (y + j);
        volatile NVU8 *src;
        row = (GLubyte *)pixels + j * stride;
        if (ry < 0 || ry >= C->rt.h)
            continue;
        src = S->a->vram + C->rt.colorOff + (long)ry * C->rt.colorPitch;
        for (i = 0; i < w; i++) {
            int rx = x + i;
            NVU32 r = 0, g = 0, b = 0;
            if (rx >= 0 && rx < C->rt.w) {
                if (C->rt.bpp == 16) {
                    NVU32 p = ((volatile unsigned short *)src)[rx];
                    r = ((p >> 10) & 31) * 255 / 31;
                    g = ((p >> 5) & 31) * 255 / 31;
                    b = (p & 31) * 255 / 31;
                } else {
                    NVU32 p = ((volatile NVU32 *)src)[rx];
                    r = (p >> 16) & 255;
                    g = (p >> 8) & 255;
                    b = p & 255;
                }
            }
            if (format == GL_BGRA_EXT) {
                row[0] = b; row[1] = g; row[2] = r; row[3] = 255;
            } else {
                row[0] = r; row[1] = g; row[2] = b;
                if (comps == 4)
                    row[3] = 255;
            }
            row += comps;
        }
    }
}

/* ---- kontext ------------------------------------------------------------------- */

void gl_ctx_init(GLCtx *c)
{
    int i;
    GLS *s = &c->s;

    for (i = 0; i < 3; i++)
        mat_ident(c->mstack[i][0]);
    c->matrixMode = GL_MODELVIEW;
    c->mvpDirty = 1;
    c->texIdentity = 1;
    c->cur[0] = c->cur[1] = c->cur[2] = c->cur[3] = 1.0f;
    c->curTex[3] = 1.0f;
    c->curTex1[3] = 1.0f;
    env_init(&c->env[0]);
    env_init(&c->env[1]);
    tex_init(&c->def, 0);
    c->bound = &c->def;
    c->unpackAlign = c->packAlign = 4;
    s->blendSrc = GL_ONE;
    s->blendDst = GL_ZERO;
    s->alphaFunc = GL_ALWAYS;
    s->depthFunc = GL_LESS;
    s->depthMask = 1;
    s->cullFace = GL_BACK;
    s->frontFace = GL_CCW;
    s->shade = GL_SMOOTH;
    s->texEnv = GL_MODULATE;
    s->cmask[0] = s->cmask[1] = s->cmask[2] = s->cmask[3] = 1;
    s->depthNear = 0;
    s->depthFar = 1;
    s->clearDepth = 1;
    s->dither = 1;
    s->polyMode = GL_FILL;
    s->fogMode = GL_EXP;
    s->fogDensity = 1;
    s->fogEnd = 1;
}

void gl_release_textures(void)
{
    int i;
    TexObj *t;
    for (i = 0; i < TEX_HASH; i++)
        for (t = gTex[i]; t; t = t->next)
            if (t->hw) {
                hw_tex_free(t->hw);
                t->hw = 0;
            }
}

/* štatistika do logu: prvé 3 snímky a potom priemer každých 250 (opt-2: časy v ms) */
static NVU32 gIvTb, gIvTicks;
static long gIvFrames;

static unsigned long ms100(NVU32 t, NVU32 perMs, long frames)
{
    return perMs && frames ? (unsigned long)((double)t * 100.0 / perMs / frames) : 0;
}

/* q3-13/14: skutočný takt CPU — rozdiel slučiek s 12 a 4 závislými sčítaniami = 8 taktov na
 * iteráciu (réžia slučky sa odčíta) */
static NVU32 loop_tb(int twelve)
{
    NVU32 t0, t1, x = 1;
    long n = 1000000;
    t0 = tb();
    if (twelve)
        __asm__ volatile("mtctr %1\n"
                         "1: add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n"
                         " add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n"
                         " add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n bdnz 1b"
                         : "+r"(x) : "r"(n) : "ctr");
    else
        __asm__ volatile("mtctr %1\n"
                         "1: add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n bdnz 1b"
                         : "+r"(x) : "r"(n) : "ctr");
    t1 = tb();
    return t1 - t0;
}

static NVU32 cpu_mhz(NVU32 perMs)
{
    NVU32 a = loop_tb(0), b = loop_tb(1);
    return b > a ? (NVU32)(8000.0 * perMs / (b - a)) : 0;
}

/* q3-15: pre agl.c (pred prvou snímkou: časová základňa 18,432 MHz z device tree) */
NVU32 gl_cpu_mhz(void)
{
    return cpu_mhz(18432);
}

/* vlastnosti uzla procesora v Name Registry (takt, znížený takt, power step) */
static void cpu_props(void)
{
    static const char *paths[] = { "Devices:device-tree:cpus:PowerPC,G4@0", "Devices:device-tree:cpus:PowerPC,G4",
                                   "Devices:device-tree:cpus:PowerPC,G4@1", 0 };
    static const char *props[] = { "clock-frequency", "max-clock-frequency", "reduced-clock-frequency",
                                   "min-clock-frequency", "dynamic-power-step", "bus-frequency",
                                   "timebase-frequency", "cpu-version", "frequency-gpio", "voltage-gpio", 0 };
    RegEntryID e;
    int i, j;
    long pclk = 0;
    Gestalt('pclk', &pclk);
    lg("Gestalt pclk %u Hz", (NVU32)pclk);
    for (i = 0; paths[i]; i++) {
        if (RegistryCStrEntryLookup(0, paths[i], &e) != 0)
            continue;
        for (j = 0; props[j]; j++) {
            NVU32 v[4] = { 0, 0, 0, 0 };
            UInt32 size = sizeof(v);
            if (RegistryPropertyGet(&e, props[j], v, &size) == 0)
                lg("%s %s = %x (%u), %u B", paths[i] + 26, props[j], v[0], v[0], (NVU32)size);
        }
        RegistryEntryIDDispose(&e);
        return;
    }
    lg("uzol CPU v Name Registry nenajdeny");
}

static void frame_trace(void)
{
    NVU32 tris = gStatTris >= gFrTris0 ? gStatTris - gFrTris0 : gStatTris, sig;
    sig = (tris == 0 ? 0 : tris < 100 ? 1 : tris < 1000 ? 2 : 3) | (gFrClears > 7 ? 7 : gFrClears) << 2 |
          (gFrClearMask & 0x4100 ? 1 : 0) << 5 | (gFrClearMask & 0x0100 ? 1 : 0) << 6 | (gFrFlush ? 1 : 0) << 7 |
          (gFrFinish ? 1 : 0) << 8 | (gFrRead ? 1 : 0) << 9 | (gFrDrawBuf & 0xfff) << 10 |
          ((gFrClearArgb ^ (gFrClearArgb >> 8) ^ (gFrClearArgb >> 16)) & 0xff) << 22;
    /* g1-f4d: bez ladenia len prvá snímka — podpis obsahuje farbu glClear, ktorá sa v Gothicu
     * mení s oblohou, a každý riadok lg() je otvorenie + zápis + FlushVol (prepad snímky) */
    if (sig != gTrSig && gTrN < (gDbg ? 600 : 1)) {
        gTrN++;
        lg("priebeh snimka %d: %u troj., glClear %u× maska %x farba %x, glFlush %u, glFinish %u, glReadPixels %u, "
           "glDrawBuffer %x, viewport %d %d %d %d, hlbka %d, blend %d",
           C->frames, tris, gFrClears, gFrClearMask, gFrClearArgb, gFrFlush, gFrFinish, gFrRead,
           gFrDrawBuf, C->s.viewport[0], C->s.viewport[1], C->s.viewport[2], C->s.viewport[3], C->s.depthTest,
           C->s.blend);
    }
    gTrSig = sig;
    {
        static int nCut;
        static NVU32 lastTb;
        NVU32 now = tb();
        static NVU32 lastGL;
        static NVU32 avg;                           /* g1-f4d: kĺzavý priemer snímky (1/16) */
        NVU32 dt = now - lastTb, dgl = gTGL >= lastGL ? gTGL - lastGL : gTGL;
        int cut = gFrClears >= 4 && (gFrClearMask & 0x4000);
        /* max-14: cutscény a každá pomalá snímka (nad 45 ms) s rozpisom textúr; g1-f4d: pomalé len so
         * stats/ladením a len nad dvojnásobkom priemeru (G1 beží ~50 ms na snímku — zápis do logu
         * po každej snímke robil ďalšie prepady) */
        int slow = dt > 45 * 18432 && (gStatsOn || gDbg) && (!avg || dt > 2 * avg);
        if (lastTb)
            avg = avg ? avg - avg / 16 + dt / 16 : dt;
        if (lastTb && (cut || slow) && nCut < 400) {
            nCut++;
            lg("%s snimka %d: %u ms (MiniGL %u ms; glTexImage %u× %u KB %u ms; VRAM zapis %u× %u KB %u ms, uvolnenie %u ms), "
               "%u troj., prekrytia %u× alfa %x",
               cut ? "cutscena" : "pomala", C->frames, dt / 18432, dgl / 18432, gFrTexImgN, gFrTexImgB >> 10,
               gTConv / 18432, gFrUplN, gFrUplB >> 10, gTUpl / 18432, gTEvict / 18432, tris, gFrOvlN, gFrOvlMax);
        }
        lastTb = now;
        lastGL = gTGL;
        gTConv = gFrTexImgN = gFrTexImgB = gTUpl = gTEvict = gFrUplN = gFrUplB = 0;
        gFrOvlN = gFrOvlMax = gFrOvlC = gFrErrN = gFrErr = 0;
    }
    if (gDump && --gDump == 0)
        lg("==== koniec vypisu");
    /* max-10: len cutscény — farebný glClear aspoň 4× za snímku (letterbox), nie portály v hre */
    if (!gDump && gDumpN < 0 && C->frames > 100 && gFrClears >= 4 && (gFrClearMask & 0x4000) &&
        C->frames - gDumpLast >= (gDumpN < 5 ? 8 : 60)) {
        gDumpN++;
        gDumpLast = C->frames;
        gDump = 1;
        lg("==== vypis snimky %d (predosla: %u troj., glClear %u×)", C->frames + 1, tris, gFrClears);
    }
    gFrVpOdd = 0;
    gFrClears = gFrClearMask = gFrFlush = gFrFinish = gFrRead = 0;
}

static void frame_stats_iv(void);

void gl_frame_stats(void)
{
    if (!C)
        return;
    frame_trace();
    frame_stats_iv();
    gFrTris0 = gStatTris;
}

static void frame_stats_iv(void)
{
    NVU32 now = tb(), ticks = TickCount();
    if (!C)
        return;
    C->frames++;
    gIvFrames++;
    if (gDbg && (C->frames <= 3 || (C->frames <= 2000 && C->frames % 100 == 0)))
        lg("snimka %d: %d trojuholnikov v %d davkach, VRAM volna %u KB", C->frames, gStatTris, gStatBatches,
           S ? S->vramFreeBytes() >> 10 : 0);
    if (C->frames <= 3 || !gIvTicks) {
        gIvTb = now;
        gIvTicks = ticks;
        gIvFrames = 0;
        gStatTris = gStatBatches = 0;
        gTEnd = gTFlush = gTWait = gTSwap = 0;
        gHwUploads = gHwSubUploads = gHwUploadBytes = gHwSyncs = 0;
        gTGL = 0;
        gTFinish = gFinishN = 0;
        return;
    }
    if (gStatsOn && gIvFrames >= 250 && ticks - gIvTicks >= 30) {
        NVU32 dt = now - gIvTb, dticks = ticks - gIvTicks;
        NVU32 perMs = (NVU32)((double)dt * 60.0 / dticks / 1000.0);   /* tb na ms */
        unsigned long fps10 = (unsigned long)(gIvFrames * 600.0 / dticks);
        static int mhzDone;
        if (!mhzDone) {
            mhzDone = 1;
            lg("CPU: ~%u MHz (meranie), ~%u MHz (druhe meranie)", cpu_mhz(perMs), cpu_mhz(perMs));
            cpu_props();
        }
        lg("snimky %d: %u.%u fps, ms/snimku: celkom %u.%02u, glEnd %u.%02u (z toho davky %u.%02u, cakanie GPU %u.%02u), swap %u.%02u; "
           "MiniGL spolu %u.%02u ms (glFinish %d× %u.%02u ms); rychla cesta: %d troj., poz. %d, atrib. %d; 2 textury %d troj., T&L %d troj., hmla %d troj., VRAM %d troj. / %d vrch. (cakanie %u); DrawElements %d/%d pomalych (%u.%02u ms); orezanych %d troj. (%u.%02u ms, pas %u.%02u); "
           "%d troj./snimku, %d davok; nahrati %u (+%u ciastocnych, %u KB), sync %u; texenv programov %u (cakania %u); tb %u/ms; "
           "limit fps: cakanie %u.%02u ms, oneskorenych %u, novy rozvrh %u",
           C->frames, fps10 / 10, fps10 % 10,
           ms100(dt, perMs, gIvFrames) / 100, ms100(dt, perMs, gIvFrames) % 100,
           ms100(gTEnd, perMs, gIvFrames) / 100, ms100(gTEnd, perMs, gIvFrames) % 100,
           ms100(gTFlush, perMs, gIvFrames) / 100, ms100(gTFlush, perMs, gIvFrames) % 100,
           ms100(gTWait, perMs, gIvFrames) / 100, ms100(gTWait, perMs, gIvFrames) % 100,
           ms100(gTSwap, perMs, gIvFrames) / 100, ms100(gTSwap, perMs, gIvFrames) % 100,
           ms100(gTGL + gTSwap, perMs, gIvFrames) / 100, ms100(gTGL + gTSwap, perMs, gIvFrames) % 100,
           (long)(gFinishN / gIvFrames), ms100(gTFinish, perMs, gIvFrames) / 100, ms100(gTFinish, perMs, gIvFrames) % 100,
           (long)(gNFastTri / gIvFrames), (long)(gNPos / gIvFrames), (long)(gNAttr / gIvFrames),
           (long)(gStatMT / gIvFrames), (long)(gStatTnl / gIvFrames), (long)(gStatFog / gIvFrames),
           (long)(gStatVB / gIvFrames),
           (long)(gStatVBv / gIvFrames), gHwVbWaits,
           gStatDE / gIvFrames, gStatDEslow / gIvFrames,
           ms100(gTVerts, perMs, gIvFrames) / 100, ms100(gTVerts, perMs, gIvFrames) % 100,
           gStatClip / gIvFrames, ms100(gTClip, perMs, gIvFrames) / 100, ms100(gTClip, perMs, gIvFrames) % 100,
           (NVU32)gGuard, (NVU32)(gGuard * 100) % 100,
           gStatTris / gIvFrames, gStatBatches / gIvFrames, gHwUploads, gHwSubUploads, gHwUploadBytes >> 10,
           gHwSyncs, gFpgNew, gFpgEvict, perMs,
           ms100(gTFps, perMs, gIvFrames) / 100, ms100(gTFps, perMs, gIvFrames) % 100, gFpsLate, gFpsReset);
        gTFps = gFpsLate = gFpsReset = 0;
        gIvTb = now;
        gIvTicks = ticks;
        gIvFrames = 0;
        gStatTris = gStatBatches = 0;
        gTEnd = gTFlush = gTWait = gTSwap = 0;
        gHwUploads = gHwSubUploads = gHwUploadBytes = gHwSyncs = 0;
        gStatDE = gStatDEslow = 0;
        gTVerts = 0;
        gStatClip = 0;
        gTClip = 0;
        gTGL = 0;
        gTFinish = gFinishN = 0;
        gNPos = gNAttr = gNFastTri = 0;
        gStatMT = gStatTnl = gStatFog = gStatVB = gStatVBv = 0;
        gHwVbWaits = 0;
    }
}
