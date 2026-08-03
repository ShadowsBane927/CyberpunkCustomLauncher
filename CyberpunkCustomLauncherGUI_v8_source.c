/* * CyberpunkCustomLauncherGUI_v4. */

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <gdiplus.h>
#include <math.h>
#include <ctype.h>
#include "saveengine.h"

#ifdef __cplusplus
using namespace Gdiplus;
#endif

#define GAME_ROOT "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Cyberpunk 2077"

#define ID_CHECK_DEPLOY     101
#define ID_CHECK_PRIORITY   102
#define ID_BUTTON_LAUNCH    104
#define ID_STATUS_TEXT      105
#define ID_PATH_BOX         106
#define ID_BROWSE_BTN       107
#define ID_BUTTON_CONFLICTS 108
#define ID_BUTTON_EDITSAVES 109
#define ID_CONFLICT_ICON    110
#define ID_BUTTON_BACKTOLAUNCHER 111

#define TIMER_PRIORITY_WATCH 1

#define MODE_LAUNCHER    0
#define MODE_SAVEEDITOR  1
int gViewMode = MODE_LAUNCHER;
BOOL gSaveEditorActivated = FALSE;
HWND hButtonBack = NULL;
/* Promoted from WM_CREATE locals so SwitchToSaveEditor/SwitchToLauncher can show/hide them. */
HWND hModsLabel = NULL, hBrowseBtn = NULL, hConflictsBtn = NULL;

HINSTANCE hInstanceGlobal;

HWND hCheckDeploy, hCheckPriority, hButtonLaunch, hStatusText, hPathBox, hMainWindow;
HWND hButtonEditSaves, hConflictIcon, hTooltip;

DWORD gGamePID = 0;
BOOL gDynamicPriorityEnabled = FALSE;
BOOL gLastWasFocused = FALSE;
BOOL gHaveAppliedInitialPriority = FALSE;
BOOL gConflictsFound = FALSE;
char gConflictSummary[4096] = "";

void AppendStatus(const char *msg) {
    int len = GetWindowTextLengthA(hStatusText);
    SendMessageA(hStatusText, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(hStatusText, EM_REPLACESEL, 0, (LPARAM)msg);
    SendMessageA(hStatusText, EM_REPLACESEL, 0, (LPARAM)"\r\n");

    /* Also write to a log file next to the exe, so a debug session can be sent as a file. */
    static char logPath[MAX_PATH] = "";
    if (logPath[0] == '\0') {
        GetModuleFileNameA(NULL, logPath, MAX_PATH);
        char *lastSlash = strrchr(logPath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';
        strncat(logPath, "debug_log.txt", MAX_PATH - strlen(logPath) - 1);
    }
    FILE *f = fopen(logPath, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        fclose(f);
    }
}

/* ---------------------------------------------------------------------- */
/* Native .archive conflict scanner. */
/* ---------------------------------------------------------------------- */

typedef struct {
    unsigned long long hash;
    char archiveName[MAX_PATH];
} HashEntry;

/* Reads exactly one .archive file's header + index + file entries, and appends each entry's. */
int ReadArchiveEntries(const char *fullPath, const char *displayName,
                        HashEntry **entries, int *count, int *capacity) {
    FILE *f = fopen(fullPath, "rb");
    if (f == NULL) return -1;

    unsigned char headerBuf[40];
    if (fread(headerBuf, 1, 40, f) != 40) { fclose(f); return -1; }

    unsigned int magic = *(unsigned int *)(headerBuf + 0);
    if (magic != 1380009042u) { /* not a valid .archive file. */
        fclose(f);
        return -1;
    }

    unsigned long long indexPosition = *(unsigned long long *)(headerBuf + 8);

    if (fseek(f, (long)indexPosition, SEEK_SET) != 0) { fclose(f); return -1; }

    unsigned char indexBuf[28];
    if (fread(indexBuf, 1, 28, f) != 28) { fclose(f); return -1; }

    unsigned int fileEntryCount = *(unsigned int *)(indexBuf + 16);

    for (unsigned int i = 0; i < fileEntryCount; i++) {
        unsigned char entryBuf[56];
        if (fread(entryBuf, 1, 56, f) != 56) break; /* truncated, stop reading this archive. */

        unsigned long long nameHash = *(unsigned long long *)(entryBuf + 0);

        if (*count >= *capacity) {
            *capacity *= 2;
            *entries = (HashEntry *)realloc(*entries, (size_t)(*capacity) * sizeof(HashEntry));
        }

        (*entries)[*count].hash = nameHash;
        strncpy((*entries)[*count].archiveName, displayName, MAX_PATH - 1);
        (*entries)[*count].archiveName[MAX_PATH - 1] = '\0';
        (*count)++;
    }

    fclose(f);
    return 0;
}

int CompareHashEntries(const void *a, const void *b) {
    unsigned long long ha = ((const HashEntry *)a)->hash;
    unsigned long long hb = ((const HashEntry *)b)->hash;
    if (ha < hb) return -1;
    if (ha > hb) return 1;
    return 0;
}

typedef struct {
    char archiveA[MAX_PATH];
    char archiveB[MAX_PATH];
    int sharedCount;
} ConflictPair;

/* Finds an existing pair entry (order-independent) or adds a new one. */
void RecordConflictPair(ConflictPair **pairs, int *pairCount, int *pairCapacity,
                         const char *a, const char *b) {
    for (int i = 0; i < *pairCount; i++) {
        if ((strcmp((*pairs)[i].archiveA, a) == 0 && strcmp((*pairs)[i].archiveB, b) == 0) ||
            (strcmp((*pairs)[i].archiveA, b) == 0 && strcmp((*pairs)[i].archiveB, a) == 0)) {
            (*pairs)[i].sharedCount++;
            return;
        }
    }
    if (*pairCount >= *pairCapacity) {
        *pairCapacity *= 2;
        *pairs = (ConflictPair *)realloc(*pairs, (size_t)(*pairCapacity) * sizeof(ConflictPair));
    }
    strncpy((*pairs)[*pairCount].archiveA, a, MAX_PATH - 1);
    strncpy((*pairs)[*pairCount].archiveB, b, MAX_PATH - 1);
    (*pairs)[*pairCount].sharedCount = 1;
    (*pairCount)++;
}

void UpdateConflictIcon(void) {
    if (hConflictIcon != NULL) {
        ShowWindow(hConflictIcon, gConflictsFound ? SW_SHOW : SW_HIDE);
    }
    if (hTooltip != NULL) {
        TOOLINFOA ti;
        ZeroMemory(&ti, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.hwnd = hMainWindow;
        ti.uId = (UINT_PTR)hConflictIcon;
        ti.lpszText = gConflictSummary;
        SendMessageA(hTooltip, TTM_UPDATETIPTEXTA, 0, (LPARAM)&ti);
    }
}

void RunNativeConflictScan(void) {
    char folderPath[MAX_PATH];
    GetWindowTextA(hPathBox, folderPath, sizeof(folderPath));

    DWORD attrs = GetFileAttributesA(folderPath);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        AppendStatus("Archives folder path doesn't exist - check the path above.");
        return;
    }

    AppendStatus("--- Scanning for mod conflicts ---");

    char searchPattern[MAX_PATH];
    snprintf(searchPattern, sizeof(searchPattern), "%s\\*.archive", folderPath);

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPattern, &findData);

    if (hFind == INVALID_HANDLE_VALUE) {
        AppendStatus("No .archive files found in that folder.");
        gConflictsFound = FALSE;
        UpdateConflictIcon();
        return;
    }

    int capacity = 1024;
    int count = 0;
    HashEntry *entries = (HashEntry *)malloc((size_t)capacity * sizeof(HashEntry));

    int archiveCount = 0;
    int skippedCount = 0;

    do {
        char fullPath[MAX_PATH];
        snprintf(fullPath, sizeof(fullPath), "%s\\%s", folderPath, findData.cFileName);

        int result = ReadArchiveEntries(fullPath, findData.cFileName, &entries, &count, &capacity);
        if (result == 0) {
            archiveCount++;
        } else {
            skippedCount++;
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);

    if (archiveCount == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Found .archive files but couldn't read any (skipped %d).", skippedCount);
        AppendStatus(msg);
        free(entries);
        gConflictsFound = FALSE;
        UpdateConflictIcon();
        return;
    }

    qsort(entries, (size_t)count, sizeof(HashEntry), CompareHashEntries);

    int pairCapacity = 32;
    int pairCount = 0;
    ConflictPair *pairs = (ConflictPair *)malloc((size_t)pairCapacity * sizeof(ConflictPair));

    int conflictGroups = 0;
    int i = 0;
    while (i < count) {
        int j = i + 1;
        while (j < count && entries[j].hash == entries[i].hash) j++;

        BOOL isConflict = FALSE;
        for (int k = i + 1; k < j; k++) {
            if (strcmp(entries[k].archiveName, entries[i].archiveName) != 0) {
                isConflict = TRUE;
                break;
            }
        }

        if (isConflict) {
            conflictGroups++;
            char line[600];
            snprintf(line, sizeof(line), "Conflict (resource 0x%016llX):", entries[i].hash);
            AppendStatus(line);

            char lastPrinted[MAX_PATH] = "";
            for (int k = i; k < j; k++) {
                if (strcmp(lastPrinted, entries[k].archiveName) != 0) {
                    char subline[300];
                    snprintf(subline, sizeof(subline), "    - %s", entries[k].archiveName);
                    AppendStatus(subline);
                    strncpy(lastPrinted, entries[k].archiveName, MAX_PATH - 1);
                }
            }

            /* Record every distinct pair within this conflict group for the clean tooltip summary. */
            for (int a = i; a < j; a++) {
                for (int b = a + 1; b < j; b++) {
                    if (strcmp(entries[a].archiveName, entries[b].archiveName) != 0) {
                        RecordConflictPair(&pairs, &pairCount, &pairCapacity,
                            entries[a].archiveName, entries[b].archiveName);
                    }
                }
            }
        }

        i = j;
    }

    free(entries);

    char summary[256];
    snprintf(summary, sizeof(summary),
        "--- Scan complete: %d archive(s) checked, %d skipped, %d conflict(s) found ---",
        archiveCount, skippedCount, conflictGroups);
    AppendStatus(summary);

    /* Build the clean, human-readable tooltip text. */
    gConflictsFound = (pairCount > 0);
    gConflictSummary[0] = '\0';
    for (int p = 0; p < pairCount; p++) {
        char line[600];
        snprintf(line, sizeof(line), "%s conflicts with %s (%d file%s)\n",
            pairs[p].archiveA, pairs[p].archiveB,
            pairs[p].sharedCount, pairs[p].sharedCount == 1 ? "" : "s");
        strncat(gConflictSummary, line, sizeof(gConflictSummary) - strlen(gConflictSummary) - 1);
    }
    free(pairs);

    UpdateConflictIcon();
}

void BrowseForArchivesFolder(void) {
    BROWSEINFOA bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = hMainWindow;
    bi.lpszTitle = "Select your Cyberpunk 2077 archive\\pc\\mod folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl != NULL) {
        char path[MAX_PATH];
        if (SHGetPathFromIDListA(pidl, path)) {
            SetWindowTextA(hPathBox, path);
        }
        CoTaskMemFree(pidl);
    }
}

/* ---------------------------------------------------------------------- */
/* REDmod deploy + game launch (unchanged from before) */
/* ---------------------------------------------------------------------- */

int RunRedModDeploy(void) {
    AppendStatus("Deploying REDmod-format mods...");

    char cmdLine[1024];
    snprintf(cmdLine, sizeof(cmdLine),
        "cmd /c cd /d \"%s\\tools\\redmod\\bin\" && redMod.exe deploy -root=\"%s\"",
        GAME_ROOT, GAME_ROOT);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (!ok) {
        AppendStatus("Failed to start redMod.exe - check GAME_ROOT path.");
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Deploy failed (exit code %lu). Check REDmodLog.txt.", exitCode);
        AppendStatus(msg);
        return 1;
    }

    AppendStatus("Deploy complete.");
    return 0;
}

void LaunchGame(int useModded, int dynamicPriority) {
    char exePath[512];
    snprintf(exePath, sizeof(exePath), "%s\\bin\\x64\\Cyberpunk2077.exe", GAME_ROOT);

    char args[64];
    if (useModded) {
        snprintf(args, sizeof(args), "-modded --launcher-skip");
    } else {
        snprintf(args, sizeof(args), "--launcher-skip");
    }

    char fullCommandLine[600];
    snprintf(fullCommandLine, sizeof(fullCommandLine), "\"%s\" %s", exePath, args);

    char workingDir[512];
    snprintf(workingDir, sizeof(workingDir), "%s\\bin\\x64", GAME_ROOT);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    AppendStatus("Launching Cyberpunk 2077...");

    BOOL success = CreateProcessA(
        exePath, fullCommandLine, NULL, NULL, FALSE,
        0, NULL, workingDir, &si, &pi
    );

    if (!success) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to launch - Windows error %lu. Check GAME_ROOT path.", GetLastError());
        AppendStatus(msg);
        return;
    }

    gGamePID = pi.dwProcessId;
    gHaveAppliedInitialPriority = FALSE;

    if (dynamicPriority) {
        gDynamicPriorityEnabled = TRUE;
        SetTimer(hMainWindow, TIMER_PRIORITY_WATCH, 1500, NULL);
        AppendStatus("Dynamic priority watching enabled - keep this window open while playing.");
    } else {
        gDynamicPriorityEnabled = FALSE;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    AppendStatus("Game launched.");
}

void CheckAndApplyDynamicPriority(void) {
    if (gGamePID == 0) return;

    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg != NULL) {
        GetWindowThreadProcessId(fg, &fgPid);
    }

    BOOL isFocused = (fgPid == gGamePID);

    if (isFocused == gLastWasFocused && gHaveAppliedInitialPriority) {
        return;
    }

    HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, gGamePID);
    if (hProc == NULL) {
        KillTimer(hMainWindow, TIMER_PRIORITY_WATCH);
        gDynamicPriorityEnabled = FALSE;
        gGamePID = 0;
        AppendStatus("Game process no longer found - priority watching stopped.");
        return;
    }

    if (isFocused) {
        SetPriorityClass(hProc, HIGH_PRIORITY_CLASS);
        if (!gHaveAppliedInitialPriority || !gLastWasFocused) {
            AppendStatus("Game focused - priority set to High.");
        }
    } else {
        SetPriorityClass(hProc, ABOVE_NORMAL_PRIORITY_CLASS);
        if (!gHaveAppliedInitialPriority || gLastWasFocused) {
            AppendStatus("Game unfocused - priority set to Above Normal.");
        }
    }

    CloseHandle(hProc);
    gLastWasFocused = isFocused;
    gHaveAppliedInitialPriority = TRUE;
}

/* ------------------------------------------------------------------ */
/* Embedded Save Editor (merged from SaveEditorGUI.c) */
/* All symbols below are prefixed SE_ to avoid clashing with the. */
/* launcher's own globals/functions of the same conceptual purpose. */
/* ------------------------------------------------------------------ */

#define SE_ID_LOAD_BTN     501
#define SE_ID_SAVE_BTN     502
#define SE_ID_PATH_BOX     503
#define SE_ID_HELP_BTN     504
#define SE_ID_STATUS_TEXT  505
#define SE_ID_INFO_TEXT    506
#define SE_TIMER_STARTUP_RELAYOUT 900
#define SE_ID_FIRST_EDIT   1000

HWND SE_hMainWnd, SE_hPathBox, SE_hStatusText, SE_hSaveBtn, SE_hInfoText, SE_hHelpBtn, SE_hTooltip, SE_hSaveLabel, SE_hLoadBtn;
HWND SE_hLevelEditBox = NULL, SE_hCredEditBox = NULL;
HWND SE_hEditBoxes[MAX_PROFICIENCIES];
HWND SE_hLabels[MAX_PROFICIENCIES];
HINSTANCE SE_hInst;

SaveFile SE_g_save;
int SE_g_saveLoaded = 0;

ULONG_PTR SE_g_gdiplusToken;
GpImage *SE_g_screenshotImage = NULL;
GpImage *SE_g_levelTriImage = NULL;
GpImage *SE_g_credTriImage = NULL;
GpImage *SE_g_bodyDecoImage = NULL;
RECT SE_g_imageRect = {620, 12, 860, 152};

int SE_g_currentClientWidth = 900;
int SE_g_currentClientHeight = 700;

HFONT SE_g_appFont = NULL; /* Bookman Old Style, used for all controls. */

/* Loads a PNG embedded as an RCDATA resource into a GDI+ image via memory stream. */
GpImage *SE_LoadImageFromResource(const char *resourceName) {
    HRSRC hRes = FindResourceA(NULL, resourceName, RT_RCDATA);
    if (!hRes) return NULL;
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) return NULL;
    DWORD size = SizeofResource(NULL, hRes);
    void *ptr = LockResource(hData);
    if (!ptr || size == 0) return NULL;

    HGLOBAL hBuf = GlobalAlloc(GMEM_MOVEABLE, size);
    void *buf = GlobalLock(hBuf);
    memcpy(buf, ptr, size);
    GlobalUnlock(hBuf);

    IStream *stream = NULL;
    if (CreateStreamOnHGlobal(hBuf, TRUE, &stream) != S_OK) return NULL;

    GpImage *img = NULL;
    GdipLoadImageFromStream(stream, &img);
    stream->lpVtbl->Release(stream);
    return img;
}

int SE_g_gridBottomY = 320; /* updated after each SE_BuildProficiencyFields call. */

/* Layout bands - computed once here so every section has a guaranteed, non-overlapping home. */
#define PATHROW_Y 22
#define INFOBOX_GAP 46
#define INFOBOX_H 58
#define BUTTONROW_GAP 8
#define BUTTONROW_H 49
#define GRID_GAP 10
#define STATUS_GAP 20
#define PATHROW_H 30 /* fixed - matches SE_RepositionPathRow's rowH. */

int SE_GetInfoBoxY(void) {
    return PATHROW_Y + PATHROW_H + INFOBOX_GAP;
}
#define SAVEBTN_STAGGER_GAP 26 /* vertical gap between Load Save and the staggered Save Changes below it. */
int SE_GetButtonRowY(void) { return SE_GetInfoBoxY() + INFOBOX_H + BUTTONROW_GAP; }
int SE_GetGridStartY(void) { return SE_GetButtonRowY() + BUTTONROW_H + GRID_GAP; }

/* SE_g_currentClientWidth/Height reflect true physical pixels. */
RECT SE_GetBodyDecoRect(void) {
    RECT r;
    int top = SE_GetGridStartY();

    int availWidth = SE_g_currentClientWidth - 40;
    /* The status log overlaps the image's own blank bottom-left corner (see SE_RepositionStatusBox) */
    int availHeight = SE_g_currentClientHeight - top - 20;
    if (availWidth < 400) availWidth = 400;
    if (availHeight < 300) availHeight = 300;

    r.left = 20;
    r.top = top;
    r.right = r.left + availWidth;
    r.bottom = r.top + availHeight;
    return r;
}

/* Uniform (non-distorting) scale-to-cover for the 1918x1008 artwork within. */
typedef struct { RECT vis; RECT art; double cropFyStart; double cropFxStart; } DecoXform;

DecoXform SE_GetDecoXform(void) {
    DecoXform t;
    t.vis = SE_GetBodyDecoRect();
    int availWidth = t.vis.right - t.vis.left;
    int availHeight = t.vis.bottom - t.vis.top;

    double scaleW = availWidth / 1918.0;
    double scaleH = availHeight / 1008.0;
    double scale = (scaleW > scaleH) ? scaleW : scaleH; /* cover: larger wins. */

    /* Never let the top crop reach fy=0.369, where the topmost skill icon graphic. */
    #define MAX_CROP_FY (372.0 / 1008.0)
    double fullH = 1008.0 * scale;
    if (fullH > availHeight) {
        double cropFy = (fullH - availHeight) / fullH;
        if (cropFy > MAX_CROP_FY) {
            double cappedFullH = availHeight / (1.0 - MAX_CROP_FY);
            scale = cappedFullH / 1008.0;
        }
    }

    double fullW = 1918.0 * scale;
    fullH = 1008.0 * scale;
    t.cropFyStart = (fullH > availHeight) ? (fullH - availHeight) / fullH : 0.0;

    if (fullW >= availWidth) {
        t.cropFxStart = (fullW - availWidth) / (2.0 * fullW);
        t.art = t.vis;
    } else {
        /* Letterbox fallback (only when the crop cap above kicked in): center the true-size art. */
        t.cropFxStart = 0.0;
        int destW = (int)fullW;
        int destX = t.vis.left + (availWidth - destW) / 2;
        t.art.left = destX;
        t.art.right = destX + destW;
        t.art.top = t.vis.top;
        t.art.bottom = t.vis.bottom;
    }
    return t;
}

