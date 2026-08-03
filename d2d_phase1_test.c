/* Direct2D save-editor renderer. */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <stdio.h>
#include <d2d1.h>

static ID2D1Factory *g_d2dFactory = NULL;
static ID2D1HwndRenderTarget *g_d2dRT = NULL;
static HWND g_d2dHwnd = NULL;

BOOL SV2_D2D_Init(HWND hwnd) {
    if (g_d2dRT && g_d2dHwnd == hwnd) return TRUE; /* already set up for this window. */

    if (!g_d2dFactory) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            &IID_ID2D1Factory, NULL, (void **)&g_d2dFactory);
        if (FAILED(hr)) {
            extern void AppendStatus(const char *);
            char msg[100];
            snprintf(msg, sizeof(msg), "[d2d] D2D1CreateFactory failed, hr=0x%08lX", (unsigned long)hr);
            AppendStatus(msg);
            return FALSE;
        }
    }

    RECT rc;
    GetClientRect(hwnd, &rc);
    D2D1_SIZE_U size = { (UINT32)(rc.right - rc.left), (UINT32)(rc.bottom - rc.top) };

    D2D1_RENDER_TARGET_PROPERTIES rtProps;
    ZeroMemory(&rtProps, sizeof(rtProps));
    rtProps.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    rtProps.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;

    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps;
    ZeroMemory(&hwndProps, sizeof(hwndProps));
    hwndProps.hwnd = hwnd;
    hwndProps.pixelSize = size;
    /* RETAIN_CONTENTS keeps the back buffer's content between Present calls instead of fully. */
    hwndProps.presentOptions = D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS;

    if (g_d2dRT) { ID2D1HwndRenderTarget_Release(g_d2dRT); g_d2dRT = NULL; }

    HRESULT hr = ID2D1Factory_CreateHwndRenderTarget(g_d2dFactory, &rtProps, &hwndProps, &g_d2dRT);
    if (SUCCEEDED(hr) && g_d2dRT) {
        /* D2D render targets default to scaling DIPs by the system DPI setting. */
        ID2D1RenderTarget_SetDpi((ID2D1RenderTarget *)g_d2dRT, 96.0f, 96.0f);
    }
    if (FAILED(hr)) {
        extern void AppendStatus(const char *);
        char msg[100];
        snprintf(msg, sizeof(msg), "[d2d] CreateHwndRenderTarget failed, hr=0x%08lX", (unsigned long)hr);
        AppendStatus(msg);
        return FALSE;
    }
    g_d2dHwnd = hwnd;

    extern void AppendStatus(const char *);
    AppendStatus("[d2d] Phase 1 device + render target created successfully");
    return TRUE;
}

/* Shared frame lifecycle. */
static D2D1_SIZE_U g_lastSize = { 0, 0 };

ID2D1RenderTarget *SV2_D2D_BeginFrame(HWND hwnd) {
    if (!SV2_D2D_Init(hwnd)) return NULL;

    RECT rc;
    GetClientRect(hwnd, &rc);
    D2D1_SIZE_U size = { (UINT32)(rc.right - rc.left), (UINT32)(rc.bottom - rc.top) };
    /* Resize() discards/resets the render target's contents. */
    if (size.width != g_lastSize.width || size.height != g_lastSize.height) {
        ID2D1HwndRenderTarget_Resize(g_d2dRT, &size);
        g_lastSize = size;
    }

    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)g_d2dRT;
    ID2D1RenderTarget_BeginDraw(rt);

    D2D1_COLOR_F clearColor = { 0.05f, 0.05f, 0.15f, 1.0f };
    ID2D1RenderTarget_Clear(rt, &clearColor);
    return rt;
}

void SV2_D2D_EndFrame(ID2D1RenderTarget *rt) {
    HRESULT hr = ID2D1RenderTarget_EndDraw(rt, NULL, NULL);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        ID2D1HwndRenderTarget_Release(g_d2dRT);
        g_d2dRT = NULL;
        g_d2dHwnd = NULL;
    }
}

