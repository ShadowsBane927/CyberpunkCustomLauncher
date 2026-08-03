// Mod made by Realbiquitous
/* PHASE 3 - DirectWrite text, ported one element at a time. */
#define COBJMACROS
#define CINTERFACE
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <d2d1.h>
#include <dwrite.h>

/* ---- mirrors of main-source layout/state ----. */
typedef struct { int x, y, w, h; double scale; } SV2LayoutMirror;
extern SV2LayoutMirror SV2_GetLayout(int clientW, int clientH);
extern int SV2_MapX(SV2LayoutMirror L, double nx);
extern int SV2_MapY(SV2LayoutMirror L, double ny);
extern char SV2_g_quest[256];
extern int SE_g_saveLoaded;
extern char SV2_g_dateOnly[40];
extern char SV2_g_saveName[128];
extern char SV2_g_lifePath[64];
extern double SV2_TimeTop, SV2_TimeBottom;
extern double SV2_LedTop, SV2_LedBottom;
extern double SV2_LedScrollY;
extern char SV2_g_timeOnly[24];
extern char SV2_g_location[80];
extern const char *SV2_D2D_GetLevelNumText(void);
extern const char *SV2_D2D_GetCredNumText(void);
extern const char *SV2_D2D_GetGlassNumText(int glassIndex, double *outCx, double *outCy, BOOL *outHovered);
extern double SV2_StatNumFontPx;
#define SV2P3_GLASS_COUNT 18

#define SV2P3_MISSION_CX 153.0
#define SV2P3_MISSION_CY 245.0
#define SV2P3_MISSION_ANGLE (-4.0)
#define SV2P3_MISSION_FONTPX 34.0
#define SV2P3_MISSION_SHADOW_DX 8
#define SV2P3_MISSION_SHADOW_DY 8

/* DATE layout mirrors SV2_LayoutItems[SV2_LI_DATE]. */
#define SV2P3_DATE_CX 282.0
#define SV2P3_DATE_CY 516.0
#define SV2P3_DATE_FONTPX 32.0

/* TITLE mirrors SV2_LayoutItems[SV2_LI_TITLE]. */
#define SV2P3_TITLE_CX 255.0
#define SV2P3_TITLE_CY 69.0
#define SV2P3_TITLE_ANGLE 12.0
#define SV2P3_TITLE_FONTPX 26.0

/* LIFEPATH mirrors SV2_LayoutItems[SV2_LI_LIFEPATH]. */
#define SV2P3_LIFEPATH_CX 376.0
#define SV2P3_LIFEPATH_CY 100.0
#define SV2P3_LIFEPATH_ANGLE (-13.0)
#define SV2P3_LIFEPATH_FONTPX 105.0

/* TIME/LED mirror SV2_LayoutItems[SV2_LI_TIME]/[SV2_LI_LED]. */
#define SV2P3_TIME_CX 68.0
#define SV2P3_TIME_FONTPX 95.0
#define SV2P3_LED_CX 141.0
#define SV2P3_LED_FONTPX 18.0
#define SV2P3_LED_SPEED_PX_PER_SEC 60.0

/* LOCATION mirrors SV2_LayoutItems[SV2_LI_LOCATION]. */
#define SV2P3_LOCATION_CX 289.0
#define SV2P3_LOCATION_CY 827.0
#define SV2P3_LOCATION_FONTPX 44.0
/* "Westbrook - Charter Hill" (24 chars) renders at the base size above. */
#define SV2P3_LOCATION_BASE_LEN 24.0

/* LEVEL_NUM/CRED_NUM mirror SV2_LayoutItems[SV2_LI_LEVELNUM]/[SV2_LI_CREDNUM]. */
#define SV2P3_LEVELNUM_CX 116.0
#define SV2P3_LEVELNUM_CY 937.0
#define SV2P3_CREDNUM_CX 337.0
#define SV2P3_CREDNUM_CY 937.0
#define SV2P3_LEVELCREDNUM_FONTPX 138.0

/* ------------------------------------------------------------------ */
/* Embedded font data table. */
/* should be visible to DirectWrite. */
/* ------------------------------------------------------------------ */
#define SV2_MAX_EMBEDDED_FONTS 8
typedef struct { const void *data; UINT32 size; } SV2EmbeddedFontData;
static SV2EmbeddedFontData g_embeddedFonts[SV2_MAX_EMBEDDED_FONTS];
static UINT32 g_embeddedFontCount = 0;

static BOOL SV2_D2D_LoadEmbeddedFontResource(const char *resourceName) {
    extern void AppendStatus(const char *);
    if (g_embeddedFontCount >= SV2_MAX_EMBEDDED_FONTS) return FALSE;
    HMODULE hMod = GetModuleHandleA(NULL);
    HRSRC hRes = FindResourceA(hMod, resourceName, RT_RCDATA);
    if (!hRes) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[d2d-text] font resource '%s' not found", resourceName);
        AppendStatus(msg);
        return FALSE;
    }
    HGLOBAL hData = LoadResource(hMod, hRes);
    DWORD size = SizeofResource(hMod, hRes);
    void *pData = LockResource(hData);
    if (!pData || !size) return FALSE;

    g_embeddedFonts[g_embeddedFontCount].data = pData;
    g_embeddedFonts[g_embeddedFontCount].size = (UINT32)size;
    g_embeddedFontCount++;

    char msg[128];
    snprintf(msg, sizeof(msg), "[d2d-text] queued embedded font '%s' (%u bytes) for custom collection", resourceName, (unsigned)size);
    AppendStatus(msg);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* MemFontFileStream. */
/* is a resource loaded via LockResource, which stays valid for the. */
/* lifetime of the process, so no copying is needed. */
/* ------------------------------------------------------------------ */
typedef struct {
    IDWriteFontFileStream base;
    LONG refCount;
    const void *data;
    UINT64 size;
} MemFontFileStream;