int SE_DecoMapX(DecoXform t, double fx) {
    double denom = 1.0 - 2.0 * t.cropFxStart;
    double vfx = (fx - t.cropFxStart) / denom;
    return t.art.left + (int)(vfx * (t.art.right - t.art.left));
}

int SE_DecoMapY(DecoXform t, double fy) {
    double denom = 1.0 - t.cropFyStart;
    double vfy = (fy - t.cropFyStart) / denom;
    return t.art.top + (int)(vfy * (t.art.bottom - t.art.top));
}

/* Screenshot width/position measured directly from the reference mockup (Alllayers.png): */
RECT SE_GetScreenshotRect(DecoXform xform) {
    RECT r;
    r.left = SE_DecoMapX(xform, 0.5845);
    r.right = SE_DecoMapX(xform, 0.8848);
    int w = r.right - r.left;
    int h = (int)(w / 1.714);

    int intelTopY = SE_DecoMapY(xform, 376.0 / 1008.0);
    int centerY = (0 + intelTopY) / 2;
    r.top = centerY - h / 2;
    r.bottom = centerY + h / 2;
    return r;
}

/* Fraction of the deco image's height below which the left-hand skill grid. */
#define LOG_TOP_FRACTION 0.60
#define LOG_WIDTH_FRACTION 0.47

void SE_RepositionStatusBox(void) {
    if (!SE_hStatusText) return;
    RECT br = SE_GetBodyDecoRect();
    int decoW = br.right - br.left;
    int decoH = br.bottom - br.top;

    int x = br.left;
    int y = br.top + (int)(LOG_TOP_FRACTION * decoH);
    int w = (int)(LOG_WIDTH_FRACTION * decoW);
    int h = br.bottom - y;

    if (w < 250) w = 250;
    if (h < 100) h = 100;

    MoveWindow(SE_hStatusText, x, y, w, h, TRUE);
    SE_g_gridBottomY = br.bottom;

    /* "Back to Launcher" sits right above (touching) the log box's top edge, out of the way of. */
    if (hButtonBack) {
        int backH = 28, backW = 180;
        MoveWindow(hButtonBack, x, y - backH, backW, backH, TRUE);
    }
}

void SE_RepositionPathRow(void) {
    if (!SE_hSaveLabel || !SE_hPathBox || !SE_hHelpBtn) return;

    int availWidth = SE_g_currentClientWidth - 40;
    int rightEdge = 20 + (int)(LOG_WIDTH_FRACTION * availWidth); /* matches log box's right edge. */

    /* Label and help button stay at their original fixed size/position. */
    int rowTop = 11, rowH = 30;
    int labelX = 20, labelW = 70, labelH = 26;
    int labelY = rowTop + (rowH - labelH) / 2;
    int btnY = rowTop, btnW = 30, btnH = 30; /* square. */
    int pathY = rowTop, pathH = rowH;

    int btnX = rightEdge - btnW;
    int pathX = labelX + labelW + 10;
    int pathW = btnX - 10 - pathX;
    if (pathW < 100) pathW = 100;

    MoveWindow(SE_hSaveLabel, labelX, labelY, labelW, labelH, TRUE);
    MoveWindow(SE_hPathBox, pathX, pathY, pathW, pathH, TRUE);
    MoveWindow(SE_hHelpBtn, btnX, btnY, btnW, btnH, TRUE);
}



/* Converts "TechnicalAbilitySkill" -> "TECHNICAL ABILITY SKILL", "StreetCred" -> "STREET. */
void SE_PrettifyName(const char *raw, char *out, size_t outSize) {
    size_t o = 0;
    for (size_t i = 0; raw[i] != '\0' && o < outSize - 2; i++) {
        if (i > 0 && raw[i] >= 'A' && raw[i] <= 'Z') {
            out[o++] = ' ';
        }
        out[o++] = (char)toupper((unsigned char)raw[i]);
    }
    out[o] = '\0';
}

