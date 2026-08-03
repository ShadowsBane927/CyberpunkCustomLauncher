/* Direct2D renderer for the main menu screen. */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <d2d1.h>
#include <wincodec.h>

/* ---- mirrors of main-source layout/image types. */
typedef struct { int x, y, w, h; double scale; } MenuLayoutMirror;
extern MenuLayoutMirror MENU_GetLayout(int clientW, int clientH);

typedef struct { void *img; RECT bbox; int valid; } MenuImgMirror;

extern MenuImgMirror gMB_Background, gMB_City, gMB_Moon, gMB_DavidMemoir;
extern MenuImgMirror gMB_Button[10];
extern MenuImgMirror gMB_ButtonInv[10];
extern MenuImgMirror gMB_Check4, gMB_Checked4, gMB_Check5, gMB_Checked5, gMB_Check8, gMB_Checked8;
extern MenuImgMirror gMB_Settings;
extern MenuImgMirror gMB_Title, gMB_TitleInv;

#define MENU_MAX_CYCLE_IMAGES 32
extern MenuImgMirror gMB_CycleNormal[MENU_MAX_CYCLE_IMAGES];
extern MenuImgMirror gMB_CycleInverted[MENU_MAX_CYCLE_IMAGES];
extern int gMB_CycleCount;
extern int gMB_CycleIndex;
extern BOOL gMB_CycleShowInverted;

extern int gMB_HoveredButton;
extern BOOL gMB_SettingsVisible;
extern BOOL gMB_Check4On, gMB_Check5On, gMB_Check8On;

/* Title state - only the two static states (NORMAL/INVERTED) are handled here. */
#define MENU_TITLE_NORMAL       0
#define MENU_TITLE_PLAYING_N2I  1
#define MENU_TITLE_INVERTED     2
#define MENU_TITLE_PLAYING_I2N  3
extern int gMB_TitleState;

/* Mirrors MenuGif exactly (field-for-field). */
typedef struct {
    void *img;
    UINT frameCount;
    UINT *delaysMs;
    void **cachedFrames;
    int cacheW, cacheH;
    UINT currentFrame;
    BOOL playing;
    DWORD lastTickMs;
} MenuGifMirror;
extern MenuGifMirror gMB_GifTitleN2I, gMB_GifTitleI2N, gMB_GifLaunch;
extern BOOL gMB_LaunchGifPlaying;

/* ---- GDI+ bridge. */
typedef int GpStatus;
extern GpStatus WINAPI GdipCreateHBITMAPFromBitmap(void *bitmap, HBITMAP *hbmReturn, UINT32 background);

static IWICImagingFactory *g_wicFactory = NULL;
static BOOL g_comInitialized = FALSE;

static BOOL MENU_D2D_EnsureWIC(void) {
    if (g_wicFactory) return TRUE;
    if (!g_comInitialized) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        g_comInitialized = TRUE;
    }
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&g_wicFactory);
    return SUCCEEDED(hr) && g_wicFactory;
}

/* Bridges one already-GDI+-decoded image into a D2D bitmap, caching the result forever. */
typedef struct {
    ID2D1Bitmap *bitmap;
    BOOL attempted;
} MenuD2DCacheSlot;

static ID2D1Bitmap *MENU_D2D_Bridge(ID2D1RenderTarget *rt, MenuD2DCacheSlot *slot, void *gpImage, const char *logTag) {
    if (slot->attempted) return slot->bitmap; /* NULL if the one-time attempt already failed. */
    slot->attempted = TRUE;
    if (!gpImage) return NULL;
    if (!MENU_D2D_EnsureWIC()) return NULL;

    extern void AppendStatus(const char *);
    HBITMAP hbm = NULL;
    GpStatus st = GdipCreateHBITMAPFromBitmap(gpImage, &hbm, 0x00FFFFFF);
    if (st != 0 || !hbm) {
        char msg[100];
        snprintf(msg, sizeof(msg), "[menud2d] GdipCreateHBITMAPFromBitmap failed for '%s', status=%d", logTag, st);
        AppendStatus(msg);
        return NULL;
    }

    IWICBitmap *wicBmp = NULL;
    HRESULT hr = IWICImagingFactory_CreateBitmapFromHBITMAP(g_wicFactory, hbm, NULL, WICBitmapUseAlpha, &wicBmp);
    DeleteObject(hbm);
    if (FAILED(hr) || !wicBmp) {
        char msg[100];
        snprintf(msg, sizeof(msg), "[menud2d] WIC CreateBitmapFromHBITMAP failed for '%s'", logTag);
        AppendStatus(msg);
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
        char msg[100];
        snprintf(msg, sizeof(msg), "[menud2d] WIC format converter failed for '%s'", logTag);
        AppendStatus(msg);
        return NULL;
    }

    ID2D1Bitmap *bmp = NULL;
    hr = ID2D1RenderTarget_CreateBitmapFromWicBitmap(rt, (IWICBitmapSource *)converter, NULL, &bmp);
    IWICFormatConverter_Release(converter);
    if (FAILED(hr) || !bmp) {
        char msg[100];
        snprintf(msg, sizeof(msg), "[menud2d] CreateBitmapFromWicBitmap failed for '%s'", logTag);
        AppendStatus(msg);
        return NULL;
    }

    slot->bitmap = bmp;
    return slot->bitmap;
}