void SV2_D2D_TestPaint(HWND hwnd) {
    ID2D1RenderTarget *rt = SV2_D2D_BeginFrame(hwnd);
    if (!rt) return;

    extern void SV2_D2D_Phase2Paint(HWND, ID2D1RenderTarget *);
    SV2_D2D_Phase2Paint(hwnd, rt);

    extern void SV2_D2D_Phase2bPaint(HWND, ID2D1RenderTarget *);
    SV2_D2D_Phase2bPaint(hwnd, rt);

    extern void SV2_D2D_Phase3Paint(HWND, ID2D1RenderTarget *);
    SV2_D2D_Phase3Paint(hwnd, rt);

    SV2_D2D_EndFrame(rt);
}

void SV2_D2D_Shutdown(void) {
    extern void SV2_D2D_Text_Shutdown(void);
    SV2_D2D_Text_Shutdown();
    if (g_d2dRT) { ID2D1HwndRenderTarget_Release(g_d2dRT); g_d2dRT = NULL; }
    if (g_d2dFactory) { ID2D1Factory_Release(g_d2dFactory); g_d2dFactory = NULL; }
    g_d2dHwnd = NULL;
}

/* PHASE 2 - static image compositing via D2D + WIC. */
#include <wincodec.h>

/* Mirrors the layout struct/functions in launcher_merged. */
typedef struct { int x, y, w, h; double scale; } SV2LayoutMirror;
extern SV2LayoutMirror SV2_GetLayout(int clientW, int clientH);
extern int SV2_MapX(SV2LayoutMirror L, double nx);
extern int SV2_MapY(SV2LayoutMirror L, double ny);

static IWICImagingFactory *g_wicFactory = NULL;
static BOOL g_comInitialized = FALSE;

typedef struct {
    const char *resourceName;
    ID2D1Bitmap *bitmap;
    BOOL loadAttempted;
} SV2D2DCachedImage;

static SV2D2DCachedImage g_testImages[] = {
    { "SV2_BACKGROUND", NULL, FALSE },
    { "SV2_DAVIDSHEAD", NULL, FALSE },
    { "SV2_DAVIDSJACKET", NULL, FALSE },
    { "SV2_SANDEVISTAN", NULL, FALSE },
    { "SV2_SAVE_LOCATION", NULL, FALSE },
    { "SV2_SHOT", NULL, FALSE },
    { "SV2_OUTSIDESHAPES", NULL, FALSE },
    { "SV2_ENGINEERING_GLASS", NULL, FALSE },
    { "SV2_COMBAT_HACKING_GLASS", NULL, FALSE },
    { "SV2_HACKING_GLASS", NULL, FALSE },
    { "SV2_TECHNICAL_ABILITY_GLASS", NULL, FALSE },
    { "SV2_KENJUTSU_GLASS", NULL, FALSE },
    { "SV2_DEMOLITION_GLASS", NULL, FALSE },
    { "SV2_INTELLIGENCE_GLASS", NULL, FALSE },
    { "SV2_COOL_GLASS", NULL, FALSE },
    { "SV2_REFLEXES_GLASS", NULL, FALSE },
    { "SV2_STRENGTH_GLASS", NULL, FALSE },
    { "SV2_GUNSLINGER_GLASS", NULL, FALSE },
    { "SV2_CRAFTING_GLASS", NULL, FALSE },
    { "SV2_ESPIONAGE_GLASS", NULL, FALSE },
    { "SV2_STEALTH_GLASS", NULL, FALSE },
    { "SV2_CONSOLE_GLASS", NULL, FALSE },
    { "SV2_LOAD_SAVE_GLASS", NULL, FALSE },
    { "SV2_SAVE_CHANGES_GLASS", NULL, FALSE },
    { "SV2_EXTRA_GLASS", NULL, FALSE },
    { "SV2_BACK_TO_LAUNCHER", NULL, FALSE },
};
#define SV2_TEST_IMAGE_COUNT (sizeof(g_testImages) / sizeof(g_testImages[0]))