void SE_AppendStatus(const char *msg) {
    int len = GetWindowTextLengthA(SE_hStatusText);
    SendMessageA(SE_hStatusText, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(SE_hStatusText, EM_REPLACESEL, 0, (LPARAM)msg);
    SendMessageA(SE_hStatusText, EM_REPLACESEL, 0, (LPARAM)"\r\n");
}

/* Extremely simple JSON field extraction: finds "fieldName":"value" or "fieldName":number. */
/* District and subdistrict code lookup, derived and verified against the real. */
typedef struct { const char *code; const char *name; } CodeName;

static const CodeName g_districtCodes[] = {
    {"bad", "Badlands"}, {"cct", "City Center"}, {"hey", "Heywood"},
    {"pac", "Pacifica"}, {"std", "Santo Domingo"}, {"wat", "Watson"},
    {"wbr", "Westbrook"},
};
static const CodeName g_subdistrictCodes[] = {
    {"cpz", "Corpo Plaza"}, {"dtn", "Downtown"},
    {"gle", "Glen"}, {"rey", "Vista Del Rey"}, {"spr", "Wellsprings"},
    {"wwd", "West Wind Estate"}, {"cvi", "Coast View"},
    {"arr", "Arroyo"}, {"rcr", "Rancho Coronado"},
    {"awf", "Arasaka Waterfront"}, {"kab", "Kabuki"}, {"lch", "Little China"}, {"nid", "Northside Industrial"},
    {"hil", "Charter Hill"}, {"jpn", "Japan Town"}, {"nok", "North Oaks"},
};

const char *LookupCode(const CodeName *table, int count, const char *code) {
    for (int i = 0; i < count; i++) {
        if (_stricmp(table[i].code, code) == 0) return table[i].name;
    }
    return NULL;
}

/* Parses a debugString like "ce_wbr_hil_02" into "Westbrook. */
void SE_DeriveLocationFromDebugString(const char *debugString, char *out, size_t outSize) {
    char buf[128];
    strncpy(buf, debugString, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *parts[8];
    int partCount = 0;
    char *tok = strtok(buf, "_");
    while (tok && partCount < 8) {
        parts[partCount++] = tok;
        tok = strtok(NULL, "_");
    }

    if (partCount >= 3) {
        const char *district = LookupCode(g_districtCodes, sizeof(g_districtCodes) / sizeof(CodeName), parts[1]);
        const char *subdistrict = LookupCode(g_subdistrictCodes, sizeof(g_subdistrictCodes) / sizeof(CodeName), parts[2]);
        if (district && subdistrict) {
            snprintf(out, outSize, "%s - %s", district, subdistrict);
            return;
        }
        if (district) {
            snprintf(out, outSize, "%s", district);
            return;
        }
    }
    snprintf(out, outSize, "Unknown area");
}

/* Reformats "HH:MM:SS, D.MM.YYYY" into "H:MM am/pm, Month Dth, YYYY". */
void SE_FormatTimestamp(const char *raw, char *out, size_t outSize) {
    int hh, mm, ss, day, mon, year;
    if (sscanf(raw, "%d:%d:%d, %d.%d.%d", &hh, &mm, &ss, &day, &mon, &year) != 6) {
        strncpy(out, raw, outSize - 1);
        out[outSize - 1] = '\0';
        return;
    }
    (void)ss;

    static const char *months[] = { "January","February","March","April","May","June",
        "July","August","September","October","November","December" };
    const char *monthName = (mon >= 1 && mon <= 12) ? months[mon - 1] : "?";

    const char *suffix = "th";
    if (day % 10 == 1 && day != 11) suffix = "st";
    else if (day % 10 == 2 && day != 12) suffix = "nd";
    else if (day % 10 == 3 && day != 13) suffix = "rd";

    int hour12 = hh % 12;
    if (hour12 == 0) hour12 = 12;
    const char *ampm = (hh < 12) ? "am" : "pm";

    snprintf(out, outSize, "%d:%02d %s, %s %d%s %d", hour12, mm, ampm, monthName, day, suffix, year);
}

/* Same parse, but split into a date-only and time-only string. */
void SE_FormatDateTimeParts(const char *raw, char *dateOut, size_t dateSize, char *timeOut, size_t timeSize) {
    int hh, mm, ss, day, mon, year;
    if (sscanf(raw, "%d:%d:%d, %d.%d.%d", &hh, &mm, &ss, &day, &mon, &year) != 6) {
        if (dateOut) strncpy(dateOut, raw, dateSize - 1), dateOut[dateSize-1] = '\0';
        if (timeOut) timeOut[0] = '\0';
        return;
    }
    (void)ss;

    static const char *months[] = { "January","February","March","April","May","June",
        "July","August","September","October","November","December" };
    const char *monthName = (mon >= 1 && mon <= 12) ? months[mon - 1] : "?";

    const char *suffix = "th";
    if (day % 10 == 1 && day != 11) suffix = "st";
    else if (day % 10 == 2 && day != 12) suffix = "nd";
    else if (day % 10 == 3 && day != 13) suffix = "rd";

    int hour12 = hh % 12;
    if (hour12 == 0) hour12 = 12;
    const char *ampm = (hh < 12) ? "am" : "pm";

    if (dateOut) snprintf(dateOut, dateSize, "%s %d%s %d", monthName, day, suffix, year);
    if (timeOut) snprintf(timeOut, timeSize, "%d:%02d %s", hour12, mm, ampm);
}

/* Extracts the last segment of a quest path like. */
void SE_PrettifyQuestPath(const char *rawPath, char *out, size_t outSize) {
    const char *lastSlash = strrchr(rawPath, '/');
    const char *segment = lastSlash ? lastSlash + 1 : rawPath;

    size_t o = 0;
    int atWordStart = 1;
    for (size_t i = 0; segment[i] != '\0' && o < outSize - 1; i++) {
        char c = segment[i];
        if (c == '_') {
            out[o++] = ' ';
            atWordStart = 1;
        } else {
            out[o++] = atWordStart ? (char)toupper((unsigned char)c) : c;
            atWordStart = 0;
        }
    }
    out[o] = '\0';
}

int SE_ExtractJsonNumberField(const char *json, const char *fieldName, double *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", fieldName);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;
    *out = atof(p);
    return 1;
}

int SE_ExtractJsonStringField(const char *json, const char *fieldName, char *out, size_t outSize) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", fieldName);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if (len >= outSize) len = outSize - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

/* Finds the first file in dir matching one of the given extensions (case-insensitive),. */
void SE_FindFirstFileWithExt(const char *dir, const char **extensions, int extCount, char *outPath, size_t outSize) {
    outPath[0] = '\0';
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char *dot = strrchr(fd.cFileName, '.');
        if (!dot) continue;
        for (int i = 0; i < extCount; i++) {
            if (_stricmp(dot, extensions[i]) == 0) {
                snprintf(outPath, outSize, "%s\\%s", dir, fd.cFileName);
                FindClose(h);
                return;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void SE_GetDirectoryOf(const char *filePath, char *outDir, size_t outSize) {
    strncpy(outDir, filePath, outSize - 1);
    outDir[outSize - 1] = '\0';
    char *lastSlash = strrchr(outDir, '\\');
    if (lastSlash) *lastSlash = '\0';
}

char SV2_g_saveName[128] = "";
char SV2_g_lifePath[64] = "";
char SV2_g_timestamp[80] = "";
char SV2_g_dateOnly[40] = "";
char SV2_g_timeOnly[24] = "";
char SV2_g_quest[256] = "";
char SV2_g_location[80] = "";
char SV2_g_folderName[MAX_PATH] = "";
int SV2_g_month = 0; /* 1-12, parsed from the timestamp. */

void SE_LoadScreenshotAndInfo(const char *savePath) {
    char dir[MAX_PATH];
    SE_GetDirectoryOf(savePath, dir, sizeof(dir));

    /* Free any previous image. */
    if (SE_g_screenshotImage) {
        GdipDisposeImage(SE_g_screenshotImage);
        SE_g_screenshotImage = NULL;
    }

    /* Find and load screenshot. */
    const char *imgExts[] = { ".png", ".jpg", ".jpeg", ".bmp" };
    char imgPath[MAX_PATH];
    SE_FindFirstFileWithExt(dir, imgExts, 4, imgPath, sizeof(imgPath));

    if (imgPath[0] != '\0') {
        WCHAR wPath[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, imgPath, -1, wPath, MAX_PATH);
        GdipLoadImageFromFile(wPath, &SE_g_screenshotImage);
    }

    /* Find and parse metadata JSON. */
    const char *jsonExts[] = { ".json" };
    char jsonPath[MAX_PATH];
    SE_FindFirstFileWithExt(dir, jsonExts, 1, jsonPath, sizeof(jsonPath));

    char infoLine[600];
    char folderName[MAX_PATH];
    char *lastSlash = strrchr(dir, '\\');
    strncpy(folderName, lastSlash ? lastSlash + 1 : dir, sizeof(folderName) - 1);
    folderName[sizeof(folderName) - 1] = '\0';
    strncpy(SV2_g_folderName, folderName, sizeof(SV2_g_folderName) - 1);

    /* Reset per-load so a save with no JSON doesn't show stale text from a previously-loaded. */
    SV2_g_saveName[0] = SV2_g_lifePath[0] = SV2_g_timestamp[0] = '\0';
    SV2_g_dateOnly[0] = SV2_g_timeOnly[0] = '\0';
    SV2_g_quest[0] = SV2_g_location[0] = '\0';
    SV2_g_month = 0;

    if (jsonPath[0] != '\0') {
        FILE *jf = fopen(jsonPath, "rb");
        if (jf) {
            fseek(jf, 0, SEEK_END);
            long jsz = ftell(jf);
            fseek(jf, 0, SEEK_SET);
            char *jsonText = (char *)malloc((size_t)jsz + 1);
            if (fread(jsonText, 1, (size_t)jsz, jf) == (size_t)jsz) {
                jsonText[jsz] = '\0';

                char saveName[128] = "?", lifePath[64] = "?", rawTimestamp[64] = "?", quest[256] = "?", debugStr[128] = "";
                double level = 0;
                SE_ExtractJsonStringField(jsonText, "name", saveName, sizeof(saveName));
                SE_ExtractJsonStringField(jsonText, "lifePath", lifePath, sizeof(lifePath));
                SE_ExtractJsonStringField(jsonText, "timestampString", rawTimestamp, sizeof(rawTimestamp));
                SE_ExtractJsonStringField(jsonText, "trackedQuestEntry", quest, sizeof(quest));
                SE_ExtractJsonStringField(jsonText, "debugString", debugStr, sizeof(debugStr));
                SE_ExtractJsonNumberField(jsonText, "level", &level);

                char timestamp[80];
                SE_FormatTimestamp(rawTimestamp, timestamp, sizeof(timestamp));
                SE_FormatDateTimeParts(rawTimestamp, SV2_g_dateOnly, sizeof(SV2_g_dateOnly),
                    SV2_g_timeOnly, sizeof(SV2_g_timeOnly));

                char location[80] = "Unknown area";
                if (debugStr[0] != '\0') {
                    SE_DeriveLocationFromDebugString(debugStr, location, sizeof(location));
                }

                char prettyQuest[256] = "?";
                if (quest[0] != '\0') {
                    SE_PrettifyQuestPath(quest, prettyQuest, sizeof(prettyQuest));
                }

                snprintf(infoLine, sizeof(infoLine),
                    "%s - %s\r\nLevel %d   |   %s   |   Saved %s\r\nQuest: %s   |   Location: %s",
                    lifePath, saveName, (int)level, folderName, timestamp, prettyQuest, location);

                strncpy(SV2_g_saveName, saveName, sizeof(SV2_g_saveName) - 1);
                strncpy(SV2_g_lifePath, lifePath, sizeof(SV2_g_lifePath) - 1);
                strncpy(SV2_g_timestamp, timestamp, sizeof(SV2_g_timestamp) - 1);
                strncpy(SV2_g_quest, prettyQuest, sizeof(SV2_g_quest) - 1);
                strncpy(SV2_g_location, location, sizeof(SV2_g_location) - 1);
                /* rawTimestamp is like "2026-07-03T00:04: */
                if (strlen(rawTimestamp) >= 7 && rawTimestamp[4] == '-') {
                    SV2_g_month = (rawTimestamp[5] - '0') * 10 + (rawTimestamp[6] - '0');
                    if (SV2_g_month < 1 || SV2_g_month > 12) SV2_g_month = 0;
                }
            } else {
                snprintf(infoLine, sizeof(infoLine), "Folder: %s   |   (could not read metadata file)", folderName);
            }
            free(jsonText);
            fclose(jf);
        }
    } else {
        snprintf(infoLine, sizeof(infoLine), "Folder: %s   |   (no metadata JSON found in this folder)", folderName);
    }

    SetWindowTextA(SE_hInfoText, infoLine);
    InvalidateRect(SE_hMainWnd, &SE_g_imageRect, TRUE);
}



void SE_ClearProficiencyFields(void) {
    for (int i = 0; i < MAX_PROFICIENCIES; i++) {
        if (SE_hEditBoxes[i]) { DestroyWindow(SE_hEditBoxes[i]); SE_hEditBoxes[i] = NULL; }
        if (SE_hLabels[i]) { DestroyWindow(SE_hLabels[i]); SE_hLabels[i] = NULL; }
    }
    SE_hLevelEditBox = NULL;
    SE_hCredEditBox = NULL;
}

/* Fractional positions (of the 1918x1008 reference canvas) for each skill's value box. */
typedef struct { const char *name; double fx, fy; } SkillPos;
static const SkillPos g_skillPositions[] = {
    /* Positions derived by OCR-detecting each label's bounding box AND directly detecting its. */
    {"Engineering", 0.111, 0.433},
    {"TechnicalAbilitySkill", 0.310, 0.433},
    {"CombatHacking", 0.182, 0.524},
    {"Kenjutsu", 0.380, 0.524},
    {"Hacking", 0.251, 0.614},
    {"Demolition", 0.444, 0.613},
    {"IntelligenceSkill", 0.645, 0.427},
    {"CoolSkill", 0.795, 0.430},     /* manually nudged down from 0.427. */
    {"ReflexesSkill", 0.551, 0.594},
    {"StrengthSkill", 0.903, 0.592},
    {"Gunslinger", 0.596, 0.754},
    {"Crafting", 0.864, 0.750},
    {"Espionage", 0.558, 0.917},
    {"Stealth", 0.876, 0.918},       /* manually nudged down from 0.913. */
};

/* The top ~35% of the deco image is blank in the artwork (the topmost skill, Engineering, doesn't start until fy=0.434) */
#define LEVEL_FX          0.253
#define CRED_FX           0.415
#define BLOCK_TOP_FY       0.19
#define SKILL_TOP_FY       0.40   /* Engineering icon starts at fy=0.434. */
#define NUM_FONT_HFRAC     0.11   /* big-number font height as a fraction of decoH. */
#define TRI_HFRAC          0.07   /* triangle height as a fraction of decoH. */
#define CAPTION_FONT_HFRAC 0.045
#define GAP_HFRAC          0.012
/* The Engineering/Technical Ability Skill LABEL text starts around fy=0.399, but the ICON. */
/* Directly measured (not estimated) via pixel scan of decoration_bg.png: the topmost dark. */
#define LABEL_ROW_TOP_FY (372.0 / 1008.0)
#define ICON_SAFETY_MARGIN 8

void SE_GetLevelCredRects(DecoXform xform, RECT *levelNum, RECT *credNum, RECT *levelTri, RECT *credTri,
                        RECT *levelCap, RECT *credCap, int *numFontPx, int *capFontPx) {
    RECT br = xform.vis;
    int decoW = br.right - br.left;
    int decoH = br.bottom - br.top;

    int nfp = (int)(NUM_FONT_HFRAC * decoH);
    if (nfp < 14) nfp = 14;
    if (nfp > 140) nfp = 140;

    int cfp = (int)(CAPTION_FONT_HFRAC * decoH);
    if (cfp < 10) cfp = 10;
    if (cfp > 42) cfp = 42;

    int numH = (int)(nfp * 1.3), numW = (int)(nfp * 2.1);
    int capH = (int)(cfp * 1.4), capW = (int)(cfp * 7.0);
    int triH = (int)(TRI_HFRAC * decoH), triW = capW; /* matches the width of "STREET CRED". */
    int gap2 = (int)(GAP_HFRAC * decoH);

    /* Available budget is measured against where the label row ACTUALLY renders. */
    int labelRowTopY = SE_DecoMapY(xform, LABEL_ROW_TOP_FY) - ICON_SAFETY_MARGIN;
    int availBlockH = labelRowTopY - br.top;
    int requiredH = triH + gap2 + (int)(1.5 * capH);
    if (requiredH > availBlockH && requiredH > 0) {
        double shrink = (double)availBlockH / (double)requiredH;
        if (shrink < 0.3) shrink = 0.3;
        nfp = (int)(nfp * shrink); cfp = (int)(cfp * shrink);
        numH = (int)(numH * shrink); numW = (int)(numW * shrink);
        triH = (int)(triH * shrink); triW = (int)(triW * shrink);
        capH = (int)(capH * shrink); capW = (int)(capW * shrink);
        gap2 = (int)(gap2 * shrink);
    }
    *numFontPx = nfp;
    *capFontPx = cfp;

    /* Shift both blocks left together so the (rightmost) Street Cred triangle's right edge. */
    int logBoxRight = br.left + (int)(LOG_WIDTH_FRACTION * decoW);
    int credCx = logBoxRight - triW / 2;
    int levelCx = credCx - (int)((CRED_FX - LEVEL_FX) * decoW);

    /* Position by working backward from where the Engineering/Technical Ability Skill row's. */
    int y = labelRowTopY - (int)(1.5 * capH) - gap2 - triH;
    if (y < br.top) y = br.top;

    levelTri->left = levelCx - triW / 2; levelTri->top = y;
    levelTri->right = levelTri->left + triW; levelTri->bottom = y + triH;
    credTri->left = credCx - triW / 2; credTri->top = y;
    credTri->right = credTri->left + triW; credTri->bottom = y + triH;

    /* Number centered on the triangle's top (cut-off) edge. */
    int numCenterY = y;
    levelNum->left = levelCx - numW / 2; levelNum->top = numCenterY - numH / 2;
    levelNum->right = levelNum->left + numW; levelNum->bottom = numCenterY + numH / 2;
    credNum->left = credCx - numW / 2; credNum->top = numCenterY - numH / 2;
    credNum->right = credNum->left + numW; credNum->bottom = numCenterY + numH / 2;

    y = levelTri->bottom + gap2;
    levelCap->left = levelCx - capW / 2; levelCap->top = y;
    levelCap->right = levelCap->left + capW; levelCap->bottom = y + capH;
    credCap->left = credCx - capW / 2; credCap->top = y;
    credCap->right = credCap->left + capW; credCap->bottom = y + capH;
}

#define BTN_W 208
#define BTN_H 49 /* 1.3x the original 160x38. */

void SE_RepositionButtons(void) {
    if (!SE_hLoadBtn || !SE_hSaveBtn) return;

    DecoXform xform = SE_GetDecoXform();
    RECT levelNumR, credNumR, levelTriR, credTriR, levelCapR, credCapR;
    int numFontPx, capFontPx;
    SE_GetLevelCredRects(xform, &levelNumR, &credNumR, &levelTriR, &credTriR,
        &levelCapR, &credCapR, &numFontPx, &capFontPx);

    /* Save Changes' vertical center lands on the Level triangle's bottom edge. */
    int saveBtnY = levelTriR.bottom - BTN_H / 2;
    int maxSaveBtnY = SE_DecoMapY(xform, LABEL_ROW_TOP_FY) - ICON_SAFETY_MARGIN - BTN_H - 10;
    if (saveBtnY > maxSaveBtnY) saveBtnY = maxSaveBtnY;
    int loadBtnY = saveBtnY - SAVEBTN_STAGGER_GAP - BTN_H;

    int minLoadBtnY = SE_GetInfoBoxY() + INFOBOX_H + 15;
    if (loadBtnY < minLoadBtnY) {
        loadBtnY = minLoadBtnY;
        saveBtnY = loadBtnY + BTN_H + SAVEBTN_STAGGER_GAP;
    }

    MoveWindow(SE_hLoadBtn, 20, loadBtnY, BTN_W, BTN_H, TRUE);
    MoveWindow(SE_hSaveBtn, 110, saveBtnY, BTN_W, BTN_H, TRUE);
}

void SE_RepositionInfoBox(void) {
    if (!SE_hInfoText) return;
    int availWidth = SE_g_currentClientWidth - 40;
    int rightEdge = 20 + (int)(LOG_WIDTH_FRACTION * availWidth); /* matches log box's right edge. */
    int w = rightEdge - 20;
    if (w < 200) w = 200;

    /* Centered in the gap between the path row's bottom and Load Save's actual top. */
    int gapTop = PATHROW_Y + PATHROW_H;
    int gapBottom = gapTop + (SE_GetButtonRowY() - gapTop); /* fallback if button not ready yet. */
    if (SE_hLoadBtn) {
        RECT btnR;
        GetWindowRect(SE_hLoadBtn, &btnR);
        POINT p = { btnR.left, btnR.top };
        ScreenToClient(SE_hMainWnd, &p);
        gapBottom = p.y;
    }
    int y = gapTop + (gapBottom - gapTop - INFOBOX_H) / 2;
    if (y < gapTop) y = gapTop;

    MoveWindow(SE_hInfoText, 20, y, w, INFOBOX_H, TRUE);
}

/* Scans the actual rendered artwork (not an offline copy) to find the true vertical center. */
int SE_CompareInts(const void *a, const void *b) { return (*(const int *)a) - (*(const int *)b); }

void SE_AutoCenterSkillNumbers(DecoXform xform, int skillBoxW, int skillBoxH) {
    RECT art = xform.art;
    int artW = art.right - art.left, artH = art.bottom - art.top;
    if (artW <= 0 || artH <= 0 || !SE_g_bodyDecoImage) return;

    HDC screenDC = GetDC(SE_hMainWnd);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, artW, artH);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
    ReleaseDC(SE_hMainWnd, screenDC);

    RECT fillR = {0, 0, artW, artH};
    FillRect(memDC, &fillR, (HBRUSH)GetStockObject(WHITE_BRUSH));

    GpGraphics *g = NULL;
    GdipCreateFromHDC(memDC, &g);
    if (g) {
        int sx = (int)(xform.cropFxStart * 1918.0);
        int sy = (int)(xform.cropFyStart * 1008.0);
        int swidth = 1918 - 2 * sx;
        int sheight = 1008 - sy;
        GdipDrawImageRectRectI(g, SE_g_bodyDecoImage, 0, 0, artW, artH,
            sx, sy, swidth, sheight, UnitPixel, NULL, NULL, NULL);
        GdipDeleteGraphics(g);
    }

    #define DARK_THRESH 160
    #define MAX_SCAN 30
    #define MAX_SHIFT 10
    int sampleOffsets[5] = { -12, -6, 0, 6, 12 };

    int numPositions = sizeof(g_skillPositions) / sizeof(SkillPos);
    for (int i = 0; i < SE_g_save.proficiencyCount; i++) {
        double fx = -1, fy = -1;
        for (int p = 0; p < numPositions; p++) {
            if (strcmp(g_skillPositions[p].name, SE_g_save.proficiencies[i].name) == 0) {
                fx = g_skillPositions[p].fx; fy = g_skillPositions[p].fy;
                break;
            }
        }
        if (fx < 0 || !SE_hEditBoxes[i]) continue;
        if (strcmp(SE_g_save.proficiencies[i].name, "Level") == 0) continue;
        if (strcmp(SE_g_save.proficiencies[i].name, "StreetCred") == 0) continue;
        if (strcmp(SE_g_save.proficiencies[i].name, "Stealth") == 0) continue;
        if (strcmp(SE_g_save.proficiencies[i].name, "CoolSkill") == 0) continue;

        int cx = SE_DecoMapX(xform, fx), cy = SE_DecoMapY(xform, fy) + (int)(skillBoxH * 0.3);
        int ly = cy - art.top;

        int ups[5], downs[5], nUp = 0, nDown = 0;
        for (int s = 0; s < 5; s++) {
            int lx = (cx - art.left) + sampleOffsets[s];
            if (lx < 0 || lx >= artW) continue;

            for (int d = 3; d <= MAX_SCAN; d++) {
                int y = ly - d;
                if (y < 0) break;
                COLORREF c = GetPixel(memDC, lx, y);
                int lum = (GetRValue(c) + GetGValue(c) + GetBValue(c)) / 3;
                if (lum < DARK_THRESH) { ups[nUp++] = d; break; }
            }
            for (int d = 3; d <= MAX_SCAN; d++) {
                int y = ly + d;
                if (y >= artH) break;
                COLORREF c = GetPixel(memDC, lx, y);
                int lum = (GetRValue(c) + GetGValue(c) + GetBValue(c)) / 3;
                if (lum < DARK_THRESH) { downs[nDown++] = d; break; }
            }
        }

        char dbg[180];
        int clusterOk = 0;
        if (nUp >= 4 && nDown >= 4) {
            qsort(ups, nUp, sizeof(int), SE_CompareInts);
            qsort(downs, nDown, sizeof(int), SE_CompareInts);
            /* Require the samples to agree tightly with each other. */
            if ((ups[nUp - 1] - ups[0]) <= 6 && (downs[nDown - 1] - downs[0]) <= 6) {
                clusterOk = 1;
            }
        }

        if (clusterOk) {
            int medUp = ups[nUp / 2], medDown = downs[nDown / 2];
            int shift = (medDown - medUp) / 2;
            if (shift > MAX_SHIFT) shift = MAX_SHIFT;
            if (shift < -MAX_SHIFT) shift = -MAX_SHIFT;

            int newCy = cy + shift;
            MoveWindow(SE_hEditBoxes[i], cx - skillBoxW / 2, newCy - skillBoxH / 2,
                skillBoxW, skillBoxH, TRUE);
            snprintf(dbg, sizeof(dbg),
                "[autocenter] %-12s up=%d down=%d (n=%d/%d) shift=%+d",
                SE_g_save.proficiencies[i].name, medUp, medDown, nUp, nDown, shift);
        } else {
            snprintf(dbg, sizeof(dbg),
                "[autocenter] %-12s low confidence (up:%d down:%d of 5) - kept pre-measured position",
                SE_g_save.proficiencies[i].name, nUp, nDown);
        }
        SE_AppendStatus(dbg);
    }

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
}

void SE_BuildProficiencyFields(void) {
    SE_ClearProficiencyFields();

    static HFONT s_prevBigFont = NULL, s_prevSkillFont = NULL;
    if (s_prevBigFont) { DeleteObject(s_prevBigFont); s_prevBigFont = NULL; }
    if (s_prevSkillFont) { DeleteObject(s_prevSkillFont); s_prevSkillFont = NULL; }

    int levelIdx = -1, credIdx = -1;
    for (int i = 0; i < SE_g_save.proficiencyCount; i++) {
        if (strcmp(SE_g_save.proficiencies[i].name, "Level") == 0) levelIdx = i;
        if (strcmp(SE_g_save.proficiencies[i].name, "StreetCred") == 0) credIdx = i;
    }

    DecoXform xform = SE_GetDecoXform();
    RECT br = xform.vis;
    int decoH = br.bottom - br.top;

    /* Level / Street Cred: big numbers positioned above a programmatically drawn triangle +. */
    RECT levelNumR, credNumR, levelTriR, credTriR, levelCapR, credCapR;
    int bigFontPx, capFontPx;
    SE_GetLevelCredRects(xform, &levelNumR, &credNumR, &levelTriR, &credTriR,
        &levelCapR, &credCapR, &bigFontPx, &capFontPx);

    HFONT hBigFont = CreateFontA(-bigFontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_ROMAN, "Bookman Old Style");
    s_prevBigFont = hBigFont;

    RECT *prominentRect[2] = { &levelNumR, &credNumR };
    int prominent[2] = { levelIdx, credIdx };
    for (int k = 0; k < 2; k++) {
        int i = prominent[k];
        if (i < 0) continue;
        RECT *r = prominentRect[k];

        char valStr[16];
        snprintf(valStr, sizeof(valStr), "%d", SE_g_save.proficiencies[i].currentValue);
        SE_hEditBoxes[i] = CreateWindowA("EDIT", valStr,
            WS_VISIBLE | WS_CHILD | ES_NUMBER | ES_CENTER,
            r->left, r->top, r->right - r->left, r->bottom - r->top,
            SE_hMainWnd, (HMENU)(intptr_t)(SE_ID_FIRST_EDIT + i), SE_hInst, NULL);
        SendMessageA(SE_hEditBoxes[i], WM_SETFONT, (WPARAM)hBigFont, TRUE);
        if (k == 0) SE_hLevelEditBox = SE_hEditBoxes[i]; else SE_hCredEditBox = SE_hEditBoxes[i];
    }

    /* Remaining skill value boxes, positioned to align with their icons in the decoration image. */
    int skillFontPx = (int)(0.034 * decoH);
    if (skillFontPx < 12) skillFontPx = 12;
    if (skillFontPx > 40) skillFontPx = 40;
    int skillBoxW = (int)(skillFontPx * 3.0);
    int skillBoxH = (int)(skillFontPx * 1.5);

    HFONT hSkillFont = CreateFontA(-skillFontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_ROMAN, "Bookman Old Style");
    s_prevSkillFont = hSkillFont;

    int numPositions = sizeof(g_skillPositions) / sizeof(SkillPos);

    for (int i = 0; i < SE_g_save.proficiencyCount; i++) {
        if (i == levelIdx || i == credIdx) continue;

        double fx = -1, fy = -1;
        for (int p = 0; p < numPositions; p++) {
            if (strcmp(g_skillPositions[p].name, SE_g_save.proficiencies[i].name) == 0) {
                fx = g_skillPositions[p].fx;
                fy = g_skillPositions[p].fy;
                break;
            }
        }
        if (fx < 0) continue; /* unknown skill name. */

        int cx = SE_DecoMapX(xform, fx);
        int cy = SE_DecoMapY(xform, fy) + (int)(skillBoxH * 0.3); /* systematic correction. */

        char valStr[16];
        snprintf(valStr, sizeof(valStr), "%d", SE_g_save.proficiencies[i].currentValue);
        SE_hEditBoxes[i] = CreateWindowA("EDIT", valStr,
            WS_VISIBLE | WS_CHILD | ES_NUMBER | ES_CENTER,
            cx - skillBoxW / 2, cy - skillBoxH / 2, skillBoxW, skillBoxH,
            SE_hMainWnd, (HMENU)(intptr_t)(SE_ID_FIRST_EDIT + i), SE_hInst, NULL);
        SendMessageA(SE_hEditBoxes[i], WM_SETFONT, (WPARAM)hSkillFont, TRUE);
    }

    SE_g_gridBottomY = br.bottom;
    SE_RepositionStatusBox();

    SE_AppendStatus("[autocenter] verifying number positions against actual artwork pixels...");
    SE_AutoCenterSkillNumbers(xform, skillBoxW, skillBoxH);
}

char SE_g_customSavesFolder[MAX_PATH] = "";

void SE_GetDefaultSavesFolder(char *out, size_t outSize) {
    if (SE_g_customSavesFolder[0] != '\0') {
        strncpy(out, SE_g_customSavesFolder, outSize - 1);
        out[outSize - 1] = '\0';
        return;
    }
    char userProfile[MAX_PATH];
    DWORD sz = GetEnvironmentVariableA("USERPROFILE", userProfile, MAX_PATH);
    if (sz > 0) {
        snprintf(out, outSize, "%s\\Saved Games\\CD Projekt Red\\Cyberpunk 2077", userProfile);
    } else {
        out[0] = '\0';
    }
}

/* Save Location: lets the user point at a non-standard saves folder (for unconventional install locations) */
void SE_ChooseSavesFolder(void) {
    char initialDir[MAX_PATH];
    SE_GetDefaultSavesFolder(initialDir, sizeof(initialDir));

    BROWSEINFOA bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = SE_hMainWnd;
    bi.lpszTitle = "Select your Cyberpunk 2077 saves folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl) {
        char path[MAX_PATH];
        if (SHGetPathFromIDListA(pidl, path)) {
            strncpy(SE_g_customSavesFolder, path, sizeof(SE_g_customSavesFolder) - 1);
            extern void AppendStatus(const char *);
            char msg[600];
            snprintf(msg, sizeof(msg), "Saves folder set to: %s", path);
            AppendStatus(msg);
        }
        CoTaskMemFree(pidl);
    }
}

/* Load Save now does the browse-for-file step itself instead of requiring it separately. */
BOOL SE_BrowseForSaveFile(void) {
    char path[MAX_PATH] = "";
    char initialDir[MAX_PATH];
    SE_GetDefaultSavesFolder(initialDir, sizeof(initialDir));

    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = SE_hMainWnd;
    ofn.lpstrFilter = "Save files (sav.dat)\0sav.dat\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = initialDir;
    ofn.lpstrTitle = "Select your sav.dat file";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn)) {
        SetWindowTextA(SE_hPathBox, path);
        return TRUE;
    }
    return FALSE;
}

void SE_LoadSaveFile(void) {
    char path[MAX_PATH];
    GetWindowTextA(SE_hPathBox, path, sizeof(path));

    if (SE_g_saveLoaded) {
        SaveFile_Free(&SE_g_save);
        SE_g_saveLoaded = 0;
    }

    SE_AppendStatus("Loading save file...");
    if (!SaveFile_Load(&SE_g_save, path)) {
        char msg[600];
        snprintf(msg, sizeof(msg), "Load failed: %s", SE_g_save.lastError);
        SE_AppendStatus(msg);
        EnableWindow(SE_hSaveBtn, FALSE);
        return;
    }

    SE_g_saveLoaded = 1;
    char msg[128];
    snprintf(msg, sizeof(msg), "Loaded OK. Found %d editable values.", SE_g_save.proficiencyCount);
    SE_AppendStatus(msg);

    SE_LoadScreenshotAndInfo(path);
    SE_BuildProficiencyFields();
    EnableWindow(SE_hSaveBtn, TRUE);
}

void SE_SaveChangesToFile(void) {
    if (!SE_g_saveLoaded) { SE_AppendStatus("No save loaded."); return; }

    /* Read edited values back from the UI into the engine struct. */
    for (int i = 0; i < SE_g_save.proficiencyCount; i++) {
        char valStr[16];
        GetWindowTextA(SE_hEditBoxes[i], valStr, sizeof(valStr));
        int newVal = atoi(valStr);
        SE_g_save.proficiencies[i].newValue = newVal;
    }

    char origPath[MAX_PATH];
    GetWindowTextA(SE_hPathBox, origPath, sizeof(origPath));

    /* Automatic backup. */
    char backupPath[MAX_PATH + 8];
    snprintf(backupPath, sizeof(backupPath), "%s.bak", origPath);

    DWORD attrs = GetFileAttributesA(backupPath);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        /* only back up once. */
        CopyFileA(origPath, backupPath, FALSE);
        SE_AppendStatus("Created backup (.bak) of original save.");
    }

    SE_AppendStatus("Writing changes...");
    if (!SaveFile_Write(&SE_g_save, origPath)) {
        char msg[600];
        snprintf(msg, sizeof(msg), "Save failed: %s (original + backup are untouched on disk)", SE_g_save.lastError);
        SE_AppendStatus(msg);
        return;
    }

    SE_AppendStatus("Saved successfully! Reloading to verify...");

    /* Verify by reloading our own output. */
    SaveFile verifySf;
    if (SaveFile_Load(&verifySf, origPath)) {
        int allMatch = 1;
        for (int i = 0; i < SE_g_save.proficiencyCount; i++) {
            for (int j = 0; j < verifySf.proficiencyCount; j++) {
                if (strcmp(SE_g_save.proficiencies[i].name, verifySf.proficiencies[j].name) == 0) {
                    if (verifySf.proficiencies[j].currentValue != SE_g_save.proficiencies[i].newValue) {
                        allMatch = 0;
                    }
                    break;
                }
            }
        }
        SaveFile_Free(&verifySf);
        SE_AppendStatus(allMatch ? "Verification passed - all values confirmed correct in the written file."
                              : "WARNING: verification found a mismatch - check values carefully before using this save.");
    } else {
        SE_AppendStatus("WARNING: could not reload the file to verify - check it carefully before using.");
    }
}