static HRESULT STDMETHODCALLTYPE MFS_QueryInterface(IDWriteFontFileStream *This, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDWriteFontFileStream)) {
        *ppv = This;
        IDWriteFontFileStream_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE MFS_AddRef(IDWriteFontFileStream *This) {
    MemFontFileStream *self = (MemFontFileStream *)This;
    return (ULONG)InterlockedIncrement(&self->refCount);
}
static ULONG STDMETHODCALLTYPE MFS_Release(IDWriteFontFileStream *This) {
    MemFontFileStream *self = (MemFontFileStream *)This;
    LONG rc = InterlockedDecrement(&self->refCount);
    if (rc == 0) HeapFree(GetProcessHeap(), 0, self);
    return (ULONG)rc;
}
static HRESULT STDMETHODCALLTYPE MFS_ReadFileFragment(IDWriteFontFileStream *This,
        const void **fragment_start, UINT64 offset, UINT64 fragment_size, void **fragment_context) {
    MemFontFileStream *self = (MemFontFileStream *)This;
    if (offset + fragment_size > self->size) return E_FAIL;
    *fragment_start = (const BYTE *)self->data + offset;
    *fragment_context = NULL;
    return S_OK;
}
static void STDMETHODCALLTYPE MFS_ReleaseFileFragment(IDWriteFontFileStream *This, void *fragment_context) {
    (void)This; (void)fragment_context;
}
static HRESULT STDMETHODCALLTYPE MFS_GetFileSize(IDWriteFontFileStream *This, UINT64 *size) {
    MemFontFileStream *self = (MemFontFileStream *)This;
    *size = self->size;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MFS_GetLastWriteTime(IDWriteFontFileStream *This, UINT64 *last_writetime) {
    (void)This;
    *last_writetime = 0;
    return S_OK;
}
static IDWriteFontFileStreamVtbl g_streamVtbl = {
    MFS_QueryInterface, MFS_AddRef, MFS_Release,
    MFS_ReadFileFragment, MFS_ReleaseFileFragment, MFS_GetFileSize, MFS_GetLastWriteTime
};

/* ------------------------------------------------------------------ */
/* MemFontFileLoader. */
/* round-trips to us is a UINT32 index into g_embeddedFonts. */
/* ------------------------------------------------------------------ */
typedef struct { IDWriteFontFileLoader base; } MemFontFileLoader;

static HRESULT STDMETHODCALLTYPE MFL_QueryInterface(IDWriteFontFileLoader *This, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDWriteFontFileLoader)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE MFL_AddRef(IDWriteFontFileLoader *This) { (void)This; return 1; }
static ULONG STDMETHODCALLTYPE MFL_Release(IDWriteFontFileLoader *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE MFL_CreateStreamFromKey(IDWriteFontFileLoader *This,
        const void *key, UINT32 key_size, IDWriteFontFileStream **stream) {
    (void)This;
    if (key_size != sizeof(UINT32)) return E_INVALIDARG;
    UINT32 idx = *(const UINT32 *)key;
    if (idx >= g_embeddedFontCount) return E_INVALIDARG;

    MemFontFileStream *s = (MemFontFileStream *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MemFontFileStream));
    if (!s) return E_OUTOFMEMORY;
    s->base.lpVtbl = &g_streamVtbl;
    s->refCount = 1;
    s->data = g_embeddedFonts[idx].data;
    s->size = g_embeddedFonts[idx].size;
    *stream = (IDWriteFontFileStream *)s;
    return S_OK;
}
static IDWriteFontFileLoaderVtbl g_loaderVtbl = { MFL_QueryInterface, MFL_AddRef, MFL_Release, MFL_CreateStreamFromKey };
static MemFontFileLoader g_fontFileLoader = { { &g_loaderVtbl } };

/* ------------------------------------------------------------------ */
/* MemFontFileEnumerator. */
/* ------------------------------------------------------------------ */
typedef struct {
    IDWriteFontFileEnumerator base;
    LONG refCount;
    IDWriteFactory *factory;
    UINT32 currentIndex; /* starts "before first": UINT32_MAX so first MoveNext lands on 0. */
} MemFontFileEnumerator;

static HRESULT STDMETHODCALLTYPE MFE_QueryInterface(IDWriteFontFileEnumerator *This, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDWriteFontFileEnumerator)) {
        *ppv = This;
        IDWriteFontFileEnumerator_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE MFE_AddRef(IDWriteFontFileEnumerator *This) {
    MemFontFileEnumerator *self = (MemFontFileEnumerator *)This;
    return (ULONG)InterlockedIncrement(&self->refCount);
}
static ULONG STDMETHODCALLTYPE MFE_Release(IDWriteFontFileEnumerator *This) {
    MemFontFileEnumerator *self = (MemFontFileEnumerator *)This;
    LONG rc = InterlockedDecrement(&self->refCount);
    if (rc == 0) {
        if (self->factory) IDWriteFactory_Release(self->factory);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return (ULONG)rc;
}
static HRESULT STDMETHODCALLTYPE MFE_MoveNext(IDWriteFontFileEnumerator *This, WINBOOL *has_current_file) {
    MemFontFileEnumerator *self = (MemFontFileEnumerator *)This;
    if (self->currentIndex == 0xFFFFFFFFu) self->currentIndex = 0;
    else self->currentIndex++;
    *has_current_file = (self->currentIndex < g_embeddedFontCount);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MFE_GetCurrentFontFile(IDWriteFontFileEnumerator *This, IDWriteFontFile **font_file) {
    MemFontFileEnumerator *self = (MemFontFileEnumerator *)This;
    if (self->currentIndex >= g_embeddedFontCount) return E_FAIL;
    UINT32 idx = self->currentIndex;
    return IDWriteFactory_CreateCustomFontFileReference(self->factory, &idx, sizeof(idx),
        (IDWriteFontFileLoader *)&g_fontFileLoader, font_file);
}
static IDWriteFontFileEnumeratorVtbl g_enumVtbl = { MFE_QueryInterface, MFE_AddRef, MFE_Release, MFE_MoveNext, MFE_GetCurrentFontFile };

/* ------------------------------------------------------------------ */
/* MemFontCollectionLoader. */
/* ------------------------------------------------------------------ */
typedef struct { IDWriteFontCollectionLoader base; } MemFontCollectionLoader;

static HRESULT STDMETHODCALLTYPE MCL_QueryInterface(IDWriteFontCollectionLoader *This, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDWriteFontCollectionLoader)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE MCL_AddRef(IDWriteFontCollectionLoader *This) { (void)This; return 1; }
static ULONG STDMETHODCALLTYPE MCL_Release(IDWriteFontCollectionLoader *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE MCL_CreateEnumeratorFromKey(IDWriteFontCollectionLoader *This,
        IDWriteFactory *factory, const void *key, UINT32 key_size, IDWriteFontFileEnumerator **enumerator) {
    (void)This; (void)key; (void)key_size;
    MemFontFileEnumerator *e = (MemFontFileEnumerator *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MemFontFileEnumerator));
    if (!e) return E_OUTOFMEMORY;
    e->base.lpVtbl = &g_enumVtbl;
    e->refCount = 1;
    e->factory = factory;
    IDWriteFactory_AddRef(factory);
    e->currentIndex = 0xFFFFFFFFu;
    *enumerator = (IDWriteFontFileEnumerator *)e;
    return S_OK;
}
static IDWriteFontCollectionLoaderVtbl g_collectionLoaderVtbl = { MCL_QueryInterface, MCL_AddRef, MCL_Release, MCL_CreateEnumeratorFromKey };
static MemFontCollectionLoader g_fontCollectionLoader = { { &g_collectionLoaderVtbl } };

/* ------------------------------------------------------------------ */
/* Top-level init / draw. */
/* ------------------------------------------------------------------ */
static IDWriteFactory *g_dwriteFactory = NULL;
static IDWriteFontCollection *g_customFontCollection = NULL;
static IDWriteTextFormat *g_missionFormat = NULL;
static float g_missionFormatSizePx = -1.0f;
static ID2D1SolidColorBrush *g_brushBlack = NULL;
static ID2D1SolidColorBrush *g_brushRed = NULL;
static ID2D1SolidColorBrush *g_brushWhite = NULL;
static ID2D1SolidColorBrush *g_brushStatDark = NULL; /* RGB(20,20,20) - unhovered stat number color. */
static ID2D1RenderTarget *g_brushOwnerRT = NULL;

static BOOL SV2_D2D_Text_Init(void) {
    extern void AppendStatus(const char *);
    if (g_missionFormat) return TRUE;

    if (!g_dwriteFactory) {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&g_dwriteFactory);
        if (FAILED(hr) || !g_dwriteFactory) {
            char msg[100];
            snprintf(msg, sizeof(msg), "[d2d-text] DWriteCreateFactory failed, hr=0x%08lX", (unsigned long)hr);
            AppendStatus(msg);
            return FALSE;
        }
        HRESULT hr2 = IDWriteFactory_RegisterFontFileLoader(g_dwriteFactory, (IDWriteFontFileLoader *)&g_fontFileLoader);
        HRESULT hr3 = IDWriteFactory_RegisterFontCollectionLoader(g_dwriteFactory, (IDWriteFontCollectionLoader *)&g_fontCollectionLoader);
        if (FAILED(hr2) || FAILED(hr3)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "[d2d-text] loader registration failed, hr2=0x%08lX hr3=0x%08lX", (unsigned long)hr2, (unsigned long)hr3);
            AppendStatus(msg);
            return FALSE;
        }
    }

    if (!g_customFontCollection) {
        if (g_embeddedFontCount == 0) {
            SV2_D2D_LoadEmbeddedFontResource("SV2_FONT_BOMBERTV");
            SV2_D2D_LoadEmbeddedFontResource("SV2_FONT_ENGRAVERS");
            SV2_D2D_LoadEmbeddedFontResource("SV2_FONT_REDMENACE");
            SV2_D2D_LoadEmbeddedFontResource("SV2_FONT_DIGITAL7");
            SV2_D2D_LoadEmbeddedFontResource("SV2_FONT_BROADWAY");
        }
        UINT32 dummyKey = 0;
        HRESULT hr = IDWriteFactory_CreateCustomFontCollection(g_dwriteFactory,
            (IDWriteFontCollectionLoader *)&g_fontCollectionLoader, &dummyKey, sizeof(dummyKey), &g_customFontCollection);
        if (FAILED(hr) || !g_customFontCollection) {
            char msg[100];
            snprintf(msg, sizeof(msg), "[d2d-text] CreateCustomFontCollection failed, hr=0x%08lX", (unsigned long)hr);
            AppendStatus(msg);
            return FALSE;
        }

        /* Log what actually landed in the collection so a mismatch shows up in debug_log.txt. */
        UINT32 famCount = IDWriteFontCollection_GetFontFamilyCount(g_customFontCollection);
        char msg[100];
        snprintf(msg, sizeof(msg), "[d2d-text] custom collection created with %u font famil%s", (unsigned)famCount, famCount == 1 ? "y" : "ies");
        AppendStatus(msg);
        for (UINT32 i = 0; i < famCount; i++) {
            IDWriteFontFamily *fam = NULL;
            if (SUCCEEDED(IDWriteFontCollection_GetFontFamily(g_customFontCollection, i, &fam)) && fam) {
                IDWriteLocalizedStrings *names = NULL;
                if (SUCCEEDED(IDWriteFontFamily_GetFamilyNames(fam, &names)) && names) {
                    UINT32 len = 0;
                    if (SUCCEEDED(IDWriteLocalizedStrings_GetStringLength(names, 0, &len))) {
                        wchar_t buf[128];
                        if (len < 128 && SUCCEEDED(IDWriteLocalizedStrings_GetString(names, 0, buf, 128))) {
                            char narrow[128];
                            WideCharToMultiByte(CP_UTF8, 0, buf, -1, narrow, sizeof(narrow), NULL, NULL);
                            char m2[160];
                            snprintf(m2, sizeof(m2), "[d2d-text]   family[%u] = '%s'", (unsigned)i, narrow);
                            AppendStatus(m2);
                        }
                    }
                    IDWriteLocalizedStrings_Release(names);
                }
                IDWriteFontFamily_Release(fam);
            }
        }
    }

    AppendStatus("[d2d-text] Phase 3 DirectWrite factory + custom font collection ready");
    return TRUE;
}

static IDWriteTextFormat *SV2_D2D_CreateFormat(const wchar_t *family, DWRITE_FONT_STYLE style, float sizePx, const char *logTag) {
    extern void AppendStatus(const char *);
    IDWriteTextFormat *fmt = NULL;
    HRESULT hr = IDWriteFactory_CreateTextFormat(g_dwriteFactory, family,
        g_customFontCollection,
        DWRITE_FONT_WEIGHT_NORMAL, style, DWRITE_FONT_STRETCH_NORMAL,
        sizePx, L"en-us", &fmt);
    if (FAILED(hr) || !fmt) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[d2d-text] CreateTextFormat(%s) failed, hr=0x%08lX", logTag, (unsigned long)hr);
        AppendStatus(msg);
        return NULL;
    }
    IDWriteTextFormat_SetTextAlignment(fmt, DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetParagraphAlignment(fmt, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return fmt;
}

/* Mission/date/location text uses "Emblema One", the app's primary display font. */
static IDWriteTextFormat *SV2_D2D_CreatePrimaryFormat(float sizePx, const char *logTag) {
    return SV2_D2D_CreateFormat(L"Emblema One", DWRITE_FONT_STYLE_NORMAL, sizePx, logTag);
}

static IDWriteTextFormat *SV2_D2D_GetMissionFormat(float sizePx) {
    if (g_missionFormat && fabsf(sizePx - g_missionFormatSizePx) < 0.01f) return g_missionFormat;
    if (g_missionFormat) { IDWriteTextFormat_Release(g_missionFormat); g_missionFormat = NULL; }
    g_missionFormat = SV2_D2D_CreatePrimaryFormat(sizePx, "mission");
    g_missionFormatSizePx = sizePx;
    return g_missionFormat;
}

static IDWriteTextFormat *g_dateFormat = NULL;
static float g_dateFormatSizePx = -1.0f;
static IDWriteTextFormat *SV2_D2D_GetDateFormat(float sizePx) {
    if (g_dateFormat && fabsf(sizePx - g_dateFormatSizePx) < 0.01f) return g_dateFormat;
    if (g_dateFormat) { IDWriteTextFormat_Release(g_dateFormat); g_dateFormat = NULL; }
    g_dateFormat = SV2_D2D_CreatePrimaryFormat(sizePx, "date");
    g_dateFormatSizePx = sizePx;
    return g_dateFormat;
}

static IDWriteTextFormat *g_locationFormat = NULL;
static float g_locationFormatSizePx = -1.0f;
static IDWriteTextFormat *SV2_D2D_GetLocationFormat(float sizePx) {
    if (g_locationFormat && fabsf(sizePx - g_locationFormatSizePx) < 0.01f) return g_locationFormat;
    if (g_locationFormat) { IDWriteTextFormat_Release(g_locationFormat); g_locationFormat = NULL; }
    g_locationFormat = SV2_D2D_CreatePrimaryFormat(sizePx, "location");
    g_locationFormatSizePx = sizePx;
    return g_locationFormat;
}

static IDWriteTextFormat *g_broadwayFormat = NULL;
static float g_broadwayFormatSizePx = -1.0f;
static IDWriteTextFormat *SV2_D2D_GetDisplayFormat(float sizePx) {
    if (g_broadwayFormat && fabsf(sizePx - g_broadwayFormatSizePx) < 0.01f) return g_broadwayFormat;
    if (g_broadwayFormat) { IDWriteTextFormat_Release(g_broadwayFormat); g_broadwayFormat = NULL; }
    g_broadwayFormat = SV2_D2D_CreateFormat(L"Limelight", DWRITE_FONT_STYLE_NORMAL, sizePx, "limelight");
    g_broadwayFormatSizePx = sizePx;
    return g_broadwayFormat;
}

/* "Bodoni MT" (stat numbers) is NOT one of this app's 5 embedded fonts. */
static IDWriteTextFormat *g_bodoniFormat = NULL;
static float g_bodoniFormatSizePx = -1.0f;
static IDWriteTextFormat *SV2_D2D_GetBodoniFormat(float sizePx) {
    extern void AppendStatus(const char *);
    if (g_bodoniFormat && fabsf(sizePx - g_bodoniFormatSizePx) < 0.01f) return g_bodoniFormat;
    if (g_bodoniFormat) { IDWriteTextFormat_Release(g_bodoniFormat); g_bodoniFormat = NULL; }

    IDWriteTextFormat *fmt = NULL;
    HRESULT hr = IDWriteFactory_CreateTextFormat(g_dwriteFactory, L"Bodoni MT",
        NULL, /* NULL collection = search the system collection. */
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        sizePx, L"en-us", &fmt);
    if (FAILED(hr) || !fmt) {
        char msg[140];
        snprintf(msg, sizeof(msg), "[d2d-text] 'Bodoni MT' not found in system font collection (hr=0x%08lX) - falling back to 'Emblema One' for stat numbers", (unsigned long)hr);
        AppendStatus(msg);
        fmt = SV2_D2D_CreateFormat(L"Emblema One", DWRITE_FONT_STYLE_NORMAL, sizePx, "statnum-fallback");
    }
    if (!fmt) return NULL;
    IDWriteTextFormat_SetTextAlignment(fmt, DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetParagraphAlignment(fmt, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_bodoniFormat = fmt;
    g_bodoniFormatSizePx = sizePx;
    return g_bodoniFormat;
}

static IDWriteTextFormat *g_titleFormat = NULL;
static float g_titleFormatSizePx = -1.0f;
static IDWriteTextFormat *SV2_D2D_GetTitleFormat(float sizePx) {
    if (g_titleFormat && fabsf(sizePx - g_titleFormatSizePx) < 0.01f) return g_titleFormat;
    if (g_titleFormat) { IDWriteTextFormat_Release(g_titleFormat); g_titleFormat = NULL; }
    g_titleFormat = SV2_D2D_CreateFormat(L"Cinzel", DWRITE_FONT_STYLE_NORMAL, sizePx, "title");
    g_titleFormatSizePx = sizePx;
    return g_titleFormat;
}

static IDWriteTextFormat *g_lifepathFormat = NULL;
static float g_lifepathFormatSizePx = -1.0f;
static IDWriteTextFormat *SV2_D2D_GetLifepathFormat(float sizePx) {
    if (g_lifepathFormat && fabsf(sizePx - g_lifepathFormatSizePx) < 0.01f) return g_lifepathFormat;
    if (g_lifepathFormat) { IDWriteTextFormat_Release(g_lifepathFormat); g_lifepathFormat = NULL; }
    g_lifepathFormat = SV2_D2D_CreateFormat(L"Red Menace", DWRITE_FONT_STYLE_NORMAL, sizePx, "lifepath");
    g_lifepathFormatSizePx = sizePx;
    return g_lifepathFormat;
}

static IDWriteTextFormat *SV2_D2D_CreateDigital7Format(float sizePx, const char *logTag) {
    extern void AppendStatus(const char *);
    IDWriteTextFormat *fmt = SV2_D2D_CreateFormat(L"DSEG7 Classic", DWRITE_FONT_STYLE_NORMAL, sizePx, logTag);
    if (!fmt) {
        /* Shouldn't happen now that digital-7.ttf is embedded (family name confirmed via its own name table) */
        char msg[100];
        snprintf(msg, sizeof(msg), "[d2d-text] 'Digital-7' lookup failed for %s - falling back to 'Emblema One'", logTag);
        AppendStatus(msg);
        fmt = SV2_D2D_CreateFormat(L"Emblema One", DWRITE_FONT_STYLE_NORMAL, sizePx, logTag);
    }
    if (!fmt) return NULL;
    IDWriteTextFormat_SetTextAlignment(fmt, DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetParagraphAlignment(fmt, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return fmt;
}

static IDWriteTextFormat *g_timeFormat = NULL;
static float g_timeFormatSizePx = -1.0f;
static IDWriteTextFormat *SV2_D2D_GetTimeFormat(float sizePx) {
    if (g_timeFormat && fabsf(sizePx - g_timeFormatSizePx) < 0.01f) return g_timeFormat;
    if (g_timeFormat) { IDWriteTextFormat_Release(g_timeFormat); g_timeFormat = NULL; }
    g_timeFormat = SV2_D2D_CreateDigital7Format(sizePx, "time");
    g_timeFormatSizePx = sizePx;
    return g_timeFormat;
}

static IDWriteTextFormat *g_ledFormat = NULL;
static float g_ledFormatSizePx = -1.0f;
static IDWriteTextFormat *SV2_D2D_GetLedFormat(float sizePx) {
    if (g_ledFormat && fabsf(sizePx - g_ledFormatSizePx) < 0.01f) return g_ledFormat;
    if (g_ledFormat) { IDWriteTextFormat_Release(g_ledFormat); g_ledFormat = NULL; }
    g_ledFormat = SV2_D2D_CreateDigital7Format(sizePx, "led");
    g_ledFormatSizePx = sizePx;
    return g_ledFormat;
}

static void SV2_D2D_EnsureBrushes(ID2D1RenderTarget *rt) {
    if (g_brushOwnerRT == rt && g_brushBlack && g_brushRed && g_brushWhite && g_brushStatDark) return;
    if (g_brushBlack) { ID2D1SolidColorBrush_Release(g_brushBlack); g_brushBlack = NULL; }
    if (g_brushRed) { ID2D1SolidColorBrush_Release(g_brushRed); g_brushRed = NULL; }
    if (g_brushWhite) { ID2D1SolidColorBrush_Release(g_brushWhite); g_brushWhite = NULL; }
    if (g_brushStatDark) { ID2D1SolidColorBrush_Release(g_brushStatDark); g_brushStatDark = NULL; }

    D2D1_COLOR_F black = { 10.0f / 255.0f, 10.0f / 255.0f, 10.0f / 255.0f, 1.0f };
    D2D1_COLOR_F red = { 255.0f / 255.0f, 47.0f / 255.0f, 0.0f / 255.0f, 1.0f };
    D2D1_COLOR_F white = { 1.0f, 1.0f, 1.0f, 1.0f };
    D2D1_COLOR_F statDark = { 20.0f / 255.0f, 20.0f / 255.0f, 20.0f / 255.0f, 1.0f };
    ID2D1RenderTarget_CreateSolidColorBrush(rt, &black, NULL, &g_brushBlack);
    ID2D1RenderTarget_CreateSolidColorBrush(rt, &red, NULL, &g_brushRed);
    ID2D1RenderTarget_CreateSolidColorBrush(rt, &white, NULL, &g_brushWhite);
    ID2D1RenderTarget_CreateSolidColorBrush(rt, &statDark, NULL, &g_brushStatDark);
    g_brushOwnerRT = rt;
}

static void SV2_D2D_DrawMissionPass(ID2D1RenderTarget *rt, IDWriteTextFormat *fmt,
                                     const wchar_t *wtext, int len, float cx, float cy,
                                     float angleDeg, ID2D1SolidColorBrush *brush) {
    D2D1_MATRIX_3X2_F rot;
    /* Sign flipped from the GDI escapement convention. */
    D2D1MakeRotateMatrix(-(FLOAT)angleDeg, (D2D1_POINT_2F){ cx, cy }, &rot);
    ID2D1RenderTarget_SetTransform(rt, &rot);

    D2D1_RECT_F layoutRect;
    layoutRect.left = cx - 1000.0f;
    layoutRect.right = cx + 1000.0f;
    layoutRect.top = cy - 200.0f;
    layoutRect.bottom = cy + 200.0f;
    ID2D1RenderTarget_DrawText(rt, wtext, (UINT32)len, fmt, &layoutRect, (ID2D1Brush *)brush,
        D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

    D2D1_MATRIX_3X2_F identity = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    ID2D1RenderTarget_SetTransform(rt, &identity);
}

/* DATE has no rotation and no shadow pass, so this skips the transform dance entirely. */
static void SV2_D2D_DrawPlainCentered(ID2D1RenderTarget *rt, IDWriteTextFormat *fmt,
                                       const wchar_t *wtext, int len, float cx, float cy,
                                       ID2D1SolidColorBrush *brush) {
    D2D1_RECT_F layoutRect;
    layoutRect.left = cx - 1000.0f;
    layoutRect.right = cx + 1000.0f;
    layoutRect.top = cy - 200.0f;
    layoutRect.bottom = cy + 200.0f;
    ID2D1RenderTarget_DrawText(rt, wtext, (UINT32)len, fmt, &layoutRect, (ID2D1Brush *)brush,
        D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
}

/* Measures a string's natural (unrotated) width/height in DIPs under a given format. */
static void SV2_D2D_MeasureText(IDWriteTextFormat *fmt, const wchar_t *wtext, int len, float *outW, float *outH) {
    *outW = 0.0f; *outH = 0.0f;
    IDWriteTextLayout *layout = NULL;
    HRESULT hr = IDWriteFactory_CreateTextLayout(g_dwriteFactory, wtext, (UINT32)len, fmt, 8192.0f, 8192.0f, &layout);
    if (FAILED(hr) || !layout) return;
    DWRITE_TEXT_METRICS metrics;
    if (SUCCEEDED(IDWriteTextLayout_GetMetrics(layout, &metrics))) {
        *outW = metrics.width;
        *outH = metrics.height;
    }
    IDWriteTextLayout_Release(layout);
}

/* TIME/LED: rotated text drawn inside an axis-aligned screen-space clip band. */
static void SV2_D2D_DrawRotatedClipped(ID2D1RenderTarget *rt, IDWriteTextFormat *fmt,
                                        const wchar_t *wtext, int len, float cx, float cy,
                                        float angleDeg, ID2D1SolidColorBrush *brush,
                                        float clipLeft, float clipTop, float clipRight, float clipBottom) {
    D2D1_RECT_F clip = { clipLeft, clipTop, clipRight, clipBottom };
    ID2D1RenderTarget_PushAxisAlignedClip(rt, &clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    D2D1_MATRIX_3X2_F rot;
    D2D1MakeRotateMatrix(-(FLOAT)angleDeg, (D2D1_POINT_2F){ cx, cy }, &rot);
    ID2D1RenderTarget_SetTransform(rt, &rot);

    D2D1_RECT_F layoutRect;
    layoutRect.left = cx - 4000.0f;
    layoutRect.right = cx + 4000.0f;
    layoutRect.top = cy - 400.0f;
    layoutRect.bottom = cy + 400.0f;
    ID2D1RenderTarget_DrawText(rt, wtext, (UINT32)len, fmt, &layoutRect, (ID2D1Brush *)brush,
        D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

    D2D1_MATRIX_3X2_F identity = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    ID2D1RenderTarget_SetTransform(rt, &identity);
    ID2D1RenderTarget_PopAxisAlignedClip(rt);
}

/* Same as above, but the caller supplies the exact left edge to start the text at. */


void SV2_D2D_Phase3Paint(HWND hwnd, ID2D1RenderTarget *rt) {
    if (!SE_g_saveLoaded) return;
    if (!SV2_D2D_Text_Init()) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    SV2LayoutMirror L = SV2_GetLayout(rc.right - rc.left, rc.bottom - rc.top);
    SV2_D2D_EnsureBrushes(rt);

    /* MISSION - rotated, black base + red shadow-offset. */
    if (SV2_g_quest[0] && g_brushBlack && g_brushRed) {
        float fontPx = (float)(SV2P3_MISSION_FONTPX * L.scale);
        if (fontPx < 6.0f) fontPx = 6.0f;
        IDWriteTextFormat *fmt = SV2_D2D_GetMissionFormat(fontPx);
        if (fmt) {
            wchar_t wtext[256];
            int len = MultiByteToWideChar(CP_UTF8, 0, SV2_g_quest, -1, wtext, 256);
            if (len > 0) {
                len -= 1;
                float cx = (float)SV2_MapX(L, SV2P3_MISSION_CX);
                float cy = (float)SV2_MapY(L, SV2P3_MISSION_CY);
                float shadowCx = cx - (float)(SV2P3_MISSION_SHADOW_DX * L.scale);
                float shadowCy = cy - (float)(SV2P3_MISSION_SHADOW_DY * L.scale);
                SV2_D2D_DrawMissionPass(rt, fmt, wtext, len, cx, cy, (float)SV2P3_MISSION_ANGLE, g_brushBlack);
                SV2_D2D_DrawMissionPass(rt, fmt, wtext, len, shadowCx, shadowCy, (float)SV2P3_MISSION_ANGLE, g_brushRed);
            }
        }
    }

    /* DATE - no rotation, no shadow, plain white, reuses the same custom font collection/family. */
    if (SV2_g_dateOnly[0] && g_brushWhite) {
        float dateFontPx = (float)(SV2P3_DATE_FONTPX * L.scale);
        if (dateFontPx < 6.0f) dateFontPx = 6.0f;
        IDWriteTextFormat *dateFmt = SV2_D2D_GetDateFormat(dateFontPx);
        if (dateFmt) {
            wchar_t dwtext[64];
            int dlen = MultiByteToWideChar(CP_UTF8, 0, SV2_g_dateOnly, -1, dwtext, 64);
            if (dlen > 0) {
                dlen -= 1;
                float dcx = (float)SV2_MapX(L, SV2P3_DATE_CX);
                float dcy = (float)SV2_MapY(L, SV2P3_DATE_CY);
                SV2_D2D_DrawPlainCentered(rt, dateFmt, dwtext, dlen, dcx, dcy, g_brushWhite);
            }
        }
    }
    /* LOCATION - beside the pin icon, no rotation, no shadow, plain white, reuses the same. */
    if (SV2_g_location[0] && g_brushWhite) {
        int textLen = (int)strlen(SV2_g_location);
        float lenScale = textLen > 0 ? (float)(SV2P3_LOCATION_BASE_LEN / textLen) : 1.0f;
        if (lenScale > 1.8f) lenScale = 1.8f;   /* don't blow up for very short names. */
        if (lenScale < 0.5f) lenScale = 0.5f;   /* don't shrink to unreadable for very long ones. */

        float locFontPx = (float)(SV2P3_LOCATION_FONTPX * L.scale * lenScale);
        if (locFontPx < 6.0f) locFontPx = 6.0f;
        IDWriteTextFormat *locFmt = SV2_D2D_GetLocationFormat(locFontPx);
        if (locFmt) {
            wchar_t lwtext[128];
            int llen = MultiByteToWideChar(CP_UTF8, 0, SV2_g_location, -1, lwtext, 128);
            if (llen > 0) {
                llen -= 1;
                float lcx = (float)SV2_MapX(L, SV2P3_LOCATION_CX);
                float lcy = (float)SV2_MapY(L, SV2P3_LOCATION_CY);
                SV2_D2D_DrawPlainCentered(rt, locFmt, lwtext, llen, lcx, lcy, g_brushWhite);
            }
        }
    }

    /* LEVEL_NUM / CRED_NUM. */
    if (g_brushWhite) {
        float lcFontPx = (float)(SV2P3_LEVELCREDNUM_FONTPX * L.scale);
        if (lcFontPx < 10.0f) lcFontPx = 10.0f;
        IDWriteTextFormat *lcFmt = SV2_D2D_GetDisplayFormat(lcFontPx);
        if (lcFmt) {
            const char *levelVal = SV2_D2D_GetLevelNumText();
            if (levelVal && levelVal[0]) {
                wchar_t wtext[16];
                int wl = MultiByteToWideChar(CP_UTF8, 0, levelVal, -1, wtext, 16);
                if (wl > 0) {
                    wl -= 1;
                    SV2_D2D_DrawPlainCentered(rt, lcFmt, wtext, wl,
                        (float)SV2_MapX(L, SV2P3_LEVELNUM_CX), (float)SV2_MapY(L, SV2P3_LEVELNUM_CY), g_brushWhite);
                }
            }
            const char *credVal = SV2_D2D_GetCredNumText();
            if (credVal && credVal[0]) {
                wchar_t wtext[16];
                int wl = MultiByteToWideChar(CP_UTF8, 0, credVal, -1, wtext, 16);
                if (wl > 0) {
                    wl -= 1;
                    SV2_D2D_DrawPlainCentered(rt, lcFmt, wtext, wl,
                        (float)SV2_MapX(L, SV2P3_CREDNUM_CX), (float)SV2_MapY(L, SV2P3_CREDNUM_CY), g_brushWhite);
                }
            }
        }
    }

    /* Per-glass STAT NUMBERS (up to 18). */
    {
        float statFontPx = (float)(SV2_StatNumFontPx * L.scale);
        if (statFontPx < 6.0f) statFontPx = 6.0f;
        IDWriteTextFormat *statFmt = SV2_D2D_GetBodoniFormat(statFontPx);
        if (statFmt) {
            for (int gi = 0; gi < SV2P3_GLASS_COUNT; gi++) {
                double gcx = 0, gcy = 0;
                BOOL hovered = FALSE;
                const char *val = SV2_D2D_GetGlassNumText(gi, &gcx, &gcy, &hovered);
                if (!val || !val[0]) continue;
                wchar_t wtext[16];
                int wl = MultiByteToWideChar(CP_UTF8, 0, val, -1, wtext, 16);
                if (wl <= 0) continue;
                wl -= 1;
                ID2D1SolidColorBrush *brush = hovered ? g_brushWhite : g_brushStatDark;
                SV2_D2D_DrawPlainCentered(rt, statFmt, wtext, wl,
                    (float)SV2_MapX(L, gcx), (float)SV2_MapY(L, gcy), brush);
            }
        }
    }

    /* TITLE (save name). */
    if (SV2_g_saveName[0] && g_brushWhite) {
        float titleFontPx = (float)(SV2P3_TITLE_FONTPX * L.scale);
        if (titleFontPx < 6.0f) titleFontPx = 6.0f;
        IDWriteTextFormat *titleFmt = SV2_D2D_GetTitleFormat(titleFontPx);
        if (titleFmt) {
            wchar_t twtext[256];
            int tlen = MultiByteToWideChar(CP_UTF8, 0, SV2_g_saveName, -1, twtext, 256);
            if (tlen > 0) {
                tlen -= 1;
                float tcx = (float)SV2_MapX(L, SV2P3_TITLE_CX);
                float tcy = (float)SV2_MapY(L, SV2P3_TITLE_CY);
                SV2_D2D_DrawMissionPass(rt, titleFmt, twtext, tlen, tcx, tcy, (float)SV2P3_TITLE_ANGLE, g_brushWhite);
            }
        }
    }

    /* LIFEPATH - rotated, single white pass, "Red Menace", uppercased. */
    if (SV2_g_lifePath[0] && g_brushWhite) {
        char lifePathUpper[64];
        strncpy(lifePathUpper, SV2_g_lifePath, sizeof(lifePathUpper) - 1);
        lifePathUpper[sizeof(lifePathUpper) - 1] = '\0';
        CharUpperA(lifePathUpper);

        float lifeFontPx = (float)(SV2P3_LIFEPATH_FONTPX * L.scale);
        if (lifeFontPx < 6.0f) lifeFontPx = 6.0f;
        IDWriteTextFormat *lifeFmt = SV2_D2D_GetLifepathFormat(lifeFontPx);
        if (lifeFmt) {
            wchar_t lwtext[64];
            int llen = MultiByteToWideChar(CP_UTF8, 0, lifePathUpper, -1, lwtext, 64);
            if (llen > 0) {
                llen -= 1;
                float lcx = (float)SV2_MapX(L, SV2P3_LIFEPATH_CX);
                float lcy = (float)SV2_MapY(L, SV2P3_LIFEPATH_CY);
                /* Unlike mission/title/date (drawn via GDI's TextOut escapement in the original app),. */
                SV2_D2D_DrawMissionPass(rt, lifeFmt, lwtext, llen, lcx, lcy, -(float)SV2P3_LIFEPATH_ANGLE, g_brushWhite);
            }
        }
    }

    /* TIME - rotated -90, clipped to a band sized from the actual "00:00 pm" glyph metrics (not a fixed guess) */
    if (SV2_g_timeOnly[0] && g_brushWhite) {
        float timeFontPx = (float)(SV2P3_TIME_FONTPX * L.scale);
        if (timeFontPx < 6.0f) timeFontPx = 6.0f;
        IDWriteTextFormat *timeFmt = SV2_D2D_GetTimeFormat(timeFontPx);
        if (timeFmt) {
            wchar_t twtext[64];
            int tlen = MultiByteToWideChar(CP_UTF8, 0, SV2_g_timeOnly, -1, twtext, 64);
            if (tlen > 0) {
                tlen -= 1;
                float sampleW = 0, sampleH = 0;
                SV2_D2D_MeasureText(timeFmt, L"00:00 pm", 8, &sampleW, &sampleH);

                float tBandTop = 0.0f;
                float tBandBottom = (float)rc.bottom;
                float tBandLeft = (float)SV2_MapX(L, 0) - sampleH;
                float tBandRight = (float)SV2_MapX(L, SV2P3_TIME_CX) + sampleH + (float)(20 * L.scale);

                float tcx = (float)SV2_MapX(L, SV2P3_TIME_CX);
                float tcy = (float)SV2_MapY(L, (SV2_TimeTop + SV2_TimeBottom) / 2.0);
                SV2_D2D_DrawRotatedClipped(rt, timeFmt, twtext, tlen, tcx, tcy, -90.0f, g_brushWhite,
                    tBandLeft, tBandTop, tBandRight, tBandBottom);
            }
        }
    }

    /* LED - infinite vertical scroll of "SAVED ", clipped to its own band. */
    {
        float ledFontPx = (float)(SV2P3_LED_FONTPX * L.scale);
        if (ledFontPx < 6.0f) ledFontPx = 6.0f;
        IDWriteTextFormat *ledFmt = SV2_D2D_GetLedFormat(ledFontPx);
        if (ledFmt && g_brushWhite) {
            float unitW = 0, unitH = 0;
            SV2_D2D_MeasureText(ledFmt, L"SAVED", 5, &unitW, &unitH); /* no trailing space. */
            double unitLenScreen = unitW > 0 ? (double)unitW : 40.0;
            double gapScreen = unitLenScreen * 0.15; /* a little breathing room between repeats, proportion of glyph width so it scales with font. */
            double strideScreen = unitLenScreen + gapScreen;
            double strideNative = strideScreen / L.scale;

            if (strideNative < 1.0) strideNative = 1.0; /* guard against a degenerate/zero measurement. */
            double wrappedScroll = fmod(SV2_LedScrollY, strideNative);
            if (wrappedScroll < 0) wrappedScroll += strideNative;
            SV2_LedScrollY = wrappedScroll; /* keeps the shared global bounded. */

            float bandTop = (float)SV2_MapY(L, SV2_LedTop);
            float bandBottom = (float)SV2_MapY(L, SV2_LedBottom);
            float bandLeft = (float)SV2_MapX(L, SV2P3_LED_CX) - (float)(40 * L.scale);
            float bandRight = (float)SV2_MapX(L, SV2P3_LED_CX) + (float)(40 * L.scale);
            float ledCx = (float)SV2_MapX(L, SV2P3_LED_CX);

            /* Enough units to cover the band with margin on both ends. */
            double bandHeightScreen = (double)(bandBottom - bandTop);
            int unitsNeeded = (int)ceil(bandHeightScreen / strideScreen) + 3;

            wchar_t unitText[8] = L"SAVED";
            int unitLen = 5;
            for (int k = -1; k <= unitsNeeded; k++) {
                float unitY = bandTop + (float)(k * strideScreen - wrappedScroll * L.scale);
                if (unitY < bandTop - (float)strideScreen || unitY > bandBottom + (float)strideScreen) continue;
                SV2_D2D_DrawRotatedClipped(rt, ledFmt, unitText, unitLen, ledCx, unitY, -90.0f, g_brushWhite,
                    bandLeft, bandTop, bandRight, bandBottom);
            }
        }
    }
}

void SV2_D2D_Text_Shutdown(void) {
    if (g_brushBlack) { ID2D1SolidColorBrush_Release(g_brushBlack); g_brushBlack = NULL; }
    if (g_brushRed) { ID2D1SolidColorBrush_Release(g_brushRed); g_brushRed = NULL; }
    if (g_brushWhite) { ID2D1SolidColorBrush_Release(g_brushWhite); g_brushWhite = NULL; }
    if (g_brushStatDark) { ID2D1SolidColorBrush_Release(g_brushStatDark); g_brushStatDark = NULL; }
    g_brushOwnerRT = NULL;
    if (g_missionFormat) { IDWriteTextFormat_Release(g_missionFormat); g_missionFormat = NULL; }
    if (g_dateFormat) { IDWriteTextFormat_Release(g_dateFormat); g_dateFormat = NULL; }
    if (g_titleFormat) { IDWriteTextFormat_Release(g_titleFormat); g_titleFormat = NULL; }
    if (g_lifepathFormat) { IDWriteTextFormat_Release(g_lifepathFormat); g_lifepathFormat = NULL; }
    if (g_timeFormat) { IDWriteTextFormat_Release(g_timeFormat); g_timeFormat = NULL; }
    if (g_ledFormat) { IDWriteTextFormat_Release(g_ledFormat); g_ledFormat = NULL; }
    if (g_locationFormat) { IDWriteTextFormat_Release(g_locationFormat); g_locationFormat = NULL; }
    if (g_broadwayFormat) { IDWriteTextFormat_Release(g_broadwayFormat); g_broadwayFormat = NULL; }
    if (g_bodoniFormat) { IDWriteTextFormat_Release(g_bodoniFormat); g_bodoniFormat = NULL; }
    if (g_customFontCollection) { IDWriteFontCollection_Release(g_customFontCollection); g_customFontCollection = NULL; }
    if (g_dwriteFactory) { IDWriteFactory_Release(g_dwriteFactory); g_dwriteFactory = NULL; }
}