/* Glass entries in g_testImages start at index 7 and run 18 entries. */
#define SV2_GLASS_START_INDEX 7
#define SV2_GLASS_COUNT_MIRROR 18

/* Inverted counterparts, one per glass, same order as SV2_Glasses[] in the main source. */
static SV2D2DCachedImage g_glassInvImages[SV2_GLASS_COUNT_MIRROR] = {
    { "SV2_ENGINEERING_GLASS_INV", NULL, FALSE },
    { "SV2_COMBAT_HACKING_GLASS_INV", NULL, FALSE },
    { "SV2_HACKING_GLASS_INV", NULL, FALSE },
    { "SV2_TECHNICAL_ABILITY_GLASS_INV", NULL, FALSE },
    { "SV2_KENJUTSU_GLASS_INV", NULL, FALSE },
    { "SV2_DEMOLITION_GLASS_INV", NULL, FALSE },
    { "SV2_INTELLIGENCE_GLASS_INV", NULL, FALSE },
    { "SV2_COOL_GLASS_INV", NULL, FALSE },
    { "SV2_REFLEXES_GLASS_INV", NULL, FALSE },
    { "SV2_STRENGTH_GLASS_INV", NULL, FALSE },
    { "SV2_GUNSLINGER_GLASS_INV", NULL, FALSE },
    { "SV2_CRAFTING_GLASS_INV", NULL, FALSE },
    { "SV2_ESPIONAGE_GLASS_INV", NULL, FALSE },
    { "SV2_STEALTH_GLASS_INV", NULL, FALSE },
    { "SV2_CONSOLE_GLASS_INV", NULL, FALSE },
    { "SV2_LOAD_SAVE_GLASS_INV", NULL, FALSE },
    { "SV2_SAVE_CHANGES_GLASS_INV", NULL, FALSE },
    { "SV2_EXTRA_GLASS_INV", NULL, FALSE },
};
extern int SV2_HoveredGlass;

static BOOL SV2_D2D_EnsureWIC(void) {
    if (g_wicFactory) return TRUE;
    if (!g_comInitialized) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        g_comInitialized = TRUE;
    }
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&g_wicFactory);
    if (FAILED(hr)) {
        extern void AppendStatus(const char *);
        char msg[100];
        snprintf(msg, sizeof(msg), "[d2d] WIC factory creation failed, hr=0x%08lX", (unsigned long)hr);
        AppendStatus(msg);
        return FALSE;
    }
    return TRUE;
}

static ID2D1Bitmap *SV2_D2D_LoadBitmapFromResource(ID2D1RenderTarget *rt, const char *resourceName) {
    extern void AppendStatus(const char *);
    if (!SV2_D2D_EnsureWIC()) return NULL;

    HMODULE hMod = GetModuleHandleA(NULL);
    HRSRC hRes = FindResourceA(hMod, resourceName, RT_RCDATA);
    if (!hRes) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[d2d] resource '%s' not found", resourceName);
        AppendStatus(msg);
        return NULL;
    }
    HGLOBAL hData = LoadResource(hMod, hRes);
    DWORD size = SizeofResource(hMod, hRes);
    void *pData = LockResource(hData);
    if (!pData || !size) return NULL;

    IWICStream *stream = NULL;
    HRESULT hr = IWICImagingFactory_CreateStream(g_wicFactory, &stream);
    if (FAILED(hr) || !stream) { AppendStatus("[d2d] IWICStream creation failed"); return NULL; }
    hr = IWICStream_InitializeFromMemory(stream, (BYTE *)pData, size);
    if (FAILED(hr)) { AppendStatus("[d2d] IWICStream InitializeFromMemory failed"); IWICStream_Release(stream); return NULL; }

    IWICBitmapDecoder *decoder = NULL;
    hr = IWICImagingFactory_CreateDecoderFromStream(g_wicFactory, (IStream *)stream, NULL,
        WICDecodeMetadataCacheOnLoad, &decoder);
    IWICStream_Release(stream);
    if (FAILED(hr) || !decoder) { AppendStatus("[d2d] CreateDecoderFromStream failed"); return NULL; }

    IWICBitmapFrameDecode *frame = NULL;
    hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    IWICBitmapDecoder_Release(decoder);
    if (FAILED(hr) || !frame) { AppendStatus("[d2d] GetFrame failed"); return NULL; }

    IWICFormatConverter *converter = NULL;
    hr = IWICImagingFactory_CreateFormatConverter(g_wicFactory, &converter);
    if (FAILED(hr) || !converter) { IWICBitmapFrameDecode_Release(frame); AppendStatus("[d2d] CreateFormatConverter failed"); return NULL; }
    hr = IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    IWICBitmapFrameDecode_Release(frame);
    if (FAILED(hr)) { IWICFormatConverter_Release(converter); AppendStatus("[d2d] format converter Initialize failed"); return NULL; }

    ID2D1Bitmap *bmp = NULL;
    hr = ID2D1RenderTarget_CreateBitmapFromWicBitmap(rt, (IWICBitmapSource *)converter, NULL, &bmp);
    IWICFormatConverter_Release(converter);
    if (FAILED(hr) || !bmp) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[d2d] CreateBitmapFromWicBitmap failed for '%s', hr=0x%08lX", resourceName, (unsigned long)hr);
        AppendStatus(msg);
        return NULL;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "[d2d] loaded '%s' successfully", resourceName);
    AppendStatus(msg);
    return bmp;
}