LRESULT CALLBACK SE_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SE_hMainWnd = hwnd;
    switch (msg) {
        case WM_CTLCOLOREDIT: {
            HWND hCtl = (HWND)lParam;
            int ctlId = GetDlgCtrlID(hCtl);
            if (ctlId >= SE_ID_FIRST_EDIT) {
                SetBkMode((HDC)wParam, TRANSPARENT);
                SetTextColor((HDC)wParam, RGB(0, 0, 0));
                return (LRESULT)GetStockObject(NULL_BRUSH);
            }
            break;
        }

        case WM_CREATE: {
            HFONT hFont = SE_g_appFont;
            HFONT hDefaultFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            char defaultFolder[MAX_PATH];
            SE_GetDefaultSavesFolder(defaultFolder, sizeof(defaultFolder));

            SE_hSaveLabel = CreateWindowA("STATIC", "Save file:",
                WS_VISIBLE | WS_CHILD | SS_CENTER, 20, 15, 70, 20, hwnd, NULL, SE_hInst, NULL);
            SendMessageA(SE_hSaveLabel, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

            /* Clickable "field" showing the path. */
            SE_hPathBox = CreateWindowA("STATIC", "(click here to choose a save file)",
                WS_VISIBLE | WS_CHILD | WS_BORDER | SS_NOTIFY | SS_LEFT | SS_CENTERIMAGE,
                95, 13, 480, 24, hwnd, (HMENU)SE_ID_PATH_BOX, SE_hInst, NULL);
            SendMessageA(SE_hPathBox, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

            SE_hHelpBtn = CreateWindowA("BUTTON", "?",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                580, 13, 26, 24, hwnd, (HMENU)SE_ID_HELP_BTN, SE_hInst, NULL);
            SendMessageA(SE_hHelpBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

            SE_hTooltip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL,
                WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
                CW_USEDEFAULT, CW_USEDEFAULT, hwnd, NULL, SE_hInst, NULL);
            TOOLINFOA ti;
            ZeroMemory(&ti, sizeof(ti));
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
            ti.hwnd = SE_hHelpBtn;
            ti.uId = (UINT_PTR)SE_hHelpBtn;
            ti.hinst = SE_hInst;
            ti.lpszText = "Click on the save file field to the left to select the save file you wish to edit";
            SendMessageA(SE_hTooltip, TTM_ADDTOOLA, 0, (LPARAM)&ti);

            SE_hLoadBtn = CreateWindowA("BUTTON", "Load Save",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                20, SE_GetButtonRowY(), BTN_W, BTN_H, hwnd, (HMENU)SE_ID_LOAD_BTN, SE_hInst, NULL);
            SendMessageA(SE_hLoadBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

            SE_hSaveBtn = CreateWindowA("BUTTON", "Save Changes",
                WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                110, SE_GetButtonRowY() + BTN_H + SAVEBTN_STAGGER_GAP, BTN_W, BTN_H,
                hwnd, (HMENU)SE_ID_SAVE_BTN, SE_hInst, NULL);
            SendMessageA(SE_hSaveBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
            EnableWindow(SE_hSaveBtn, FALSE);

            SE_hInfoText = CreateWindowA("STATIC", "",
                WS_VISIBLE | WS_CHILD | SS_CENTER, 20, SE_GetInfoBoxY(), 750, INFOBOX_H, hwnd, (HMENU)SE_ID_INFO_TEXT, SE_hInst, NULL);
            SendMessageA(SE_hInfoText, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

            SE_hStatusText = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_VISIBLE | WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                20, SE_GetGridStartY(), 1300, 160, hwnd, (HMENU)SE_ID_STATUS_TEXT, SE_hInst, NULL);
            SendMessageA(SE_hStatusText, WM_SETFONT, (WPARAM)hFont, TRUE);

            { RECT br0 = SE_GetBodyDecoRect(); SE_g_gridBottomY = br0.bottom; }
            SE_RepositionStatusBox();

            {
                char bootMsg[200];
                SYSTEMTIME st; GetLocalTime(&st);
                snprintf(bootMsg, sizeof(bootMsg),
                    "[boot] PID=%lu time=%02d:%02d:%02d.%03d SE_g_saveLoaded=%d proficiencyCount=%d",
                    (unsigned long)GetCurrentProcessId(), st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                    SE_g_saveLoaded, SE_g_save.proficiencyCount);
                SE_AppendStatus(bootMsg);
            }
            SE_AppendStatus("Ready. Click the save file field above and choose a save.");
            SE_AppendStatus("(A backup .bak is created automatically before any save.)");
            break;
        }

        case WM_TIMER: {
            if (wParam == SE_TIMER_STARTUP_RELAYOUT) {
                KillTimer(hwnd, SE_TIMER_STARTUP_RELAYOUT);
                RECT rc0;
                GetClientRect(hwnd, &rc0);
                SE_g_currentClientWidth = rc0.right;
                SE_g_currentClientHeight = rc0.bottom;
                SE_RepositionPathRow();
                SE_RepositionButtons();
                SE_RepositionInfoBox();
                SE_g_imageRect = SE_GetScreenshotRect(SE_GetDecoXform());
                if (SE_g_saveLoaded) {
                    SE_BuildProficiencyFields();
                } else {
                    SE_RepositionStatusBox();
                }
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }

        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            SE_g_currentClientWidth = rc.right;
            SE_g_currentClientHeight = rc.bottom;

            SE_RepositionPathRow();
            SE_RepositionButtons();
            SE_RepositionInfoBox();

            /* Screenshot positioned in the same crop-mapped coordinate space as the rest of the. */
            SE_g_imageRect = SE_GetScreenshotRect(SE_GetDecoXform());
            InvalidateRect(hwnd, NULL, TRUE);

            if (SE_g_saveLoaded) {
                SE_BuildProficiencyFields(); /* reflow grid to fit new width, also repositions status box. */
            } else {
                SE_RepositionStatusBox();
            }

            /* Dump the full computed layout state to the log. */
            {
                DecoXform dbgXform = SE_GetDecoXform();
                RECT dbgBr = dbgXform.vis;
                int decoW = dbgBr.right - dbgBr.left, decoH = dbgBr.bottom - dbgBr.top;

                RECT lNum, cNum, lTri, cTri, lCap, cCap;
                int numFontPx, capFontPx;
                SE_GetLevelCredRects(dbgXform, &lNum, &cNum, &lTri, &cTri, &lCap, &cCap, &numFontPx, &capFontPx);

                int skillFontPx = (int)(0.034 * decoH);
                if (skillFontPx < 12) skillFontPx = 12;
                if (skillFontPx > 40) skillFontPx = 40;

                char dbg[512];
                snprintf(dbg, sizeof(dbg),
                    "[debug] client=%dx%d pathY=%d infoY=%d btnY=%d gridTop=%d",
                    SE_g_currentClientWidth, SE_g_currentClientHeight,
                    PATHROW_Y, SE_GetInfoBoxY(), SE_GetButtonRowY(), SE_GetGridStartY());
                SE_AppendStatus(dbg);

                snprintf(dbg, sizeof(dbg),
                    "[debug] deco=(%d,%d)-(%d,%d) [%dx%d]  screenshot=(%d,%d)-(%d,%d) [%dx%d]",
                    dbgBr.left, dbgBr.top, dbgBr.right, dbgBr.bottom, decoW, decoH,
                    SE_g_imageRect.left, SE_g_imageRect.top, SE_g_imageRect.right, SE_g_imageRect.bottom,
                    SE_g_imageRect.right - SE_g_imageRect.left, SE_g_imageRect.bottom - SE_g_imageRect.top);
                SE_AppendStatus(dbg);

                snprintf(dbg, sizeof(dbg),
                    "[debug] levelNum=(%d,%d)-(%d,%d) levelTri=(%d,%d)-(%d,%d) levelCap=(%d,%d)-(%d,%d) numFont=%d capFont=%d",
                    lNum.left, lNum.top, lNum.right, lNum.bottom,
                    lTri.left, lTri.top, lTri.right, lTri.bottom,
                    lCap.left, lCap.top, lCap.right, lCap.bottom,
                    numFontPx, capFontPx);
                SE_AppendStatus(dbg);

                RECT logR; GetWindowRect(SE_hStatusText, &logR);
                POINT pt1 = {logR.left, logR.top}, pt2 = {logR.right, logR.bottom};
                ScreenToClient(hwnd, &pt1); ScreenToClient(hwnd, &pt2);
                snprintf(dbg, sizeof(dbg),
                    "[debug] skillFont=%d logBox=(%d,%d)-(%d,%d)",
                    skillFontPx, pt1.x, pt1.y, pt2.x, pt2.y);
                SE_AppendStatus(dbg);

                DecoXform xf = SE_GetDecoXform();
                int engX = SE_DecoMapX(xf, 0.111), engY = SE_DecoMapY(xf, 0.434);
                snprintf(dbg, sizeof(dbg),
                    "[debug] cropFyTop=%.3f cropFxSide=%.3f engineeringMappedAt=(%d,%d)",
                    xf.cropFyStart, xf.cropFxStart, engX, engY);
                SE_AppendStatus(dbg);

                RECT lblR, pathR, btnR;
                GetWindowRect(SE_hSaveLabel, &lblR); GetWindowRect(SE_hPathBox, &pathR); GetWindowRect(SE_hHelpBtn, &btnR);
                POINT lp1={lblR.left,lblR.top}, lp2={lblR.right,lblR.bottom};
                POINT pp1={pathR.left,pathR.top}, pp2={pathR.right,pathR.bottom};
                POINT bp1={btnR.left,btnR.top}, bp2={btnR.right,btnR.bottom};
                ScreenToClient(hwnd,&lp1); ScreenToClient(hwnd,&lp2);
                ScreenToClient(hwnd,&pp1); ScreenToClient(hwnd,&pp2);
                ScreenToClient(hwnd,&bp1); ScreenToClient(hwnd,&bp2);
                snprintf(dbg, sizeof(dbg),
                    "[debug] label=(%d,%d)-(%d,%d) pathBox=(%d,%d)-(%d,%d) helpBtn=(%d,%d)-(%d,%d)",
                    lp1.x,lp1.y,lp2.x,lp2.y, pp1.x,pp1.y,pp2.x,pp2.y, bp1.x,bp1.y,bp2.x,bp2.y);
                SE_AppendStatus(dbg);
            }
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            GpGraphics *graphics = NULL;
            GdipCreateFromHDC(hdc, &graphics);
            if (graphics) {
                if (SE_g_screenshotImage) {
                    GdipDrawImageRectI(graphics, SE_g_screenshotImage,
                        SE_g_imageRect.left, SE_g_imageRect.top,
                        SE_g_imageRect.right - SE_g_imageRect.left,
                        SE_g_imageRect.bottom - SE_g_imageRect.top);
                } else {
                    RECT r = SE_g_imageRect;
                    FrameRect(hdc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
                }

                if (SE_g_bodyDecoImage) {
                    DecoXform xform = SE_GetDecoXform();
                    RECT br = xform.art;

                    int sx = (int)(xform.cropFxStart * 1918.0);
                    int sy = (int)(xform.cropFyStart * 1008.0);
                    int swidth = 1918 - 2 * sx;
                    int sheight = 1008 - sy;

                    GdipDrawImageRectRectI(graphics, SE_g_bodyDecoImage,
                        br.left, br.top, br.right - br.left, br.bottom - br.top,
                        sx, sy, swidth, sheight,
                        UnitPixel, NULL, NULL, NULL);
                }

                GdipDeleteGraphics(graphics);
            }

            /* Level / Street Cred triangle + caption, drawn with plain GDI instead of the (broken) */
            if (SE_g_bodyDecoImage) {
                DecoXform paintXform = SE_GetDecoXform();
                RECT levelNumR, credNumR, levelTriR, credTriR, levelCapR, credCapR;
                int numFontPx, capFontPx;
                SE_GetLevelCredRects(paintXform, &levelNumR, &credNumR, &levelTriR, &credTriR,
                    &levelCapR, &credCapR, &numFontPx, &capFontPx);

                HPEN hTriPen = CreatePen(PS_SOLID, 2, RGB(20, 20, 30));
                HPEN hOldPen = (HPEN)SelectObject(hdc, hTriPen);
                HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

                /* Side angle ~40 degrees from the base (within the requested 35-45 range) instead of an. */
                double sideAngleDeg = 40.0;
                int levelTriH = levelTriR.bottom - levelTriR.top;
                int levelInset = (int)(levelTriH / tan(sideAngleDeg * 3.14159265 / 180.0));
                int levelHalfW = (levelTriR.right - levelTriR.left) / 2;
                if (levelInset > levelHalfW - 3) levelInset = levelHalfW - 3;

                int credTriH = credTriR.bottom - credTriR.top;
                int credInset = (int)(credTriH / tan(sideAngleDeg * 3.14159265 / 180.0));
                int credHalfW = (credTriR.right - credTriR.left) / 2;
                if (credInset > credHalfW - 3) credInset = credHalfW - 3;

                POINT levelPts[4] = {
                    { levelTriR.left + levelInset, levelTriR.top },
                    { levelTriR.left, levelTriR.bottom },
                    { levelTriR.right, levelTriR.bottom },
                    { levelTriR.right - levelInset, levelTriR.top }
                };
                Polyline(hdc, levelPts, 4);

                POINT credPts[4] = {
                    { credTriR.left + credInset, credTriR.top },
                    { credTriR.left, credTriR.bottom },
                    { credTriR.right, credTriR.bottom },
                    { credTriR.right - credInset, credTriR.top }
                };
                Polyline(hdc, credPts, 4);

                SelectObject(hdc, hOldPen);
                SelectObject(hdc, hOldBrush);
                DeleteObject(hTriPen);

                HFONT hCapFont = CreateFontA(-capFontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                    DEFAULT_PITCH | FF_ROMAN, "Bookman Old Style");
                HFONT hOldFont = (HFONT)SelectObject(hdc, hCapFont);
                int oldBkMode = SetBkMode(hdc, TRANSPARENT);
                COLORREF oldColor = SetTextColor(hdc, RGB(20, 20, 40));

                /* Center each caption's own tightly-measured width on its triangle's center, rather than. */
                int levelCx = (levelCapR.left + levelCapR.right) / 2;
                int credCx = (credCapR.left + credCapR.right) / 2;

                SIZE levelSz, credSz;
                GetTextExtentPoint32A(hdc, "LEVEL", 5, &levelSz);
                GetTextExtentPoint32A(hdc, "STREET CRED", 11, &credSz);

                RECT levelTextR = { levelCx - levelSz.cx / 2, levelCapR.top,
                                     levelCx + levelSz.cx / 2, levelCapR.bottom };
                RECT credTextR = { credCx - credSz.cx / 2, credCapR.top,
                                    credCx + credSz.cx / 2, credCapR.bottom };

                DrawTextA(hdc, "LEVEL", -1, &levelTextR, DT_CENTER | DT_SINGLELINE | DT_NOCLIP);
                DrawTextA(hdc, "STREET CRED", -1, &credTextR, DT_CENTER | DT_SINGLELINE | DT_NOCLIP);

                SetTextColor(hdc, oldColor);
                SetBkMode(hdc, oldBkMode);
                SelectObject(hdc, hOldFont);
                DeleteObject(hCapFont);
            }

            EndPaint(hwnd, &ps);
            break;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == SE_ID_PATH_BOX && HIWORD(wParam) == STN_CLICKED) {
                SE_BrowseForSaveFile();
            } else if (LOWORD(wParam) == SE_ID_LOAD_BTN) {
                SE_LoadSaveFile();
            } else if (LOWORD(wParam) == SE_ID_SAVE_BTN) {
                SE_SaveChangesToFile();
            } else if (HIWORD(wParam) == EN_CHANGE &&
                       GetDlgCtrlID((HWND)lParam) >= SE_ID_FIRST_EDIT) {
                /* All these boxes are transparent (artwork shows through), so a full repaint is needed on. */
                RECT r;
                GetWindowRect((HWND)lParam, &r);
                POINT p1 = {r.left, r.top}, p2 = {r.right, r.bottom};
                ScreenToClient(hwnd, &p1); ScreenToClient(hwnd, &p2);
                RECT inval = {p1.x - 4, p1.y - 4, p2.x + 4, p2.y + 4};
                InvalidateRect(hwnd, &inval, TRUE);
            }
            break;

        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

/* One-time setup: GDI+, fonts, and artwork resources. */
void SE_InitOnce(HINSTANCE hInstance) {
    SE_hInst = hInstance;

    GdiplusStartupInput gdiInput;
    gdiInput.GdiplusVersion = 1;
    gdiInput.DebugEventCallback = NULL;
    gdiInput.SuppressBackgroundThread = FALSE;
    gdiInput.SuppressExternalCodecs = FALSE;
    GdiplusStartup(&SE_g_gdiplusToken, &gdiInput, NULL);

    SE_g_appFont = CreateFontA(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_ROMAN, "Bookman Old Style");

    SE_g_levelTriImage = SE_LoadImageFromResource("IDR_LEVEL_TRI");
    SE_g_credTriImage = SE_LoadImageFromResource("IDR_CRED_TRI");
    SE_g_bodyDecoImage = SE_LoadImageFromResource("IDR_BODY_DECO");
}

void SE_Shutdown(void) {
    if (SE_g_saveLoaded) SaveFile_Free(&SE_g_save);
    if (SE_g_screenshotImage) GdipDisposeImage(SE_g_screenshotImage);
    GdiplusShutdown(SE_g_gdiplusToken);
}

/* ---------------------------------------------------------------------- */
/* Main window */
/* ---------------------------------------------------------------------- */

/* New image-based main menu. All symbols prefixed MENU_. */

#define MENU_NATIVE_W 1918
#define MENU_NATIVE_H 974

/* GDI+ doesn't always export FrameDimensionTime/PropertyTagFrameDelay as linkable symbols. */
static const GUID MENU_FrameDimensionTime =
    {0x6aedbd6d, 0x3fb5, 0x418a, {0x83, 0xa6, 0x7f, 0x45, 0x22, 0x9d, 0xc8, 0x72}};
#define MENU_PROPERTY_TAG_FRAME_DELAY 0x5100

typedef struct {
    GpImage *img;
    RECT bbox;      /* non-transparent pixel bounding box, native coords. */
    int valid;
} MenuImg;

typedef struct {
    GpImage *img;
    UINT frameCount;
    UINT *delaysMs;
    GpBitmap **cachedFrames; /* pre-decoded, ready-to-draw frames. */
    int cacheW, cacheH;
    UINT currentFrame;
    BOOL playing;
    DWORD lastTickMs;
} MenuGif;

/* Static layer images. */
MenuImg gMB_Background, gMB_City, gMB_Moon, gMB_DavidMemoir;
MenuImg gMB_Button[10];       /* 1..9 used. */
MenuImg gMB_ButtonInv[10];    /* 0 = no inverted variant. */
MenuImg gMB_Check4, gMB_Checked4, gMB_Check5, gMB_Checked5, gMB_Check8, gMB_Checked8;
MenuImg gMB_Settings;
MenuImg gMB_Title, gMB_TitleInv;

/* Cycling background images (folder-driven, embedded fallback) */
#define MENU_MAX_CYCLE_IMAGES 32
MenuImg gMB_CycleNormal[MENU_MAX_CYCLE_IMAGES];
MenuImg gMB_CycleInverted[MENU_MAX_CYCLE_IMAGES]; /* .valid=0 if none for that slot. */
int gMB_CycleCount = 0;
int gMB_CycleIndex = 0;
BOOL gMB_CycleShowInverted = FALSE;
DWORD gMB_CycleLastSwitchMs = 0;
#define MENU_CYCLE_INTERVAL_MS 4000

/* GIF players. */
MenuGif gMB_GifTitleN2I, gMB_GifTitleI2N, gMB_GifLaunch;

/* Title state machine. */
#define MENU_TITLE_NORMAL       0
#define MENU_TITLE_PLAYING_N2I  1
#define MENU_TITLE_INVERTED     2
#define MENU_TITLE_PLAYING_I2N  3
int gMB_TitleState = MENU_TITLE_NORMAL;

/* Launch gif overlay. */
BOOL gMB_LaunchGifPlaying = FALSE;
BOOL gMB_LaunchGifPendingLaunch = FALSE; /* fire the real launch once gif ends. */

/* Console visibility (was always-visible; now toggled) */
BOOL gMB_ConsoleVisible = FALSE;

/* Check states - default ON. */
BOOL gMB_Check4On = TRUE; /* deploy REDmod-format mods. */
BOOL gMB_Check5On = TRUE; /* dynamic priority. */
BOOL gMB_Check8On = TRUE; /* skip loading screen. */

/* Settings panel (buttons 4/5/8 and their checkboxes) is hidden by default and only shown. */
BOOL gMB_SettingsVisible = FALSE;

int gMB_HoveredButton = 0; /* 0 = none, else 1. */

HDC gMB_MemDC = NULL;
HBITMAP gMB_MemBmp = NULL;
int gMB_MemW = 0, gMB_MemH = 0;

/* ---- loading + bbox helpers ----. */

GpImage *MENU_LoadPngResource(const char *resourceName) {
    HMODULE hMod = GetModuleHandleA(NULL);
    HRSRC hRes = FindResourceA(hMod, resourceName, RT_RCDATA);
    if (!hRes) return NULL;
    HGLOBAL hData = LoadResource(hMod, hRes);
    if (!hData) return NULL;
    DWORD size = SizeofResource(hMod, hRes);
    void *pData = LockResource(hData);
    if (!pData) return NULL;

    HGLOBAL hBuf = GlobalAlloc(GMEM_MOVEABLE, size);
    void *pBuf = GlobalLock(hBuf);
    memcpy(pBuf, pData, size);
    GlobalUnlock(hBuf);

    IStream *stream = NULL;
    CreateStreamOnHGlobal(hBuf, TRUE, &stream);

    GpImage *img = NULL;
    GdipLoadImageFromStream(stream, &img);
    stream->lpVtbl->Release(stream);
    return img;
}

/* Scans the image's alpha channel for the tightest non-transparent bounding box. */
void MENU_ComputeBBox(GpImage *img, RECT *bbox) {
    bbox->left = bbox->top = bbox->right = bbox->bottom = 0;
    if (!img) return;

    UINT w = 0, h = 0;
    GdipGetImageWidth(img, &w);
    GdipGetImageHeight(img, &h);
    if (w == 0 || h == 0) return;

    GpBitmap *bmp = (GpBitmap *)img;
    BitmapData bd;
    GpRect rect = {0, 0, (INT)w, (INT)h};
    if (GdipBitmapLockBits(bmp, &rect, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) {
        bbox->right = w; bbox->bottom = h;
        return;
    }

    int minX = (int)w, minY = (int)h, maxX = -1, maxY = -1;
    BYTE *base = (BYTE *)bd.Scan0;
    /* Sample every 2nd pixel for speed. */
    for (UINT y = 0; y < h; y += 2) {
        UINT32 *row = (UINT32 *)(base + (size_t)y * bd.Stride);
        for (UINT x = 0; x < w; x += 2) {
            BYTE alpha = (BYTE)((row[x] >> 24) & 0xFF);
            if (alpha > 10) {
                if ((int)x < minX) minX = (int)x;
                if ((int)x > maxX) maxX = (int)x;
                if ((int)y < minY) minY = (int)y;
                if ((int)y > maxY) maxY = (int)y;
            }
        }
    }
    GdipBitmapUnlockBits(bmp, &bd);

    if (maxX < 0) { bbox->left = bbox->top = bbox->right = bbox->bottom = 0; }
    else {
        bbox->left = minX; bbox->top = minY;
        bbox->right = maxX + 2; bbox->bottom = maxY + 2;
    }
}

void MENU_LoadImg(MenuImg *m, const char *resourceName) {
    m->img = MENU_LoadPngResource(resourceName);
    m->valid = (m->img != NULL);
    if (m->valid) MENU_ComputeBBox(m->img, &m->bbox);
}

GpImage *MENU_LoadPngFile(const char *path) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, path, -1, NULL, 0);
    WCHAR *wpath = (WCHAR *)malloc(sizeof(WCHAR) * wlen);
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, wlen);
    GpImage *img = NULL;
    GdipLoadImageFromFile(wpath, &img);
    free(wpath);
    return img;
}

/* ---- GIF playback ----. */

void MENU_GifLoad(MenuGif *g, GpImage *img) {
    memset(g, 0, sizeof(*g));
    g->img = img;
    if (!img) return;
    GdipImageGetFrameCount(img, &MENU_FrameDimensionTime, &g->frameCount);
    if (g->frameCount == 0) g->frameCount = 1;

    g->delaysMs = (UINT *)malloc(sizeof(UINT) * g->frameCount);
    UINT propSize = 0;
    if (GdipGetPropertyItemSize(img, MENU_PROPERTY_TAG_FRAME_DELAY, &propSize) == Ok && propSize > 0) {
        PropertyItem *item = (PropertyItem *)malloc(propSize);
        if (GdipGetPropertyItem(img, MENU_PROPERTY_TAG_FRAME_DELAY, propSize, item) == Ok) {
            LONG *vals = (LONG *)item->value;
            for (UINT i = 0; i < g->frameCount; i++) {
                UINT idx = (item->length / sizeof(LONG) > i) ? i : 0;
                g->delaysMs[i] = (UINT)(vals[idx] * 10); /* 1/100s -> ms. */
                if (g->delaysMs[i] < 20) g->delaysMs[i] = 30;
            }
        } else {
            for (UINT i = 0; i < g->frameCount; i++) g->delaysMs[i] = 100;
        }
        free(item);
    } else {
        for (UINT i = 0; i < g->frameCount; i++) g->delaysMs[i] = 100;
    }

    /* Pre-decode every frame once, at a resolution that's still sharp when scaled up for. */
    g->cacheW = 1280;
    g->cacheH = (int)(800.0 * MENU_NATIVE_H / MENU_NATIVE_W);
    g->cachedFrames = (GpBitmap **)malloc(sizeof(GpBitmap *) * g->frameCount);
    for (UINT i = 0; i < g->frameCount; i++) {
        GdipImageSelectActiveFrame(img, &MENU_FrameDimensionTime, i);
        GpBitmap *frameBmp = NULL;
        GdipCreateBitmapFromScan0(g->cacheW, g->cacheH, 0, PixelFormat32bppARGB, NULL, &frameBmp);
        GpGraphics *fg = NULL;
        GdipGetImageGraphicsContext((GpImage *)frameBmp, &fg);
        GdipSetInterpolationMode(fg, InterpolationModeBilinear);
        GdipSetCompositingQuality(fg, CompositingQualityHighSpeed);
        GdipDrawImageRectI(fg, img, 0, 0, g->cacheW, g->cacheH);
        GdipDeleteGraphics(fg);
        g->cachedFrames[i] = frameBmp;
    }

    g->currentFrame = 0;
    g->playing = FALSE;
}

void MENU_GifStart(MenuGif *g) {
    if (!g->img) return;
    g->currentFrame = 0;
    g->playing = TRUE;
    g->lastTickMs = GetTickCount();
}

/* Advances the gif by however much time has passed. */
BOOL MENU_GifTick(MenuGif *g) {
    if (!g->playing || !g->img) return FALSE;
    DWORD now = GetTickCount();
    DWORD elapsed = now - g->lastTickMs;
    if (elapsed < g->delaysMs[g->currentFrame]) return FALSE;

    /* Advance by however many frames' worth of real time have actually passed, not just one. */
    while (elapsed >= g->delaysMs[g->currentFrame]) {
        elapsed -= g->delaysMs[g->currentFrame];
        g->currentFrame++;
        if (g->currentFrame >= g->frameCount) {
            g->playing = FALSE;
            g->currentFrame = g->frameCount - 1;
            g->lastTickMs = now;
            return TRUE;
        }
    }
    g->lastTickMs = now - elapsed; /* keep the leftover fractional time. */
    return FALSE;
}

/* Current frame's pre-decoded bitmap, ready to draw directly. */
GpImage *MENU_GifCurrentFrame(MenuGif *g) {
    if (!g->cachedFrames || g->frameCount == 0) return g->img;
    return (GpImage *)g->cachedFrames[g->currentFrame];
}

/* ---- cycling images: folder scan with embedded fallback ----. */

void MENU_LoadCycleImages(void) {
    gMB_CycleCount = 0;

    char ownDir[MAX_PATH];
    GetModuleFileNameA(NULL, ownDir, MAX_PATH);
    char *lastSlash = strrchr(ownDir, '\\');
    if (lastSlash) *lastSlash = '\0';

    char folderPattern[MAX_PATH + 32];
    snprintf(folderPattern, sizeof(folderPattern), "%s\\MenuImages\\*.png", ownDir);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(folderPattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (gMB_CycleCount >= MENU_MAX_CYCLE_IMAGES) break;

            char fullPath[MAX_PATH * 2];
            snprintf(fullPath, sizeof(fullPath), "%s\\MenuImages\\%s", ownDir, fd.cFileName);

            MenuImg *slot = &gMB_CycleNormal[gMB_CycleCount];
            slot->img = MENU_LoadPngFile(fullPath);
            slot->valid = (slot->img != NULL);
            if (slot->valid) MENU_ComputeBBox(slot->img, &slot->bbox);

            /* matching "<name>_inverted.png" in the Inverted subfolder. */
            char baseName[MAX_PATH];
            strncpy(baseName, fd.cFileName, sizeof(baseName) - 1);
            baseName[sizeof(baseName) - 1] = '\0';
            char *dot = strrchr(baseName, '.');
            if (dot) *dot = '\0';

            char invPath[MAX_PATH * 2];
            snprintf(invPath, sizeof(invPath), "%s\\MenuImages\\Inverted\\%s_inverted.png", ownDir, baseName);
            DWORD attrs = GetFileAttributesA(invPath);
            MenuImg *invSlot = &gMB_CycleInverted[gMB_CycleCount];
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                invSlot->img = MENU_LoadPngFile(invPath);
                invSlot->valid = (invSlot->img != NULL);
                if (invSlot->valid) MENU_ComputeBBox(invSlot->img, &invSlot->bbox);
            } else {
                invSlot->valid = 0;
            }

            if (slot->valid) gMB_CycleCount++;
        } while (FindNextFileA(hFind, &fd) && gMB_CycleCount < MENU_MAX_CYCLE_IMAGES);
        FindClose(hFind);
    }

    if (gMB_CycleCount == 0) {
        /* Embedded fallback: Image 1/2/3, with Image 2 having an inverted click variant. */
        MENU_LoadImg(&gMB_CycleNormal[0], "IDP_IMAGE_1_PNG");
        gMB_CycleInverted[0].valid = 0;
        MENU_LoadImg(&gMB_CycleNormal[1], "IDP_IMAGE_2_PNG");
        MENU_LoadImg(&gMB_CycleInverted[1], "IDP_IMAGE_2_INVERTED_PNG");
        MENU_LoadImg(&gMB_CycleNormal[2], "IDP_IMAGE_3_PNG");
        gMB_CycleInverted[2].valid = 0;
        gMB_CycleCount = 3;
    }

    gMB_CycleIndex = 0;
    gMB_CycleShowInverted = FALSE;
    gMB_CycleLastSwitchMs = GetTickCount();
}

/* ---- one-time init ----. */

void MENU_InitOnce(void) {
    MENU_LoadImg(&gMB_Background, "IDP_BACKGROUND_PNG");
    MENU_LoadImg(&gMB_City, "IDP_CITY_PNG");
    MENU_LoadImg(&gMB_Moon, "IDP_MOON_PNG");
    MENU_LoadImg(&gMB_DavidMemoir, "IDP_DAVID_MEMOIR_PNG");

    MENU_LoadImg(&gMB_Button[1], "IDP_BUTTON_1_PNG");
    MENU_LoadImg(&gMB_ButtonInv[1], "IDP_BUTTON_1_INVERTED_PNG");
    MENU_LoadImg(&gMB_Button[2], "IDP_BUTTON_2_PNG");
    MENU_LoadImg(&gMB_ButtonInv[2], "IDP_BUTTON_2_INVERTED_PNG");
    MENU_LoadImg(&gMB_Button[3], "IDP_BUTTON_3_PNG");
    MENU_LoadImg(&gMB_ButtonInv[3], "IDP_BUTTON_3_INVERTED_PNG");
    MENU_LoadImg(&gMB_Button[4], "IDP_BUTTON_4_PNG");
    MENU_LoadImg(&gMB_Button[5], "IDP_BUTTON_5_PNG");
    MENU_LoadImg(&gMB_Button[6], "IDP_BUTTON_6_PNG");
    MENU_LoadImg(&gMB_Button[7], "IDP_BUTTON_7_PNG");
    MENU_LoadImg(&gMB_Button[8], "IDP_BUTTON_8_PNG");
    MENU_LoadImg(&gMB_Button[9], "IDP_BUTTON_9_PNG");
    MENU_LoadImg(&gMB_ButtonInv[9], "IDP_BUTTON_9_INVERTED_PNG");

    MENU_LoadImg(&gMB_Check4, "IDP_CHECK__BUTTON_4__PNG");
    MENU_LoadImg(&gMB_Checked4, "IDP_CHECKED__BUTTON_4__PNG");
    MENU_LoadImg(&gMB_Check5, "IDP_CHECK__BUTTON_5__PNG");
    MENU_LoadImg(&gMB_Checked5, "IDP_CHECKED__BUTTON_5__PNG");
    MENU_LoadImg(&gMB_Check8, "IDP_CHECK__BUTTON_8__PNG");
    MENU_LoadImg(&gMB_Checked8, "IDP_CHECKED__BUTTON_8__PNG");

    MENU_LoadImg(&gMB_Settings, "IDP_SETTINGS_PNG");

    MENU_LoadImg(&gMB_Title, "IDP_TITLE_PNG");
    MENU_LoadImg(&gMB_TitleInv, "IDP_TITLE_INVERTED_PNG");

    MENU_GifLoad(&gMB_GifTitleN2I, MENU_LoadPngResource("IDP_NORMAL_TO_INVERTED_TITLE_GIF"));
    MENU_GifLoad(&gMB_GifTitleI2N, MENU_LoadPngResource("IDP_INVERTED_TO_NORMAL_TITLE_GIF"));
    MENU_GifLoad(&gMB_GifLaunch, MENU_LoadPngResource("IDP_DAVID_GIF"));

    MENU_LoadCycleImages();
}

/* ---- layout: uniform scale + letterbox to fit window ----. */

typedef struct { int x, y, w, h; double scale; } MenuLayout;

MenuLayout MENU_GetLayout(int clientW, int clientH) {
    MenuLayout L;
    double scaleX = (double)clientW / MENU_NATIVE_W;
    double scaleY = (double)clientH / MENU_NATIVE_H;
    L.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (L.scale <= 0) L.scale = 0.01;
    L.w = (int)(MENU_NATIVE_W * L.scale);
    L.h = (int)(MENU_NATIVE_H * L.scale);
    L.x = (clientW - L.w) / 2;
    L.y = (clientH - L.h) / 2;
    return L;
}

void MENU_NativeToScreen(MenuLayout L, RECT native, RECT *out) {
    out->left = L.x + (int)(native.left * L.scale);
    out->top = L.y + (int)(native.top * L.scale);
    out->right = L.x + (int)(native.right * L.scale);
    out->bottom = L.y + (int)(native.bottom * L.scale);
}

BOOL MENU_ScreenToNative(MenuLayout L, int sx, int sy, int *nx, int *ny) {
    if (L.scale <= 0) return FALSE;
    *nx = (int)((sx - L.x) / L.scale);
    *ny = (int)((sy - L.y) / L.scale);
    return TRUE;
}

BOOL MENU_PtInBBox(int nx, int ny, RECT *bbox) {
    return (nx >= bbox->left && nx < bbox->right && ny >= bbox->top && ny < bbox->bottom);
}

/* ---- drawing ----. */

void MENU_DrawImg(GpGraphics *g, MenuImg *m, MenuLayout L) {
    if (!m->valid) return;
    GdipDrawImageRectI(g, m->img, L.x, L.y, L.w, L.h);
}

HDC gMB_CacheDC = NULL;
HBITMAP gMB_CacheBmp = NULL;
int gMB_CacheW = 0, gMB_CacheH = 0;
BOOL gMB_CacheDirty = TRUE;

void MENU_InvalidateCache(void) { gMB_CacheDirty = TRUE; }

void MENU_Paint(HWND hwnd, HDC hdc, RECT clientRect) {
    int cw = clientRect.right, ch = clientRect.bottom;
    if (cw <= 0 || ch <= 0) return;

    /* Offscreen buffer to avoid flicker from this many layered draws. */
    if (!gMB_MemDC || gMB_MemW != cw || gMB_MemH != ch) {
        if (gMB_MemBmp) DeleteObject(gMB_MemBmp);
        if (gMB_MemDC) DeleteDC(gMB_MemDC);
        gMB_MemDC = CreateCompatibleDC(hdc);
        gMB_MemBmp = CreateCompatibleBitmap(hdc, cw, ch);
        SelectObject(gMB_MemDC, gMB_MemBmp);
        gMB_MemW = cw; gMB_MemH = ch;
    }

    /* Full-screen launch gif completely covers everything underneath. */
    if (gMB_LaunchGifPlaying && gMB_GifLaunch.img) {
        GpGraphics *g = NULL;
        GdipCreateFromHDC(gMB_MemDC, &g);
        GdipSetInterpolationMode(g, InterpolationModeBilinear);
        GdipSetCompositingQuality(g, CompositingQualityHighSpeed);
        GdipDrawImageRectI(g, MENU_GifCurrentFrame(&gMB_GifLaunch), 0, 0, cw, ch);
        GdipDeleteGraphics(g);
        BitBlt(hdc, 0, 0, cw, ch, gMB_MemDC, 0, 0, SRCCOPY);
        return;
    }

    MenuLayout L = MENU_GetLayout(cw, ch);

    /* Static-layer cache: Background/Cycle-image/City/Moon/Buttons/Checks don't change during. */
    if (!gMB_CacheDC || gMB_CacheW != cw || gMB_CacheH != ch) {
        if (gMB_CacheBmp) DeleteObject(gMB_CacheBmp);
        if (gMB_CacheDC) DeleteDC(gMB_CacheDC);
        gMB_CacheDC = CreateCompatibleDC(hdc);
        gMB_CacheBmp = CreateCompatibleBitmap(hdc, cw, ch);
        SelectObject(gMB_CacheDC, gMB_CacheBmp);
        gMB_CacheW = cw; gMB_CacheH = ch;
        gMB_CacheDirty = TRUE;
    }

    if (gMB_CacheDirty) {
        RECT fillR = {0, 0, cw, ch};
        FillRect(gMB_CacheDC, &fillR, (HBRUSH)GetStockObject(BLACK_BRUSH));

        GpGraphics *cg = NULL;
        GdipCreateFromHDC(gMB_CacheDC, &cg);
        GdipSetInterpolationMode(cg, InterpolationModeBilinear);
        GdipSetCompositingQuality(cg, CompositingQualityHighSpeed);
        GdipSetSmoothingMode(cg, SmoothingModeNone);
        GdipSetPixelOffsetMode(cg, PixelOffsetModeHighSpeed);

        MENU_DrawImg(cg, &gMB_Background, L);
        if (gMB_CycleCount > 0) {
            MenuImg *cur = &gMB_CycleNormal[gMB_CycleIndex];
            if (gMB_CycleShowInverted && gMB_CycleInverted[gMB_CycleIndex].valid) {
                cur = &gMB_CycleInverted[gMB_CycleIndex];
            }
            MENU_DrawImg(cg, cur, L);
        }
        MENU_DrawImg(cg, &gMB_City, L);
        MENU_DrawImg(cg, &gMB_Moon, L);
        for (int i = 1; i <= 9; i++) {
            if (i == 4 || i == 5 || i == 8) continue; /* part of the settings panel, drawn below. */
            MenuImg *img = &gMB_Button[i];
            if (gMB_HoveredButton == i && gMB_ButtonInv[i].valid) img = &gMB_ButtonInv[i];
            MENU_DrawImg(cg, img, L);
        }
        if (gMB_SettingsVisible) {
            MENU_DrawImg(cg, &gMB_Button[4], L);
            MENU_DrawImg(cg, &gMB_Button[5], L);
            MENU_DrawImg(cg, &gMB_Button[8], L);
            MENU_DrawImg(cg, gMB_Check4On ? &gMB_Checked4 : &gMB_Check4, L);
            MENU_DrawImg(cg, gMB_Check5On ? &gMB_Checked5 : &gMB_Check5, L);
            MENU_DrawImg(cg, gMB_Check8On ? &gMB_Checked8 : &gMB_Check8, L);
        }
        MENU_DrawImg(cg, &gMB_Settings, L);
        /* David Memoir's drawn area (bottom-right) never overlaps Title's (top-left), verified. */
        MENU_DrawImg(cg, &gMB_DavidMemoir, L);
        GdipDeleteGraphics(cg);
        gMB_CacheDirty = FALSE;
    }

    BitBlt(gMB_MemDC, 0, 0, cw, ch, gMB_CacheDC, 0, 0, SRCCOPY);

    GpGraphics *g = NULL;
    GdipCreateFromHDC(gMB_MemDC, &g);
    GdipSetInterpolationMode(g, InterpolationModeBilinear);
    GdipSetCompositingQuality(g, CompositingQualityHighSpeed);

    /* Title: static image, or the active gif frame while animating. */
    if (gMB_TitleState == MENU_TITLE_PLAYING_N2I && gMB_GifTitleN2I.img) {
        GdipDrawImageRectI(g, MENU_GifCurrentFrame(&gMB_GifTitleN2I), L.x, L.y, L.w, L.h);
    } else if (gMB_TitleState == MENU_TITLE_PLAYING_I2N && gMB_GifTitleI2N.img) {
        GdipDrawImageRectI(g, MENU_GifCurrentFrame(&gMB_GifTitleI2N), L.x, L.y, L.w, L.h);
    } else if (gMB_TitleState == MENU_TITLE_INVERTED) {
        MENU_DrawImg(g, &gMB_TitleInv, L);
    } else {
        MENU_DrawImg(g, &gMB_Title, L);
    }

    GdipDeleteGraphics(g);

    BitBlt(hdc, 0, 0, cw, ch, gMB_MemDC, 0, 0, SRCCOPY);
}

/* ------------------------------------------------------------------ */
/* New menu interaction: hover, click dispatch, animation ticking. */
/* ------------------------------------------------------------------ */

#define MENU_TIMER_ANIM 950

/* Forward declarations of launcher actions this wires into (defined elsewhere in the merged file) */
void RunNativeConflictScan(void);
void BrowseForArchivesFolder(void);
void SwitchToSaveEditor(HWND hwnd);
int RunRedModDeploy(void);
void LaunchGame(int deployRequested, int dynamicPriority);
extern HWND hStatusText;

HWND hConsoleCloseBtn = NULL;
#define ID_CONSOLE_CLOSE 112

void MENU_UpdateConsoleVisibility(HWND hwnd) {
    if (!hStatusText) return;
    ShowWindow(hStatusText, gMB_ConsoleVisible ? SW_SHOW : SW_HIDE);
    if (gMB_ConsoleVisible) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int consoleLeft = 20, consoleTop = 170;
        int consoleRight = rc.right - 20, consoleBottom = rc.bottom - 20;
        MoveWindow(hStatusText, consoleLeft, consoleTop,
            consoleRight - consoleLeft, consoleBottom - consoleTop, TRUE);

        /* A real "Close" button living on top of the console itself. */
        if (!hConsoleCloseBtn) {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            hConsoleCloseBtn = CreateWindowA("BUTTON", "Close Console",
                WS_CHILD | BS_PUSHBUTTON,
                consoleRight - 110, consoleTop - 30, 110, 26,
                hwnd, (HMENU)ID_CONSOLE_CLOSE, NULL, NULL);
            SendMessageA(hConsoleCloseBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        } else {
            MoveWindow(hConsoleCloseBtn, consoleRight - 110, consoleTop - 30, 110, 26, TRUE);
        }
        ShowWindow(hConsoleCloseBtn, SW_SHOW);
        BringWindowToTop(hConsoleCloseBtn);
    } else if (hConsoleCloseBtn) {
        ShowWindow(hConsoleCloseBtn, SW_HIDE);
    }
}

void MENU_StartLaunch(HWND hwnd) {
    /* Deploy immediately (so any status text queued shows once the console is opened) then. */
    int deploy = gMB_Check4On;
    int dynamicPriority = gMB_Check5On;
    int skipLoadingScreen = gMB_Check8On;

    int deployFailed = 0;
    if (deploy) deployFailed = RunRedModDeploy();

    if (!deployFailed) {
        if (skipLoadingScreen) {
            LaunchGame(0 /* already deployed above. */, dynamicPriority);
            ShowWindow(hwnd, SW_MINIMIZE);
        } else {
            gMB_LaunchGifPendingLaunch = TRUE;
            gMB_LaunchGifPlaying = TRUE;
            MENU_GifStart(&gMB_GifLaunch);
            /* stash dynamicPriority choice for when the gif finishes. */
            gMB_Check5On = dynamicPriority;
        }
    } else {
        gMB_ConsoleVisible = TRUE;
        MENU_UpdateConsoleVisibility(hwnd);
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

/* Hit-tests topmost-first, matching the actual paint z-order. */
int MENU_HitTest(int nx, int ny) {
    if (gMB_Title.valid && gMB_TitleState != MENU_TITLE_PLAYING_N2I && gMB_TitleState != MENU_TITLE_PLAYING_I2N) {
        RECT *b = (gMB_TitleState == MENU_TITLE_INVERTED) ? &gMB_TitleInv.bbox : &gMB_Title.bbox;
        if (MENU_PtInBBox(nx, ny, b)) return 200;
    }
    if (gMB_Settings.valid && MENU_PtInBBox(nx, ny, &gMB_Settings.bbox)) return 103;
    if (gMB_SettingsVisible) {
        if (gMB_Check4.valid && MENU_PtInBBox(nx, ny, &gMB_Check4.bbox)) return 100;
        if (gMB_Check5.valid && MENU_PtInBBox(nx, ny, &gMB_Check5.bbox)) return 101;
        if (gMB_Check8.valid && MENU_PtInBBox(nx, ny, &gMB_Check8.bbox)) return 102;
    }

    for (int i = 9; i >= 1; i--) {
        if (gMB_Button[i].valid && MENU_PtInBBox(nx, ny, &gMB_Button[i].bbox)) return i;
    }

    if (gMB_CycleCount > 0) {
        MenuImg *cur = &gMB_CycleNormal[gMB_CycleIndex];
        if (MENU_PtInBBox(nx, ny, &cur->bbox)) return 300;
    }
    return 0;
}

void MENU_OnLButtonDown(HWND hwnd, int sx, int sy) {
    if (gMB_LaunchGifPlaying) return; /* ignore input during launch animation. */

    RECT rc;
    GetClientRect(hwnd, &rc);
    MenuLayout L = MENU_GetLayout(rc.right, rc.bottom);
    int nx, ny;
    if (!MENU_ScreenToNative(L, sx, sy, &nx, &ny)) return;

    int hit = MENU_HitTest(nx, ny);
    switch (hit) {
        case 1: /* check mod conflicts. */
            gMB_ConsoleVisible = TRUE;
            MENU_UpdateConsoleVisibility(hwnd);
            RunNativeConflictScan();
            break;
        case 2: /* launch game. */
            MENU_StartLaunch(hwnd);
            break;
        case 3: /* edit saves. */
            SwitchToSaveEditor(hwnd);
            break;
        case 6: /* toggle console. */
            gMB_ConsoleVisible = !gMB_ConsoleVisible;
            MENU_UpdateConsoleVisibility(hwnd);
            break;
        case 7: /* mod folder. */
            BrowseForArchivesFolder();
            break;
        case 9: /* mod manager - placeholder, not implemented yet. */
            break;
        case 100: /* deploy redmod check toggle. */
            gMB_Check4On = !gMB_Check4On;
            MENU_InvalidateCache();
            break;
        case 101: /* dynamic priority check toggle. */
            gMB_Check5On = !gMB_Check5On;
            MENU_InvalidateCache();
            break;
        case 102: /* skip loading screen check toggle. */
            gMB_Check8On = !gMB_Check8On;
            MENU_InvalidateCache();
            break;
        case 103: /* settings panel visibility toggle. */
            gMB_SettingsVisible = !gMB_SettingsVisible;
            MENU_InvalidateCache();
            break;
        case 200: /* title click. */
            if (gMB_TitleState == MENU_TITLE_NORMAL) {
                gMB_TitleState = MENU_TITLE_PLAYING_N2I;
                MENU_GifStart(&gMB_GifTitleN2I);
            } else if (gMB_TitleState == MENU_TITLE_INVERTED) {
                gMB_TitleState = MENU_TITLE_PLAYING_I2N;
                MENU_GifStart(&gMB_GifTitleI2N);
            }
            break;
        case 300: /* cycle image click. */
            if (gMB_CycleInverted[gMB_CycleIndex].valid) {
                gMB_CycleShowInverted = !gMB_CycleShowInverted;
                MENU_InvalidateCache();
            }
            break;
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

void MENU_OnMouseMove(HWND hwnd, int sx, int sy) {
    if (gMB_LaunchGifPlaying) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    MenuLayout L = MENU_GetLayout(rc.right, rc.bottom);
    int nx, ny;
    if (!MENU_ScreenToNative(L, sx, sy, &nx, &ny)) return;

    int newHover = 0;
    for (int i = 9; i >= 1; i--) {
        if (gMB_Button[i].valid && MENU_PtInBBox(nx, ny, &gMB_Button[i].bbox)) { newHover = i; break; }
    }
    if (newHover != gMB_HoveredButton) {
        gMB_HoveredButton = newHover;
        MENU_InvalidateCache();
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

/* Advances all active animations. */
BOOL MENU_Tick(HWND hwnd) {
    BOOL changed = FALSE;

    if (gMB_TitleState == MENU_TITLE_PLAYING_N2I) {
        if (MENU_GifTick(&gMB_GifTitleN2I)) {
            gMB_TitleState = MENU_TITLE_INVERTED;
        }
        changed = TRUE;
    } else if (gMB_TitleState == MENU_TITLE_PLAYING_I2N) {
        if (MENU_GifTick(&gMB_GifTitleI2N)) {
            gMB_TitleState = MENU_TITLE_NORMAL;
        }
        changed = TRUE;
    }

    if (gMB_LaunchGifPlaying) {
        if (MENU_GifTick(&gMB_GifLaunch)) {
            gMB_LaunchGifPlaying = FALSE;
            if (gMB_LaunchGifPendingLaunch) {
                gMB_LaunchGifPendingLaunch = FALSE;
                LaunchGame(0 /* already deployed above. */, gMB_Check5On);
                ShowWindow(hwnd, SW_MINIMIZE);
            }
        }
        changed = TRUE;
    }

    if (!gMB_LaunchGifPlaying && gMB_CycleCount > 0) {
        DWORD now = GetTickCount();
        if (now - gMB_CycleLastSwitchMs >= MENU_CYCLE_INTERVAL_MS) {
            gMB_CycleLastSwitchMs = now;
            gMB_CycleIndex = (gMB_CycleIndex + 1) % gMB_CycleCount;
            MENU_InvalidateCache();
            gMB_CycleShowInverted = FALSE; /* always resumes normal on its next turn. */
            changed = TRUE;
        }
    }

    return changed;
}

/* Switches the (single, shared) window's content between the launcher's own controls and. */
void SwitchToSaveEditor(HWND hwnd) {
    gViewMode = MODE_SAVEEDITOR;
    ShowWindow(hStatusText, SW_HIDE); /* launcher's own console. */
    if (hConsoleCloseBtn) ShowWindow(hConsoleCloseBtn, SW_HIDE);
    if (hButtonBack) ShowWindow(hButtonBack, SW_HIDE); /* SV2 draws+hit-tests its own Back to Launcher. */

    if (!gSaveEditorActivated) {
        SE_WndProc(hwnd, WM_CREATE, 0, 0);
        gSaveEditorActivated = TRUE;
        SetTimer(hwnd, SE_TIMER_STARTUP_RELAYOUT, 150, NULL);
    } else {
        for (int i = 0; i < MAX_PROFICIENCIES; i++) {
            if (SE_hEditBoxes[i]) ShowWindow(SE_hEditBoxes[i], SW_SHOW);
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        SendMessageA(hwnd, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
    }

    /* All of the old skin's visual controls are replaced by SV2's drawn art. */
    ShowWindow(SE_hSaveLabel, SW_HIDE);
    ShowWindow(SE_hPathBox, SW_HIDE);
    ShowWindow(SE_hHelpBtn, SW_HIDE);
    ShowWindow(SE_hLoadBtn, SW_HIDE);
    ShowWindow(SE_hSaveBtn, SW_HIDE);
    ShowWindow(SE_hInfoText, SW_HIDE);
    ShowWindow(SE_hStatusText, SW_HIDE);
    for (int i = 0; i < MAX_PROFICIENCIES; i++) {
        if (SE_hLabels[i]) ShowWindow(SE_hLabels[i], SW_HIDE);
    }

    SV2_RepositionStatBoxes(hwnd);
    if (SE_g_saveLoaded) SV2_PopulateFromSave();

    InvalidateRect(hwnd, NULL, TRUE);
}

void SwitchToLauncher(HWND hwnd) {
    gViewMode = MODE_LAUNCHER;

    ShowWindow(SE_hSaveLabel, SW_HIDE);
    ShowWindow(SE_hPathBox, SW_HIDE);
    ShowWindow(SE_hHelpBtn, SW_HIDE);
    ShowWindow(SE_hLoadBtn, SW_HIDE);
    ShowWindow(SE_hSaveBtn, SW_HIDE);
    ShowWindow(SE_hInfoText, SW_HIDE);
    ShowWindow(SE_hStatusText, SW_HIDE);
    for (int i = 0; i < MAX_PROFICIENCIES; i++) {
        if (SE_hEditBoxes[i]) ShowWindow(SE_hEditBoxes[i], SW_HIDE);
        if (SE_hLabels[i]) ShowWindow(SE_hLabels[i], SW_HIDE);
    }
    if (hButtonBack) ShowWindow(hButtonBack, SW_HIDE);
    extern HWND SV2_hLevelCredCaptionOverlay;
    if (SV2_hLevelCredCaptionOverlay) ShowWindow(SV2_hLevelCredCaptionOverlay, SW_HIDE);

    MENU_UpdateConsoleVisibility(hwnd); /* restores whatever the console's state was. */

    InvalidateRect(hwnd, NULL, TRUE);
}

/* ------------------------------------------------------------------ */
/* New image-based save editor (replaces SE_ system's rendering). */
/* All symbols prefixed SV2_ to avoid collisions. */
/* from saveengine. */
/* ------------------------------------------------------------------ */

#define SV2_NATIVE_W 1918
#define SV2_NATIVE_H 1008

typedef struct {
    GpImage *img;
    RECT bbox;
    int valid;
} SV2Img;

/* Static/structural layers (back to front, minus glass/text handled below) */
SV2Img SV2_Background, SV2_DavidsHead, SV2_DavidsJacket, SV2_Sandevistan;
SV2Img SV2_SaveLocationBtn, SV2_Shot, SV2_OutsideShapes, SV2_Image;
SV2Img SV2_LocationPin, SV2_LevelCredText, SV2_BackToLauncher;
SV2Img SV2_Months[12]; /* Jan..Dec. */

/* Glass panels: 14 stat glasses + Console/LoadSave/SaveChanges/Extra. */
#define SV2_GLASS_COUNT 18
typedef struct {
    const char *name;      /* matches SaveFile proficiency name, or a UI id. */
    const char *resNormal;
    const char *resInverted;
    SV2Img normal, inverted;
    double numCx, numCy;   /* native px - where the editable number goes (stats only) */
} SV2Glass;

SV2Glass SV2_Glasses[SV2_GLASS_COUNT] = {
    {"Engineering",           "SV2_ENGINEERING_GLASS",       "SV2_ENGINEERING_GLASS_INV",       {0}, {0}, 1575, 274},
    {"CombatHacking",         "SV2_COMBAT_HACKING_GLASS",    "SV2_COMBAT_HACKING_GLASS_INV",    {0}, {0}, 1338, 610},
    {"Hacking",               "SV2_HACKING_GLASS",           "SV2_HACKING_GLASS_INV",           {0}, {0}, 1796, 273},
    {"TechnicalAbilitySkill",  "SV2_TECHNICAL_ABILITY_GLASS", "SV2_TECHNICAL_ABILITY_GLASS_INV", {0}, {0}, 1589, 375},
    {"Kenjutsu",              "SV2_KENJUTSU_GLASS",          "SV2_KENJUTSU_GLASS_INV",          {0}, {0}, 1666, 604},
    {"Demolition",            "SV2_DEMOLITION_GLASS",        "SV2_DEMOLITION_GLASS_INV",        {0}, {0}, 975, 46},
    {"IntelligenceSkill",     "SV2_INTELLIGENCE_GLASS",      "SV2_INTELLIGENCE_GLASS_INV",      {0}, {0}, 889, 380},
    {"CoolSkill",             "SV2_COOL_GLASS",              "SV2_COOL_GLASS_INV",              {0}, {0}, 1045, 168},
    {"ReflexesSkill",         "SV2_REFLEXES_GLASS",          "SV2_REFLEXES_GLASS_INV",          {0}, {0}, 1125, 475},
    {"StrengthSkill",         "SV2_STRENGTH_GLASS",          "SV2_STRENGTH_GLASS_INV",          {0}, {0}, 1317, 436},
    {"Gunslinger",            "SV2_GUNSLINGER_GLASS",        "SV2_GUNSLINGER_GLASS_INV",        {0}, {0}, 1315, 332},
    {"Crafting",              "SV2_CRAFTING_GLASS",          "SV2_CRAFTING_GLASS_INV",          {0}, {0}, 1076, 315},
    {"Espionage",             "SV2_ESPIONAGE_GLASS",         "SV2_ESPIONAGE_GLASS_INV",         {0}, {0}, 1692, 494},
    {"Stealth",               "SV2_STEALTH_GLASS",           "SV2_STEALTH_GLASS_INV",           {0}, {0}, 1316, 157},
    /* Non-stat UI glass panels. */
    {"__console",             "SV2_CONSOLE_GLASS",           "SV2_CONSOLE_GLASS_INV",           {0}, {0}, 0, 0},
    {"__loadsave",            "SV2_LOAD_SAVE_GLASS",         "SV2_LOAD_SAVE_GLASS_INV",         {0}, {0}, 0, 0},
    {"__savechanges",         "SV2_SAVE_CHANGES_GLASS",      "SV2_SAVE_CHANGES_GLASS_INV",      {0}, {0}, 0, 0},
    {"__extra",               "SV2_EXTRA_GLASS",             "SV2_EXTRA_GLASS_INV",             {0}, {0}, 0, 0},
};

/* ---- Adjustable layout system: drag to reposition, scroll wheel to resize, dump to. */
typedef struct {
    const char *name;
    double cx, cy;
    double angle;
    BOOL hasAngle;
    double fontPx;    /* base size at scale=1.0. */
    BOOL isBand;      /* TIME/LED: dragging Y shifts the whole top/bottom band. */
} SV2LayoutItem;

enum {
    SV2_LI_LEVELNUM, SV2_LI_CREDNUM, SV2_LI_MISSION, SV2_LI_LOCATION,
    SV2_LI_DATE, SV2_LI_TITLE, SV2_LI_LIFEPATH, SV2_LI_TIME, SV2_LI_LED, SV2_LI_PIN,
    SV2_LI_COUNT
};

SV2LayoutItem SV2_LayoutItems[SV2_LI_COUNT] = {
    {"LEVEL_NUM", 116, 937, 0, FALSE, 138, FALSE},
    {"CRED_NUM",  337, 937, 0, FALSE, 138, FALSE},
    {"MISSION",   153, 245, -4.0, TRUE, 34, FALSE},
    {"LOCATION",  289, 827, 0, FALSE, 44, FALSE},
    {"DATE",      282, 516, 0, FALSE, 32, FALSE},
    {"TITLE",     255, 69, 12.0, TRUE, 18, FALSE},
    {"LIFEPATH",  376, 100, -13.0, TRUE, 105, FALSE},
    {"TIME",      68, 550, -90.0, TRUE, 133, TRUE},
    {"LED",       141, 550, -90.0, TRUE, 18, TRUE},
    {"PIN",       46, 824, 0, FALSE, 30, FALSE},
};

double SV2_TimeTop = 334, SV2_TimeBottom = 766;
double SV2_LedTop = 335, SV2_LedBottom = 766;
double SV2_StatNumFontPx = 22; /* shared base size for all 14 stat numbers. */

#define SV2_MISSION_SHADOW_DX 8
#define SV2_MISSION_SHADOW_DY 8

/* Accessor macros so the rest of the code can keep reading these as simple names, now. */
#define SV2_LEVEL_NUM_CX SV2_LayoutItems[SV2_LI_LEVELNUM].cx
#define SV2_LEVEL_NUM_CY SV2_LayoutItems[SV2_LI_LEVELNUM].cy
#define SV2_CRED_NUM_CX SV2_LayoutItems[SV2_LI_CREDNUM].cx
#define SV2_CRED_NUM_CY SV2_LayoutItems[SV2_LI_CREDNUM].cy
#define SV2_MISSION_CX SV2_LayoutItems[SV2_LI_MISSION].cx
#define SV2_MISSION_CY SV2_LayoutItems[SV2_LI_MISSION].cy
#define SV2_MISSION_ANGLE SV2_LayoutItems[SV2_LI_MISSION].angle
#define SV2_LOCATION_TEXT_CX SV2_LayoutItems[SV2_LI_LOCATION].cx
#define SV2_LOCATION_TEXT_CY SV2_LayoutItems[SV2_LI_LOCATION].cy
#define SV2_DATE_CX SV2_LayoutItems[SV2_LI_DATE].cx
#define SV2_DATE_CY SV2_LayoutItems[SV2_LI_DATE].cy
#define SV2_TITLE_CX SV2_LayoutItems[SV2_LI_TITLE].cx
#define SV2_TITLE_CY SV2_LayoutItems[SV2_LI_TITLE].cy
#define SV2_TITLE_ANGLE SV2_LayoutItems[SV2_LI_TITLE].angle
#define SV2_LIFEPATH_CX SV2_LayoutItems[SV2_LI_LIFEPATH].cx
#define SV2_LIFEPATH_CY SV2_LayoutItems[SV2_LI_LIFEPATH].cy
#define SV2_LIFEPATH_ANGLE SV2_LayoutItems[SV2_LI_LIFEPATH].angle
#define SV2_TIME_CX SV2_LayoutItems[SV2_LI_TIME].cx
#define SV2_TIME_TOP SV2_TimeTop
#define SV2_TIME_BOTTOM SV2_TimeBottom
#define SV2_LED_CX SV2_LayoutItems[SV2_LI_LED].cx
#define SV2_LED_TOP SV2_LedTop
#define SV2_LED_BOTTOM SV2_LedBottom


HWND SV2_hMainWnd = NULL;
#define SV2_COLORKEY RGB(255, 3, 253) /* deliberately unusual magenta. */
HWND SV2_hSaveLocBtn = NULL, SV2_hBackBtn = NULL;
int SV2_HoveredGlass = -1;
GpImage *SV2_ScreenshotMasked = NULL; /* pre-masked to Image.png's alpha shape. */

/* LED scroll state. */
double SV2_LedScrollY = 0;
DWORD SV2_LedLastTick = 0;
#define SV2_LED_SPEED_PX_PER_SEC 60.0

HDC SV2_MemDC = NULL;
HBITMAP SV2_MemBmp = NULL;
int SV2_MemW = 0, SV2_MemH = 0;

/* ---- helpers (image loading, bbox, layout) reuse the same approach as the main menu. */

void SV2_LoadImg(SV2Img *m, const char *resourceName) {
    m->img = MENU_LoadPngResource(resourceName);
    m->valid = (m->img != NULL);
    if (m->valid) MENU_ComputeBBox(m->img, &m->bbox);
}

HFONT SV2_MakeFont(const char *faceName, int heightPx, BOOL bold) {
    return CreateFontA(-heightPx, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, PROOF_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, faceName);
}

typedef struct { int x, y, w, h; double scale; } SV2Layout;

SV2Layout SV2_GetLayout(int clientW, int clientH) {
    SV2Layout L;
    double scaleX = (double)clientW / SV2_NATIVE_W;
    double scaleY = (double)clientH / SV2_NATIVE_H;
    L.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (L.scale <= 0) L.scale = 0.01;
    L.w = (int)(SV2_NATIVE_W * L.scale);
    L.h = (int)(SV2_NATIVE_H * L.scale);
    L.x = (clientW - L.w) / 2;
    L.y = (clientH - L.h) / 2;
    return L;
}

int SV2_MapX(SV2Layout L, double nx) { return L.x + (int)(nx * L.scale); }
int SV2_MapY(SV2Layout L, double ny) { return L.y + (int)(ny * L.scale); }
void SV2_NativeToScreen(SV2Layout L, RECT native, RECT *out) {
    out->left = SV2_MapX(L, native.left);
    out->top = SV2_MapY(L, native.top);
    out->right = SV2_MapX(L, native.right);
    out->bottom = SV2_MapY(L, native.bottom);
}

BOOL SV2_ScreenToNative(SV2Layout L, int sx, int sy, int *nx, int *ny) {
    if (L.scale <= 0) return FALSE;
    *nx = (int)((sx - L.x) / L.scale);
    *ny = (int)((sy - L.y) / L.scale);
    return TRUE;
}

BOOL SV2_PtInBBox(int nx, int ny, RECT *b) {
    return (nx >= b->left && nx < b->right && ny >= b->top && ny < b->bottom);
}

/* ---- screenshot: masked into Image.png's alpha shape once per load ----. */

void SV2_BuildMaskedScreenshot(GpImage *screenshot) {
    if (SV2_ScreenshotMasked) { GdipDisposeImage(SV2_ScreenshotMasked); SV2_ScreenshotMasked = NULL; }
    if (!screenshot || !SV2_Image.valid) return;

    RECT b = SV2_Image.bbox;
    int w = b.right - b.left, h = b.bottom - b.top;
    if (w <= 0 || h <= 0) return;

    GpBitmap *result = NULL;
    GdipCreateBitmapFromScan0(w, h, 0, PixelFormat32bppARGB, NULL, &result);

    /* Draw the screenshot (scaled to fill the mask's bbox) into a temp buffer, then the mask on. */
    GpBitmap *shotScaled = NULL;
    GdipCreateBitmapFromScan0(w, h, 0, PixelFormat32bppARGB, NULL, &shotScaled);
    GpGraphics *sg = NULL;
    GdipGetImageGraphicsContext((GpImage *)shotScaled, &sg);
    GdipSetInterpolationMode(sg, InterpolationModeBilinear);
    GdipDrawImageRectI(sg, screenshot, 0, 0, w, h);
    GdipDeleteGraphics(sg);

    BitmapData maskData, shotData, outData;
    GpRect r = {0, 0, w, h};
    GdipBitmapLockBits((GpBitmap *)SV2_Image.img, &(GpRect){b.left, b.top, w, h}, ImageLockModeRead, PixelFormat32bppARGB, &maskData);
    GdipBitmapLockBits(shotScaled, &r, ImageLockModeRead, PixelFormat32bppARGB, &shotData);
    GdipBitmapLockBits(result, &r, ImageLockModeWrite, PixelFormat32bppARGB, &outData);

    for (int y = 0; y < h; y++) {
        UINT32 *maskRow = (UINT32 *)((BYTE *)maskData.Scan0 + (size_t)y * maskData.Stride);
        UINT32 *shotRow = (UINT32 *)((BYTE *)shotData.Scan0 + (size_t)y * shotData.Stride);
        UINT32 *outRow = (UINT32 *)((BYTE *)outData.Scan0 + (size_t)y * outData.Stride);
        for (int x = 0; x < w; x++) {
            BYTE maskA = (BYTE)((maskRow[x] >> 24) & 0xFF);
            UINT32 shotPixel = shotRow[x];
            BYTE sr = (BYTE)((shotPixel >> 16) & 0xFF);
            BYTE sgc = (BYTE)((shotPixel >> 8) & 0xFF);
            BYTE sb = (BYTE)(shotPixel & 0xFF);
            outRow[x] = ((UINT32)maskA << 24) | ((UINT32)sr << 16) | ((UINT32)sgc << 8) | sb;
        }
    }

    GdipBitmapUnlockBits((GpBitmap *)SV2_Image.img, &maskData);
    GdipBitmapUnlockBits(shotScaled, &shotData);
    GdipBitmapUnlockBits(result, &outData);
    GdipDisposeImage((GpImage *)shotScaled);

    SV2_ScreenshotMasked = (GpImage *)result;
}


/* ------------------------------------------------------------------ */
/* Save editor v2: init, paint, interaction. */
/* ------------------------------------------------------------------ */

#define SV2_TIMER_ANIM 960

int SV2_MonthFromSaveInfo(void) {
    extern int SV2_g_month;
    if (SV2_g_month >= 1 && SV2_g_month <= 12) return SV2_g_month;
    SYSTEMTIME st;
    GetLocalTime(&st);
    return (int)st.wMonth;
}

int SV2_EditBoxGlassIndex[MAX_PROFICIENCIES]; /* which SV2_Glasses[] slot each SE_hEditBoxes[i] maps to, -1 if none. */
int SV2_LevelBoxIsHovered = 0; /* unused placeholder for symmetry. */

HWND SV2_hLevelCredCaptionOverlay = NULL;

void SV2_UpdateCaptionOverlay(SV2Layout L) {
    extern int SE_g_saveLoaded;
    if (!SV2_LevelCredText.valid || !SV2_hMainWnd) return;
    if (!SE_g_saveLoaded) {
        if (SV2_hLevelCredCaptionOverlay) ShowWindow(SV2_hLevelCredCaptionOverlay, SW_HIDE);
        return;
    }
    RECT b = SV2_LevelCredText.bbox;
    int w = (int)((b.right - b.left) * L.scale);
    int h = (int)((b.bottom - b.top) * L.scale);
    int x = SV2_MapX(L, b.left);
    /* No longer shifted down by its own height. */
    int y = SV2_MapY(L, b.top);
    if (w <= 0 || h <= 0) return;

    /* x,y above are client-area-relative (same convention as every other SV2_Map* call in this app) */
    POINT scr = { x, y };
    ClientToScreen(SV2_hMainWnd, &scr);
    int sx = scr.x, sy = scr.y;

    if (!SV2_hLevelCredCaptionOverlay) {
        SV2_hLevelCredCaptionOverlay = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
            "STATIC", "", WS_POPUP,
            sx, sy, w, h, SV2_hMainWnd, NULL, NULL, NULL);
    } else {
        SetWindowPos(SV2_hLevelCredCaptionOverlay, NULL, sx, sy, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; /* top-down. */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hBmp);

    GpGraphics *g = NULL;
    GdipCreateFromHDC(memDC, &g);
    GdipSetInterpolationMode(g, InterpolationModeBilinear);
    GdipSetCompositingMode(g, CompositingModeSourceCopy); /* keep real alpha, don't blend onto garbage bg. */
    GdipDrawImageRectRectI(g, SV2_LevelCredText.img, 0, 0, w, h,
        b.left, b.top, b.right - b.left, b.bottom - b.top, UnitPixel, NULL, NULL, NULL);
    GdipDeleteGraphics(g);

    /* UpdateLayeredWindow with ULW_ALPHA requires premultiplied alpha. */
    UINT32 *px = (UINT32 *)bits;
    for (int i = 0; i < w * h; i++) {
        BYTE a = (BYTE)((px[i] >> 24) & 0xFF);
        BYTE r = (BYTE)((px[i] >> 16) & 0xFF);
        BYTE gC = (BYTE)((px[i] >> 8) & 0xFF);
        BYTE bC = (BYTE)(px[i] & 0xFF);
        r = (BYTE)(r * a / 255); gC = (BYTE)(gC * a / 255); bC = (BYTE)(bC * a / 255);
        px[i] = ((UINT32)a << 24) | ((UINT32)r << 16) | ((UINT32)gC << 8) | bC;
    }

    POINT ptSrc = {0, 0};
    POINT ptDst = {sx, sy};
    SIZE sz = {w, h};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(SV2_hLevelCredCaptionOverlay, screenDC, &ptDst, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);

    ShowWindow(SV2_hLevelCredCaptionOverlay, SW_SHOW);
    SetWindowPos(SV2_hLevelCredCaptionOverlay, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void SV2_InitOnce(void) {
    SV2_LoadImg(&SV2_Background, "SV2_BACKGROUND");
    SV2_LoadImg(&SV2_DavidsHead, "SV2_DAVIDSHEAD");
    SV2_LoadImg(&SV2_DavidsJacket, "SV2_DAVIDSJACKET");
    SV2_LoadImg(&SV2_Sandevistan, "SV2_SANDEVISTAN");
    SV2_LoadImg(&SV2_SaveLocationBtn, "SV2_SAVE_LOCATION");
    SV2_LoadImg(&SV2_Shot, "SV2_SHOT");
    SV2_LoadImg(&SV2_OutsideShapes, "SV2_OUTSIDESHAPES");
    SV2_LoadImg(&SV2_Image, "SV2_IMAGE");
    SV2_LoadImg(&SV2_LocationPin, "SV2_LOCATION_PIN_ICON");
    SV2_LoadImg(&SV2_LevelCredText, "SV2_LEVEL_AND_STREET_CRED_TEXT");
    SV2_LoadImg(&SV2_BackToLauncher, "SV2_BACK_TO_LAUNCHER");

    const char *monthRes[12] = {
        "SV2_JANUARY", "SV2_FEBRUARY", "SV2_MARCH", "SV2_APRIL", "SV2_MAY", "SV2_JUNE",
        "SV2_JULY", "SV2_AUGUST", "SV2_SEPTEMBER", "SV2_OCTOBER", "SV2_NOVEMBER", "SV2_DECEMBER"
    };
    for (int i = 0; i < 12; i++) SV2_LoadImg(&SV2_Months[i], monthRes[i]);

    for (int i = 0; i < SV2_GLASS_COUNT; i++) {
        SV2_LoadImg(&SV2_Glasses[i].normal, SV2_Glasses[i].resNormal);
        SV2_LoadImg(&SV2_Glasses[i].inverted, SV2_Glasses[i].resInverted);
        SV2_BuildGlassAlphaMask(i);
    }

}

/* Repositions the SAME edit boxes SE_BuildProficiencyFields() already created and correctly. */
int SV2_ActiveEditBox = -1; /* proficiency index currently shown as a real edit box, -1 = none (all drawn as text) */

#define SV2_MSG_DEFERRED_HIDE (WM_APP + 137)

LRESULT CALLBACK SV2_EditBoxSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR subclassId, DWORD_PTR refData) {
    if (msg == WM_ERASEBKGND) return 1; /* suppress entirely. */
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        /* Enter confirms: shift focus away, which triggers WM_KILLFOCUS below and closes the edit. */
        SetFocus(SV2_hMainWnd);
        return 0;
    }
    if (msg == WM_CHAR && wParam == VK_RETURN) return 0; /* swallow the matching WM_CHAR too. */
    if (msg == WM_KILLFOCUS) {
        /* Editing finished. */
        if (GetDlgCtrlID(hwnd) - SE_ID_FIRST_EDIT == SV2_ActiveEditBox) {
            PostMessage(hwnd, SV2_MSG_DEFERRED_HIDE, 0, 0);
        }
    }
    if (msg == SV2_MSG_DEFERRED_HIDE) {
        /* This message is POSTED (async) from WM_KILLFOCUS above, so it can sit in the queue. */
        if (GetDlgCtrlID(hwnd) - SE_ID_FIRST_EDIT == SV2_ActiveEditBox) {
            ShowWindow(hwnd, SW_HIDE);
            SV2_ActiveEditBox = -1;
            InvalidateRect(SV2_hMainWnd, NULL, FALSE);
        }
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, SV2_EditBoxSubclassProc, subclassId);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void SV2_RepositionStatBoxes(HWND hwnd) {
    extern SaveFile SE_g_save;
    extern HWND SE_hEditBoxes[], SE_hLevelEditBox, SE_hCredEditBox;
    SV2_hMainWnd = hwnd;
    RECT rc; GetClientRect(hwnd, &rc);
    SV2Layout L = SV2_GetLayout(rc.right, rc.bottom);


    int statW = (int)(70 * L.scale), statH = (int)(34 * L.scale);

    int levelFontPx = (int)(SV2_LayoutItems[SV2_LI_LEVELNUM].fontPx * L.scale); if (levelFontPx < 10) levelFontPx = 10;
    int credFontPx = (int)(SV2_LayoutItems[SV2_LI_CREDNUM].fontPx * L.scale); if (credFontPx < 10) credFontPx = 10;
    int statFontPx = (int)(SV2_StatNumFontPx * L.scale); if (statFontPx < 6) statFontPx = 6;

    /* Cache these instead of creating (and leaking) fresh HFONTs on every single call. */
    static int s_cachedLevelPx = -1, s_cachedCredPx = -1, s_cachedStatPx = -1;
    static HFONT s_levelFont = NULL, s_credFont = NULL, s_statFont = NULL;
    if (levelFontPx != s_cachedLevelPx) {
        if (s_levelFont) DeleteObject(s_levelFont);
        s_levelFont = SV2_MakeFont("Limelight", levelFontPx, FALSE);
        s_cachedLevelPx = levelFontPx;
    }
    if (credFontPx != s_cachedCredPx) {
        if (s_credFont) DeleteObject(s_credFont);
        s_credFont = SV2_MakeFont("Limelight", credFontPx, FALSE);
        s_cachedCredPx = credFontPx;
    }
    if (statFontPx != s_cachedStatPx) {
        if (s_statFont) DeleteObject(s_statFont);
        s_statFont = SV2_MakeFont("Bodoni MT", statFontPx, FALSE);
        s_cachedStatPx = statFontPx;
    }
    HFONT levelFont = s_levelFont;
    HFONT credFont = s_credFont;
    HFONT statFont = s_statFont;

    int matchedCount = 0;
    for (int i = 0; i < SE_g_save.proficiencyCount; i++) {
        SV2_EditBoxGlassIndex[i] = -1;
        if (!SE_hEditBoxes[i]) {
            extern void AppendStatus(const char *);
            char msg[128];
            snprintf(msg, sizeof(msg), "[sv2] no edit box HWND for proficiency '%s' (index %d)",
                SE_g_save.proficiencies[i].name, i);
            AppendStatus(msg);
            continue;
        }
        SetWindowSubclass(SE_hEditBoxes[i], SV2_EditBoxSubclassProc, 1, 0);

        /* GDI's native EDIT control and DirectWrite center text vertically using different. */
        int yNudge = (int)(3 * L.scale);

        if (strcmp(SE_g_save.proficiencies[i].name, "Level") == 0) {
            int w = (int)(levelFontPx * 2.2), h = (int)(levelFontPx * 1.1);
            MoveWindow(SE_hEditBoxes[i], SV2_MapX(L, SV2_LEVEL_NUM_CX) - w / 2,
                SV2_MapY(L, SV2_LEVEL_NUM_CY) - h / 2 + yNudge, w, h, TRUE);
            SendMessageA(SE_hEditBoxes[i], WM_SETFONT, (WPARAM)levelFont, FALSE);
            ShowWindow(SE_hEditBoxes[i], (i == SV2_ActiveEditBox) ? SW_SHOW : SW_HIDE);
            if (i == SV2_ActiveEditBox) BringWindowToTop(SE_hEditBoxes[i]);
            continue;
        }
        if (strcmp(SE_g_save.proficiencies[i].name, "StreetCred") == 0) {
            int w = (int)(credFontPx * 2.2), h = (int)(credFontPx * 1.1);
            MoveWindow(SE_hEditBoxes[i], SV2_MapX(L, SV2_CRED_NUM_CX) - w / 2,
                SV2_MapY(L, SV2_CRED_NUM_CY) - h / 2 + yNudge, w, h, TRUE);
            SendMessageA(SE_hEditBoxes[i], WM_SETFONT, (WPARAM)credFont, FALSE);
            ShowWindow(SE_hEditBoxes[i], (i == SV2_ActiveEditBox) ? SW_SHOW : SW_HIDE);
            if (i == SV2_ActiveEditBox) BringWindowToTop(SE_hEditBoxes[i]);
            continue;
        }
        BOOL matched = FALSE;
        for (int gi = 0; gi < SV2_GLASS_COUNT; gi++) {
            if (strcmp(SV2_Glasses[gi].name, SE_g_save.proficiencies[i].name) == 0) {
                MoveWindow(SE_hEditBoxes[i], SV2_MapX(L, SV2_Glasses[gi].numCx) - statW / 2,
                    SV2_MapY(L, SV2_Glasses[gi].numCy) - statH / 2 + yNudge, statW, statH, TRUE);
                SendMessageA(SE_hEditBoxes[i], WM_SETFONT, (WPARAM)statFont, FALSE);
                /* Hidden by default. */
                ShowWindow(SE_hEditBoxes[i], (i == SV2_ActiveEditBox) ? SW_SHOW : SW_HIDE);
                if (i == SV2_ActiveEditBox) BringWindowToTop(SE_hEditBoxes[i]);
                SV2_EditBoxGlassIndex[i] = gi;
                matched = TRUE;
                matchedCount++;
                break;
            }
        }
        if (!matched) {
            extern void AppendStatus(const char *);
            char msg[128];
            snprintf(msg, sizeof(msg), "[sv2] proficiency name '%s' matched no glass panel",
                SE_g_save.proficiencies[i].name);
            AppendStatus(msg);
        }
    }

    SV2_UpdateCaptionOverlay(L);
}

void SV2_PopulateFromSave(void) {
    extern GpImage *SE_g_screenshotImage;
    SV2_BuildMaskedScreenshot(SE_g_screenshotImage);
}

BYTE *SV2_GlassAlphaMask[SV2_GLASS_COUNT] = {0};

void SV2_BuildGlassAlphaMask(int idx) {
    SV2Img *m = &SV2_Glasses[idx].normal;
    if (!m->valid) return;
    RECT b = m->bbox;
    int w = b.right - b.left, h = b.bottom - b.top;
    if (w <= 0 || h <= 0) return;

    SV2_GlassAlphaMask[idx] = (BYTE *)malloc((size_t)w * h);
    memset(SV2_GlassAlphaMask[idx], 0, (size_t)w * h);

    GpBitmap *bmp = (GpBitmap *)m->img;
    BitmapData bd;
    GpRect r = {b.left, b.top, w, h};
    if (GdipBitmapLockBits(bmp, &r, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
        for (int y = 0; y < h; y++) {
            UINT32 *row = (UINT32 *)((BYTE *)bd.Scan0 + (size_t)y * bd.Stride);
            for (int x = 0; x < w; x++) {
                SV2_GlassAlphaMask[idx][y * w + x] = (BYTE)((row[x] >> 24) & 0xFF);
            }
        }
        GdipBitmapUnlockBits(bmp, &bd);
    }
}

BOOL SV2_GlassContainsPoint(int idx, int nx, int ny) {
    SV2Img *m = &SV2_Glasses[idx].normal;
    if (!m->valid) return FALSE;
    RECT b = m->bbox;
    if (nx < b.left || nx >= b.right || ny < b.top || ny >= b.bottom) return FALSE;
    if (!SV2_GlassAlphaMask[idx]) return TRUE; /* fallback if mask build failed. */
    int w = b.right - b.left;
    return SV2_GlassAlphaMask[idx][(ny - b.top) * w + (nx - b.left)] > 20;
}

int SV2_HitTestGlass(int nx, int ny) {
    for (int i = SV2_GLASS_COUNT - 1; i >= 0; i--) {
        if (SV2_GlassContainsPoint(i, nx, ny)) return i;
    }
    return -1;
}

void SV2_OnMouseMove(HWND hwnd, int sx, int sy) {
    RECT rc; GetClientRect(hwnd, &rc);
    SV2Layout L = SV2_GetLayout(rc.right, rc.bottom);
    int nx, ny;
    if (!SV2_ScreenToNative(L, sx, sy, &nx, &ny)) return;

    int hit = SV2_HitTestGlass(nx, ny);
    if (hit != SV2_HoveredGlass) {
        int oldHovered = SV2_HoveredGlass;
        SV2_HoveredGlass = hit;

        extern void AppendStatus(const char *);
        char msg[200];
        const char *oldName = (oldHovered >= 0) ? SV2_Glasses[oldHovered].name : "(none)";
        const char *newName = (hit >= 0) ? SV2_Glasses[hit].name : "(none)";
        snprintf(msg, sizeof(msg), "[sv2hover] %s -> %s%s", oldName, newName,
            (hit >= 0 && !SV2_Glasses[hit].inverted.valid) ? " [NO INVERTED IMAGE LOADED]" : "");
        AppendStatus(msg);

        /* Skip the repaint trigger (but still update the hover state above) while a stat number is. */
        extern int SV2_ActiveEditBox;
        if (SV2_ActiveEditBox >= 0) return;

        RECT invalRect; BOOL haveRect = FALSE;
        if (oldHovered >= 0 && SV2_Glasses[oldHovered].normal.valid) {
            RECT b; SV2_NativeToScreen(L, SV2_Glasses[oldHovered].normal.bbox, &b);
            invalRect = b; haveRect = TRUE;
        }
        if (hit >= 0 && SV2_Glasses[hit].normal.valid) {
            RECT b; SV2_NativeToScreen(L, SV2_Glasses[hit].normal.bbox, &b);
            if (haveRect) UnionRect(&invalRect, &invalRect, &b); else { invalRect = b; haveRect = TRUE; }
        }
        if (haveRect) {
            snprintf(msg, sizeof(msg), "[sv2hover] invalidating rect (%ld,%ld)-(%ld,%ld) [%ldx%ld]",
                (long)invalRect.left, (long)invalRect.top, (long)invalRect.right, (long)invalRect.bottom,
                (long)(invalRect.right - invalRect.left), (long)(invalRect.bottom - invalRect.top));
            AppendStatus(msg);
        }
        if (haveRect) InvalidateRect(hwnd, &invalRect, FALSE);
        else InvalidateRect(hwnd, NULL, FALSE);
    }
}

void SV2_ActivateEditBox(HWND hwnd, int proficiencyIndex) {
    extern HWND SE_hEditBoxes[];
    if (SV2_ActiveEditBox == proficiencyIndex) return;
    if (SV2_ActiveEditBox >= 0 && SE_hEditBoxes[SV2_ActiveEditBox]) {
        ShowWindow(SE_hEditBoxes[SV2_ActiveEditBox], SW_HIDE);
    }
    SV2_ActiveEditBox = proficiencyIndex;
    SV2_RepositionStatBoxes(hwnd); /* shows the newly-active box at the right position/font. */
    if (SE_hEditBoxes[proficiencyIndex]) {
        SetFocus(SE_hEditBoxes[proficiencyIndex]);
        SendMessageA(SE_hEditBoxes[proficiencyIndex], EM_SETSEL, 0, -1);
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

void SV2_OnLButtonDown(HWND hwnd, int sx, int sy) {
    RECT rc; GetClientRect(hwnd, &rc);
    SV2Layout L = SV2_GetLayout(rc.right, rc.bottom);
    int nx, ny;
    if (!SV2_ScreenToNative(L, sx, sy, &nx, &ny)) return;

    if (SV2_BackToLauncher.valid && SV2_PtInBBox(nx, ny, &SV2_BackToLauncher.bbox)) {
        extern void SwitchToLauncher(HWND);
        SwitchToLauncher(hwnd);
        return;
    }
    if (SV2_SaveLocationBtn.valid && SV2_PtInBBox(nx, ny, &SV2_SaveLocationBtn.bbox)) {
        extern void SE_ChooseSavesFolder(void);
        SE_ChooseSavesFolder();
        return;
    }

    /* Level/Cred: no glass panel to hit-test against, just check proximity to their drawn-text. */
    extern SaveFile SE_g_save;
    double distLevel = sqrt(pow(nx - SV2_LEVEL_NUM_CX, 2) + pow(ny - SV2_LEVEL_NUM_CY, 2));
    double distCred = sqrt(pow(nx - SV2_CRED_NUM_CX, 2) + pow(ny - SV2_CRED_NUM_CY, 2));
    if (distLevel < 100 || distCred < 100) {
        const char *wantName = (distLevel < distCred) ? "Level" : "StreetCred";
        for (int p = 0; p < SE_g_save.proficiencyCount; p++) {
            if (strcmp(SE_g_save.proficiencies[p].name, wantName) == 0) {
                SV2_ActivateEditBox(hwnd, p);
                return;
            }
        }
    }

    int hit = SV2_HitTestGlass(nx, ny);
    if (hit < 0) return;

    if (strcmp(SV2_Glasses[hit].name, "__loadsave") == 0) {
        extern BOOL SE_BrowseForSaveFile(void);
        extern void SE_LoadSaveFile(void);
        if (SE_BrowseForSaveFile()) {
            SE_LoadSaveFile();
            SV2_RepositionStatBoxes(hwnd); /* SE_LoadSaveFile() rebuilds the edit boxes at the OLD system's positions internally. */
            SV2_PopulateFromSave();
            InvalidateRect(hwnd, NULL, TRUE);
        }
    } else if (strcmp(SV2_Glasses[hit].name, "__savechanges") == 0) {
        extern void SE_SaveChangesToFile(void);
        SE_SaveChangesToFile();
    } else if (strcmp(SV2_Glasses[hit].name, "__console") == 0) {
        extern BOOL gMB_ConsoleVisible;
        extern void MENU_UpdateConsoleVisibility(HWND);
        gMB_ConsoleVisible = !gMB_ConsoleVisible;
        MENU_UpdateConsoleVisibility(hwnd);
    } else if (SV2_Glasses[hit].name[0] != '_') {
        /* A real stat glass. */
        for (int p = 0; p < SE_g_save.proficiencyCount; p++) {
            if (strcmp(SE_g_save.proficiencies[p].name, SV2_Glasses[hit].name) == 0) {
                SV2_ActivateEditBox(hwnd, p);
                break;
            }
        }
    }
    /* __extra: reserved, no action defined yet. */
}

BOOL SV2_Tick(HWND hwnd) {
    DWORD now = GetTickCount();
    if (SV2_LedLastTick == 0) SV2_LedLastTick = now;
    double dt = (now - SV2_LedLastTick) / 1000.0;
    SV2_LedLastTick = now;

    double span = SV2_LED_BOTTOM - SV2_LED_TOP;
    SV2_LedScrollY += SV2_LED_SPEED_PX_PER_SEC * dt;
    if (SV2_LedScrollY > span) SV2_LedScrollY -= span;
    return TRUE;
}


/* Safe getters for the D2D test path (d2d_phase3_text.c) to read the Level/Cred display. */
const char *SV2_D2D_GetLevelNumText(void) {
    static char buf[16];
    for (int p = 0; p < SE_g_save.proficiencyCount; p++) {
        if (strcmp(SE_g_save.proficiencies[p].name, "Level") != 0) continue;
        if (p == SV2_ActiveEditBox || !SE_hEditBoxes[p]) return NULL;
        GetWindowTextA(SE_hEditBoxes[p], buf, sizeof(buf));
        return buf[0] ? buf : NULL;
    }
    return NULL;
}
const char *SV2_D2D_GetCredNumText(void) {
    static char buf[16];
    for (int p = 0; p < SE_g_save.proficiencyCount; p++) {
        if (strcmp(SE_g_save.proficiencies[p].name, "StreetCred") != 0) continue;
        if (p == SV2_ActiveEditBox || !SE_hEditBoxes[p]) return NULL;
        GetWindowTextA(SE_hEditBoxes[p], buf, sizeof(buf));
        return buf[0] ? buf : NULL;
    }
    return NULL;
}

/* Same pattern as the two getters above, but for the per-glass stat numbers. */
const char *SV2_D2D_GetGlassNumText(int glassIndex, double *outCx, double *outCy, BOOL *outHovered) {
    static char buf[16];
    if (glassIndex < 0 || glassIndex >= SV2_GLASS_COUNT) return NULL;
    *outCx = SV2_Glasses[glassIndex].numCx;
    *outCy = SV2_Glasses[glassIndex].numCy;
    *outHovered = (glassIndex == SV2_HoveredGlass);

    for (int p = 0; p < SE_g_save.proficiencyCount; p++) {
        if (strcmp(SV2_Glasses[glassIndex].name, SE_g_save.proficiencies[p].name) != 0) continue;
        if (p == SV2_ActiveEditBox || !SE_hEditBoxes[p]) return NULL;
        GetWindowTextA(SE_hEditBoxes[p], buf, sizeof(buf));
        return buf[0] ? buf : NULL;
    }
    return NULL;
}


LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            char defaultPath[MAX_PATH];
            snprintf(defaultPath, sizeof(defaultPath), "%s\\archive\\pc\\mod", GAME_ROOT);

            /* Hidden - purely storage for the configured mods folder path, set via the "Mod Folder". */
            hPathBox = CreateWindowA("EDIT", defaultPath,
                WS_CHILD | ES_AUTOHSCROLL,
                0, 0, 10, 10, hwnd, (HMENU)ID_PATH_BOX, NULL, NULL);

            /* "Console" - the launcher's log, now toggled by Button 6 instead of always being visible. */
            hStatusText = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                20, 170, 700, 500, hwnd, (HMENU)ID_STATUS_TEXT, NULL, NULL);
            SendMessageA(hStatusText, WM_SETFONT, (WPARAM)hFont, TRUE);

            MENU_InitOnce();
            SetTimer(hwnd, MENU_TIMER_ANIM, 15, NULL);

            AppendStatus("Ready. Check mod conflicts anytime, or launch the game.");
            break;
        }

        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (gViewMode == MODE_LAUNCHER) {
                if (hStatusText != NULL && gMB_ConsoleVisible) {
                    MoveWindow(hStatusText, 20, 170, rc.right - 40, rc.bottom - 190, TRUE);
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
            if (gSaveEditorActivated) {
                SE_WndProc(hwnd, WM_SIZE, wParam, lParam); /* rebuilds SE_hEditBoxes[] data binding. */
                if (gViewMode == MODE_SAVEEDITOR) {
                    SV2_RepositionStatBoxes(hwnd); /* re-skins those same boxes to SV2's layout. */
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            break;
        }

        case WM_WINDOWPOSCHANGED:
            /* The caption overlay is a real top-level (WS_POPUP) window using absolute screen. */
            if (gSaveEditorActivated && gViewMode == MODE_SAVEEDITOR) {
                SV2_RepositionStatBoxes(hwnd);
            }
            return DefWindowProcA(hwnd, msg, wParam, lParam);

        case WM_PAINT: {
            if (gViewMode == MODE_SAVEEDITOR && gSaveEditorActivated) {
                extern void SV2_D2D_TestPaint(HWND);
                PAINTSTRUCT psD2D;
                BeginPaint(hwnd, &psD2D);
                SV2_D2D_TestPaint(hwnd);
                EndPaint(hwnd, &psD2D);
                return 0;
            }
            if (gViewMode == MODE_LAUNCHER) {
                extern void MENU_D2D_Paint(HWND);
                PAINTSTRUCT psD2D;
                BeginPaint(hwnd, &psD2D);
                MENU_D2D_Paint(hwnd);
                EndPaint(hwnd, &psD2D);
                return 0;
            }
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            MENU_Paint(hwnd, hdc, rc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            /* MENU_Paint fills the entire client area itself (offscreen, then blitted), and the save. */
            return 1;

        case WM_MOUSEMOVE:
            if (gViewMode == MODE_LAUNCHER) {
                MENU_OnMouseMove(hwnd, LOWORD(lParam), HIWORD(lParam));
            } else if (gViewMode == MODE_SAVEEDITOR) {
                SV2_OnMouseMove(hwnd, LOWORD(lParam), HIWORD(lParam));
            }
            break;

        case WM_LBUTTONDOWN:
            if (gViewMode == MODE_LAUNCHER) {
                MENU_OnLButtonDown(hwnd, LOWORD(lParam), HIWORD(lParam));
            } else if (gViewMode == MODE_SAVEEDITOR) {
                SV2_OnLButtonDown(hwnd, LOWORD(lParam), HIWORD(lParam));
            }
            break;

        case WM_CTLCOLOREDIT: {
            if (gSaveEditorActivated) {
                if (gViewMode == MODE_SAVEEDITOR) {
                    /* Transparent so the glass art shows through. */
                    HWND hCtl = (HWND)lParam;
                    int ctlId = GetDlgCtrlID(hCtl);
                    if (ctlId >= SE_ID_FIRST_EDIT) {
                        int idx = ctlId - SE_ID_FIRST_EDIT;
                        BOOL isLevelOrCred = (idx >= 0 && idx < SE_g_save.proficiencyCount &&
                            (strcmp(SE_g_save.proficiencies[idx].name, "Level") == 0 ||
                             strcmp(SE_g_save.proficiencies[idx].name, "StreetCred") == 0));
                        BOOL isHovered = (idx >= 0 && idx < MAX_PROFICIENCIES &&
                            SV2_EditBoxGlassIndex[idx] == SV2_HoveredGlass && SV2_HoveredGlass >= 0);
                        SetBkMode((HDC)wParam, TRANSPARENT);
                        SetTextColor((HDC)wParam, (isHovered || isLevelOrCred) ? RGB(255, 255, 255) : RGB(20, 20, 20));
                        return (LRESULT)GetStockObject(NULL_BRUSH);
                    }
                }
                LRESULT r = SE_WndProc(hwnd, msg, wParam, lParam);
                if (r != 0) return r;
            }
            return DefWindowProcA(hwnd, msg, wParam, lParam);
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_BUTTON_BACKTOLAUNCHER) {
                SwitchToLauncher(hwnd);
            } else if (LOWORD(wParam) == ID_CONSOLE_CLOSE) {
                gMB_ConsoleVisible = FALSE;
                MENU_UpdateConsoleVisibility(hwnd);
            } else if (gSaveEditorActivated && HIWORD(wParam) == EN_CHANGE) {
                /* EN_CHANGE repaint-on-edit (avoids ghosting on the transparent boxes) still needs. */
                SE_WndProc(hwnd, msg, wParam, lParam);
            }
            break;

        case WM_TIMER:
            if (wParam == TIMER_PRIORITY_WATCH) {
                CheckAndApplyDynamicPriority();
            } else if (wParam == SE_TIMER_STARTUP_RELAYOUT) {
                SE_WndProc(hwnd, msg, wParam, lParam);
                if (gViewMode == MODE_SAVEEDITOR) {
                    SV2_RepositionStatBoxes(hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            } else if (wParam == MENU_TIMER_ANIM) {
                if (gViewMode == MODE_LAUNCHER && MENU_Tick(hwnd)) {
                    InvalidateRect(hwnd, NULL, FALSE);
                } else if (gViewMode == MODE_SAVEEDITOR) {
                    BOOL animated = SV2_Tick(hwnd);
                    /* SV2_Tick still runs either way (keeps LED's scroll position correct for when editing ends) */
                    extern int SV2_ActiveEditBox;
                    if (animated && SV2_ActiveEditBox < 0) {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
            }
            break;

        case WM_DESTROY:
            if (gSaveEditorActivated) SE_Shutdown();
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    /* Must be declared before any window is created. */
    SetProcessDPIAware();

    /* Fresh log file each run (truncated, not appended-forever) so old sessions don't pile up. */
    {
        char logPath[MAX_PATH];
        GetModuleFileNameA(NULL, logPath, MAX_PATH);
        char *lastSlash = strrchr(logPath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';
        strncat(logPath, "debug_log.txt", MAX_PATH - strlen(logPath) - 1);
        FILE *f = fopen(logPath, "w");
        if (f) {
            fprintf(f, "===== New session started =====\n");
            fclose(f);
        }
    }

    hInstanceGlobal = hInstance;

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    /* One-time save editor setup: GDI+, its font, and its embedded artwork resources. */
    SE_InitOnce(hInstance);
    SV2_InitOnce();

    const char CLASS_NAME[] = "CyberpunkLauncherWindowClassV4";

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(hInstance, "IDI_APPICON");

    RegisterClassA(&wc);

    hMainWindow = CreateWindowExA(
        0, CLASS_NAME, "Cyberpunk 2077 Custom Launcher",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 720,
        NULL, NULL, hInstance, NULL
    );

    if (hMainWindow == NULL) {
        return 0;
    }

    ShowWindow(hMainWindow, SW_SHOWMAXIMIZED);
    UpdateWindow(hMainWindow);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return 0;
}