/* Draws a bridged bitmap at the exact full-canvas rect every menu image uses. */
static void MENU_D2D_DrawFull(ID2D1RenderTarget *rt, ID2D1Bitmap *bmp, MenuLayoutMirror L) {
    if (!bmp) return;
    D2D1_RECT_F dest = { (FLOAT)L.x, (FLOAT)L.y, (FLOAT)(L.x + L.w), (FLOAT)(L.y + L.h) };
    ID2D1RenderTarget_DrawBitmap(rt, bmp, &dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
}

/* Per-frame cache for an animated GIF. */
typedef struct {
    MenuD2DCacheSlot *frameSlots;
    UINT frameCount;
} MenuD2DGifCache;

static ID2D1Bitmap *MENU_D2D_BridgeGifFrame(ID2D1RenderTarget *rt, MenuD2DGifCache *cache, MenuGifMirror *gif, const char *logTag) {
    if (!gif->cachedFrames || gif->frameCount == 0) return NULL;
    if (!cache->frameSlots || cache->frameCount != gif->frameCount) {
        if (cache->frameSlots) free(cache->frameSlots);
        cache->frameSlots = (MenuD2DCacheSlot *)calloc(gif->frameCount, sizeof(MenuD2DCacheSlot));
        cache->frameCount = gif->frameCount;
    }
    if (gif->currentFrame >= gif->frameCount) return NULL;
    return MENU_D2D_Bridge(rt, &cache->frameSlots[gif->currentFrame], gif->cachedFrames[gif->currentFrame], logTag);
}
static MenuD2DCacheSlot s_background, s_city, s_moon, s_davidMemoir;
static MenuD2DCacheSlot s_button[10], s_buttonInv[10];
static MenuD2DCacheSlot s_check4, s_checked4, s_check5, s_checked5, s_check8, s_checked8;
static MenuD2DCacheSlot s_settings, s_title, s_titleInv;
static MenuD2DCacheSlot s_cycleNormal[MENU_MAX_CYCLE_IMAGES], s_cycleInverted[MENU_MAX_CYCLE_IMAGES];
static MenuD2DGifCache s_gifTitleN2I, s_gifTitleI2N, s_gifLaunch;

void MENU_D2D_Paint(HWND hwnd) {
    extern ID2D1RenderTarget *SV2_D2D_BeginFrame(HWND hwnd);
    extern void SV2_D2D_EndFrame(ID2D1RenderTarget *rt);

    ID2D1RenderTarget *rt = SV2_D2D_BeginFrame(hwnd);
    if (!rt) return;

    RECT rc;
    GetClientRect(hwnd, &rc);

    /* Full-screen launch gif completely covers everything underneath. */
    if (gMB_LaunchGifPlaying) {
        ID2D1Bitmap *bmp = MENU_D2D_BridgeGifFrame(rt, &s_gifLaunch, &gMB_GifLaunch, "gifLaunch");
        if (bmp) {
            D2D1_RECT_F dest = { 0.0f, 0.0f, (FLOAT)(rc.right - rc.left), (FLOAT)(rc.bottom - rc.top) };
            ID2D1RenderTarget_DrawBitmap(rt, bmp, &dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
        }
        SV2_D2D_EndFrame(rt);
        return;
    }

    MenuLayoutMirror L = MENU_GetLayout(rc.right - rc.left, rc.bottom - rc.top);

    MENU_D2D_DrawFull(rt, MENU_D2D_Bridge(rt, &s_background, gMB_Background.img, "background"), L);

    if (gMB_CycleCount > 0) {
        int idx = gMB_CycleIndex;
        if (idx >= 0 && idx < MENU_MAX_CYCLE_IMAGES) {
            void *img = gMB_CycleNormal[idx].img;
            MenuD2DCacheSlot *slot = &s_cycleNormal[idx];
            if (gMB_CycleShowInverted && gMB_CycleInverted[idx].valid) {
                img = gMB_CycleInverted[idx].img;
                slot = &s_cycleInverted[idx];
            }
            MENU_D2D_DrawFull(rt, MENU_D2D_Bridge(rt, slot, img, "cycle"), L);
        }
    }

    MENU_D2D_DrawFull(rt, MENU_D2D_Bridge(rt, &s_city, gMB_City.img, "city"), L);
    MENU_D2D_DrawFull(rt, MENU_D2D_Bridge(rt, &s_moon, gMB_Moon.img, "moon"), L);

    for (int i = 1; i <= 9; i++) {
        if (i == 4 || i == 5 || i == 8) continue; /* part of the settings panel, drawn below. */
        void *img = gMB_Button[i].img;
        MenuD2DCacheSlot *slot = &s_button[i];
        if (gMB_HoveredButton == i && gMB_ButtonInv[i].valid) {
            img = gMB_ButtonInv[i].img;
            slot = &s_buttonInv[i];
        }
        MENU_D2D_DrawFull(rt, MENU_D2D_Bridge(rt, slot, img, "button"), L);
    }

    if (gMB_SettingsVisible) {
        MENU_D2D_DrawFull(rt, MENU_D2D_Bridge(rt, &s_button[4], gMB_Button[4].img, "button4"), L);
        MENU_D2D_DrawFull(rt, MENU_D2D_Bridge(rt, &s_button[5], gMB_Button[5].img, "button5"), L);
        MENU_D2D_DrawFull(rt, MENU_D2D_Bridge(rt, &s_button[8], gMB_Button[8].img, "button8"), L);

        MENU_D2D_DrawFull(rt, gMB_Check4On
            ? MENU_D2D_Bridge(rt, &s_checked4, gMB_Checked4.img, "checked4")
            : MENU_D2D_Bridge(rt, &s_check4, gMB_Check4.img, "check4"), L);
        MENU_D2D_DrawFull(rt, gMB_Check5On
            ? MENU_D2D_Bridge(rt, &s_checked5, gMB_Checked5.img, "checked5")
            : MENU_D2D_Bridge(rt, &s_check5, gMB_Check5.img, "check5"), L);
        MENU_D2D_DrawFull(rt, gMB_Check8On
            ? MENU_D2D_Bridge(rt, &s_checked8, gMB_Checked8.img, "checked8")
            : MENU_D2D_Bridge(rt, &s_check8, gMB_Check8.img, "check8"), L);
    }

    MENU_D2D_DrawFull(rt, MENU_D2D_Bridge(rt, &s_settings, gMB_Settings.img, "settings"), L);
    MENU_D2D_DrawFull(rt, MENU_D2D_Bridge(rt, &s_davidMemoir, gMB_DavidMemoir.img, "davidmemoir"), L);

    /* Title: static image, or the active gif frame while animating. */
    if (gMB_TitleState == MENU_TITLE_PLAYING_N2I) {
        MENU_D2D_DrawFull(rt, MENU_D2D_BridgeGifFrame(rt, &s_gifTitleN2I, &gMB_GifTitleN2I, "gifTitleN2I"), L);
    } else if (gMB_TitleState == MENU_TITLE_PLAYING_I2N) {
        MENU_D2D_DrawFull(rt, MENU_D2D_BridgeGifFrame(rt, &s_gifTitleI2N, &gMB_GifTitleI2N, "gifTitleI2N"), L);
    } else if (gMB_TitleState == MENU_TITLE_INVERTED) {
        MENU_D2D_DrawFull(rt, MENU_D2D_Bridge(rt, &s_titleInv, gMB_TitleInv.img, "titleinv"), L);
    } else {
        MENU_D2D_DrawFull(rt, MENU_D2D_Bridge(rt, &s_title, gMB_Title.img, "title"), L);
    }

    SV2_D2D_EndFrame(rt);
}