void SV2_D2D_Phase2Paint(HWND hwnd, ID2D1RenderTarget *rt) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    SV2LayoutMirror L = SV2_GetLayout(rc.right - rc.left, rc.bottom - rc.top);

    for (size_t i = 0; i < SV2_TEST_IMAGE_COUNT; i++) {
        /* The currently-hovered glass is skipped here and drawn afterward instead, using its. */
        int glassIdx = (int)i - SV2_GLASS_START_INDEX;
        if (glassIdx >= 0 && glassIdx < SV2_GLASS_COUNT_MIRROR && glassIdx == SV2_HoveredGlass) continue;

        if (!g_testImages[i].loadAttempted) {
            g_testImages[i].loadAttempted = TRUE;
            g_testImages[i].bitmap = SV2_D2D_LoadBitmapFromResource(rt, g_testImages[i].resourceName);
        }
        if (!g_testImages[i].bitmap) continue;

        /* Every PNG in this app is a full 1918x1008 canvas-sized layer with transparency around the. */
        D2D1_RECT_F destRect;
        destRect.left = (FLOAT)SV2_MapX(L, 0);
        destRect.top = (FLOAT)SV2_MapY(L, 0);
        destRect.right = (FLOAT)SV2_MapX(L, 1918);
        destRect.bottom = (FLOAT)SV2_MapY(L, 1008);

        ID2D1RenderTarget_DrawBitmap(rt, g_testImages[i].bitmap, &destRect, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
    }

    /* Hovered glass, drawn on top using its inverted image. */
    if (SV2_HoveredGlass >= 0 && SV2_HoveredGlass < SV2_GLASS_COUNT_MIRROR) {
        if (!g_glassInvImages[SV2_HoveredGlass].loadAttempted) {
            g_glassInvImages[SV2_HoveredGlass].loadAttempted = TRUE;
            g_glassInvImages[SV2_HoveredGlass].bitmap = SV2_D2D_LoadBitmapFromResource(rt, g_glassInvImages[SV2_HoveredGlass].resourceName);
        }
        ID2D1Bitmap *hoverBmp = g_glassInvImages[SV2_HoveredGlass].bitmap;
        int normalIdx = SV2_GLASS_START_INDEX + SV2_HoveredGlass;
        if (!hoverBmp && normalIdx < (int)SV2_TEST_IMAGE_COUNT) hoverBmp = g_testImages[normalIdx].bitmap;

        if (hoverBmp) {
            D2D1_RECT_F destRect;
            destRect.left = (FLOAT)SV2_MapX(L, 0);
            destRect.top = (FLOAT)SV2_MapY(L, 0);
            destRect.right = (FLOAT)SV2_MapX(L, 1918);
            destRect.bottom = (FLOAT)SV2_MapY(L, 1008);
            ID2D1RenderTarget_DrawBitmap(rt, hoverBmp, &destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
        }
    }
}

/* PHASE 2b - the save-dependent static elements Phase 2 originally left out (screenshot mask, location pin, month image) */

/* Mirrors SV2Img's layout. */
typedef struct { void *img; RECT bbox; int valid; } SV2ImgMirror;
extern SV2ImgMirror SV2_LocationPin;
extern SV2ImgMirror SV2_Image;
extern SV2ImgMirror SV2_Months[12];
extern void *SV2_ScreenshotMasked; /* GpImage* from the GDI+ side. */
extern int SV2_MonthFromSaveInfo(void);
extern int SE_g_saveLoaded;

#define SV2P2B_PIN_CX 28.0 /* nudged left from the original 46.0. */
#define SV2P2B_PIN_CY 824.0

static ID2D1Bitmap *g_pinBitmap = NULL;
static BOOL g_pinLoadAttempted = FALSE;
static ID2D1Bitmap *g_monthBitmaps[12] = { NULL };
static BOOL g_monthLoadAttempted[12] = { FALSE };
static const char *g_monthResNames[12] = {
    "SV2_JANUARY", "SV2_FEBRUARY", "SV2_MARCH", "SV2_APRIL", "SV2_MAY", "SV2_JUNE",
    "SV2_JULY", "SV2_AUGUST", "SV2_SEPTEMBER", "SV2_OCTOBER", "SV2_NOVEMBER", "SV2_DECEMBER"
};

/* GDI+ flat C API. */
typedef int GpStatus;
extern GpStatus WINAPI GdipCreateHBITMAPFromBitmap(void *bitmap, HBITMAP *hbmReturn, UINT32 background);

static void *g_lastScreenshotSource = NULL;
static ID2D1Bitmap *g_screenshotBitmap = NULL;

static ID2D1Bitmap *SV2_D2D_GetScreenshotBitmap(ID2D1RenderTarget *rt) {
    extern void AppendStatus(const char *);
    if (!SV2_ScreenshotMasked) return NULL;
    if (SV2_ScreenshotMasked == g_lastScreenshotSource && g_screenshotBitmap) return g_screenshotBitmap;

    /* Source changed (new save loaded, or masking redone). */
    if (g_screenshotBitmap) { ID2D1Bitmap_Release(g_screenshotBitmap); g_screenshotBitmap = NULL; }
    g_lastScreenshotSource = SV2_ScreenshotMasked;

    if (!SV2_D2D_EnsureWIC()) return NULL;

    HBITMAP hbm = NULL;
    GpStatus st = GdipCreateHBITMAPFromBitmap(SV2_ScreenshotMasked, &hbm, 0x00FFFFFF /* transparent-ish background, alpha preserved. */);
    if (st != 0 /* Ok. */ || !hbm) {
        char msg[100];
        snprintf(msg, sizeof(msg), "[d2d] GdipCreateHBITMAPFromBitmap failed for screenshot, status=%d", st);
        AppendStatus(msg);
        return NULL;
    }

    IWICBitmap *wicBmp = NULL;
    HRESULT hr = IWICImagingFactory_CreateBitmapFromHBITMAP(g_wicFactory, hbm, NULL, WICBitmapUseAlpha, &wicBmp);
    DeleteObject(hbm);
    if (FAILED(hr) || !wicBmp) {
        AppendStatus("[d2d] WIC CreateBitmapFromHBITMAP failed for screenshot");
        return NULL;
    }

    IWICFormatConverter *converter = NULL;
    hr = IWICImagingFactory_CreateFormatConverter(g_wicFactory, &converter);
    if (SUCCEEDED(hr) && converter) {
        hr = IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)wicBmp, &GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    }
    IWICBitmap_Release(wicBmp);
    if (FAILED(hr) || !converter) {
        if (converter) IWICFormatConverter_Release(converter);
        AppendStatus("[d2d] WIC format converter failed for screenshot");
        return NULL;
    }

    ID2D1Bitmap *bmp = NULL;
    hr = ID2D1RenderTarget_CreateBitmapFromWicBitmap(rt, (IWICBitmapSource *)converter, NULL, &bmp);
    IWICFormatConverter_Release(converter);
    if (FAILED(hr) || !bmp) {
        AppendStatus("[d2d] CreateBitmapFromWicBitmap failed for screenshot");
        return NULL;
    }

    AppendStatus("[d2d] screenshot bridged from GDI+ successfully");
    g_screenshotBitmap = bmp;
    return g_screenshotBitmap;
}

void SV2_D2D_Phase2bPaint(HWND hwnd, ID2D1RenderTarget *rt) {
    if (!SE_g_saveLoaded) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    SV2LayoutMirror L = SV2_GetLayout(rc.right - rc.left, rc.bottom - rc.top);

    /* Screenshot, masked to Image.png's alpha shape. */
    ID2D1Bitmap *shot = SV2_D2D_GetScreenshotBitmap(rt);
    if (shot && SV2_Image.valid) {
        RECT b = SV2_Image.bbox;
        D2D1_RECT_F destRect;
        destRect.left = (FLOAT)SV2_MapX(L, b.left);
        destRect.top = (FLOAT)SV2_MapY(L, b.top);
        destRect.right = destRect.left + (FLOAT)((b.right - b.left) * L.scale);
        destRect.bottom = destRect.top + (FLOAT)((b.bottom - b.top) * L.scale);
        ID2D1RenderTarget_DrawBitmap(rt, shot, &destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
    }

    /* Location pin - cropped to its own bbox region (source rect), not the full-canvas pattern,. */
    if (!g_pinLoadAttempted) {
        g_pinLoadAttempted = TRUE;
        g_pinBitmap = SV2_D2D_LoadBitmapFromResource(rt, "SV2_LOCATION_PIN_ICON");
    }
    if (g_pinBitmap && SV2_LocationPin.valid) {
        RECT pb = SV2_LocationPin.bbox;
        int srcW = pb.right - pb.left, srcH = pb.bottom - pb.top;
        int pw = (int)(srcW * L.scale);
        int ph = (int)(srcH * L.scale);
        int px = SV2_MapX(L, SV2P2B_PIN_CX) - pw / 2;
        int py = SV2_MapY(L, SV2P2B_PIN_CY) - ph / 2;

        D2D1_RECT_F destRect = { (FLOAT)px, (FLOAT)py, (FLOAT)(px + pw), (FLOAT)(py + ph) };
        D2D1_RECT_F srcRect = { (FLOAT)pb.left, (FLOAT)pb.top, (FLOAT)pb.right, (FLOAT)pb.bottom };
        ID2D1RenderTarget_DrawBitmap(rt, g_pinBitmap, &destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &srcRect);
    }

    /* Month/calendar image. */
    int month = SV2_MonthFromSaveInfo();
    if (month >= 1 && month <= 12) {
        int mi = month - 1;
        if (!g_monthLoadAttempted[mi]) {
            g_monthLoadAttempted[mi] = TRUE;
            g_monthBitmaps[mi] = SV2_D2D_LoadBitmapFromResource(rt, g_monthResNames[mi]);
        }
        if (g_monthBitmaps[mi]) {
            D2D1_RECT_F destRect;
            destRect.left = (FLOAT)SV2_MapX(L, 0);
            destRect.top = (FLOAT)SV2_MapY(L, 0);
            destRect.right = (FLOAT)SV2_MapX(L, 1918);
            destRect.bottom = (FLOAT)SV2_MapY(L, 1008);
            ID2D1RenderTarget_DrawBitmap(rt, g_monthBitmaps[mi], &destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
        }
    }
}
