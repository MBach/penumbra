// Penumbra: a lean screen dimmer for Windows.
// Copyright (c) 2026 Matthieu BACHELIER. MIT License, see LICENSE.
//
// Dimming scales the graphics card's colour table (the gamma ramp) of every
// monitor: nothing is drawn over the desktop, the mouse cursor dims too, and
// screenshots come out undimmed since the table applies after capture.
// Windows refuses tables below 50 % unless GdiIcmGammaRange is set (the MSI
// sets it; it takes effect after a restart); until then the screen goes as
// low as Windows allows. HDR displays ignore the table: not supported.
//
// Panel: ten big buttons, 10 % to 100 %, then a 10-100 % slider with its
// live value (using it unselects the buttons). While the panel is open, its
// taskbar thumbnail has two quick buttons, last preset and 100 %. Minimizing
// hides the panel in the tray. Tray: left click toggles between the last
// preset and 100 %; right click opens the menu (open, last preset, 100 %,
// remember last setting, start with Windows, quit). The close button quits.

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <imm.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "advapi32")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "ole32")
#pragma comment(lib, "imm32")

namespace {

constexpr wchar_t kAppName[] = L"Penumbra";
constexpr wchar_t kMainClass[] = L"Penumbra.Main";
constexpr wchar_t kSettingsKey[] = L"Software\\Penumbra";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kThemeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
// Each monitor's original gamma ramp while ours is applied, so a crash
// cannot leave the screen dimmed: the next launch puts them back.
constexpr wchar_t kGammaKey[] = L"Software\\Penumbra\\Gamma";

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT kTrayId = 1;
constexpr UINT kThumbPreset = 100;
constexpr UINT kThumbFull = 101;
constexpr UINT kMenuOpen = 197;
constexpr UINT kMenuPreset = 198;
constexpr UINT kMenuFull = 199;
constexpr UINT kMenuRemember = 200;
constexpr UINT kMenuStartup = 201;
constexpr UINT kMenuQuit = 202;
constexpr UINT kMenuDimAtLogin = 203;
constexpr UINT_PTR kTimerGamma = 1;
constexpr UINT_PTR kTimerTrim = 2;  // once, 1 s after startup
constexpr int kDefaultPreset = 70;

// Panel layout, in DIPs.
constexpr int kCols = 5;
constexpr int kRows = 2;
constexpr int kCell = 64;
constexpr int kGap = 8;
constexpr int kMargin = 16;
constexpr int kSection = 12;    // space above and below the separator
constexpr int kSliderRow = 32;  // height of the slider row
constexpr int kThumb = 20;      // slider thumb diameter
constexpr int kValue = 56;      // width of the value right of the slider
constexpr int kRadius = 4;      // button corners (Fluent 2 control radius)
constexpr int kHoverSlider = 100;  // gHover value for the slider row

struct Settings {
    // Current brightness, 10..100. A first launch dims to the default preset,
    // so the effect shows right after install.
    int level = kDefaultPreset;
    int preset = kDefaultPreset;   // last level picked below 100
    bool remember = true;
    bool startup = true;
    // Started by Windows at login: apply the saved level, or start at 100 %
    // (the default: the morning light may need the full screen; the preset
    // stays one tray click away).
    bool dimAtLogin = false;
} g;

HINSTANCE gInst;
HWND gMain;
UINT gMsgTaskbarCreated;
UINT gMsgButtonCreated;
ITaskbarList3* gTaskbar;
bool gThumbsAdded;
bool gTrayAdded;
HICON gTrayIcon;
HICON gThumbIcons[2];
HFONT gFont;
HFONT gValueFont;
UINT gDpi = 96;
bool gFromSlider;  // level set by the slider: no button is selected
bool gDragging;
int gHover = -1;    // button under the mouse, kHoverSlider, or -1
int gPressed = -1;  // button held down, or -1
bool gTracking;     // WM_MOUSELEAVE requested
int gWheelDelta;   // wheel travel not yet turned into a 5 % step

int Clamp(int level) { return level < 10 ? 10 : level > 100 ? 100 : level; }

int Px(int dip) { return MulDiv(dip, static_cast<int>(gDpi), 96); }

// ---- text ------------------------------------------------------------------
//
// Picked once from the Windows display language, English when none matches.
// No language setting: lean. This file is UTF-8 (cl /utf-8).

struct Text {
    const wchar_t* open;
    const wchar_t* remember;
    const wchar_t* startup;
    const wchar_t* dimAtLogin;
    const wchar_t* quit;
    const wchar_t* tip;  // tray tooltip: current level, level a click gives
};

const Text kEnglish{
    L"Open Penumbra",
    L"Remember last setting",
    L"Start with Windows",
    L"Dim at Windows startup",
    L"Quit",
    L"Penumbra %d%%  (click: %d%%)",
};

const Text kFrench{
    L"Ouvrir Penumbra",
    L"Mémoriser le dernier réglage",
    L"Lancer au démarrage de Windows",
    L"Assombrir au démarrage de Windows",
    L"Quitter",
    L"Penumbra %d %%  (clic : %d %%)",
};

const Text kGerman{
    L"Penumbra öffnen",
    L"Letzte Einstellung merken",
    L"Mit Windows starten",
    L"Beim Windows-Start abdunkeln",
    L"Beenden",
    L"Penumbra %d %%  (Klick: %d %%)",
};

const Text kItalian{
    L"Apri Penumbra",
    L"Ricorda l'ultima impostazione",
    L"Avvia con Windows",
    L"Scurisci all'avvio di Windows",
    L"Esci",
    L"Penumbra %d%%  (clic: %d%%)",
};

const Text kSpanish{
    L"Abrir Penumbra",
    L"Recordar el último ajuste",
    L"Iniciar con Windows",
    L"Atenuar al iniciar Windows",
    L"Salir",
    L"Penumbra %d %%  (clic: %d %%)",
};

const Text kPortuguese{
    L"Abrir o Penumbra",
    L"Lembrar a última configuração",
    L"Iniciar com o Windows",
    L"Escurecer ao iniciar o Windows",
    L"Sair",
    L"Penumbra %d%%  (clique: %d%%)",
};

const Text kJapanese{
    L"Penumbra を開く",
    L"最後の設定を記憶する",
    L"Windows の起動時に開始",
    L"Windows の起動時に暗くする",
    L"終了",
    L"Penumbra %d%%（クリック: %d%%）",
};

const Text& PickText() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_FRENCH: return kFrench;
    case LANG_GERMAN: return kGerman;
    case LANG_ITALIAN: return kItalian;
    case LANG_SPANISH: return kSpanish;
    case LANG_PORTUGUESE: return kPortuguese;
    case LANG_JAPANESE: return kJapanese;
    default: return kEnglish;
    }
}

const Text* gText = &kEnglish;

// ---- settings --------------------------------------------------------------

DWORD ReadDword(const wchar_t* key, const wchar_t* name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof value;
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD,
                                  nullptr, &value, &size);
    return status == ERROR_SUCCESS ? value : fallback;
}

void WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                   sizeof value);
}

void LoadSettings() {
    g.remember = ReadDword(kSettingsKey, L"Remember", 1) != 0;
    g.startup = ReadDword(kSettingsKey, L"Startup", 1) != 0;
    g.dimAtLogin = ReadDword(kSettingsKey, L"DimAtLogin", 0) != 0;
    if (!g.remember) return;
    g.level = Clamp(static_cast<int>(ReadDword(kSettingsKey, L"Level", kDefaultPreset)));
    g.preset = Clamp(static_cast<int>(ReadDword(kSettingsKey, L"Preset", kDefaultPreset)));
    if (g.preset == 100) g.preset = kDefaultPreset;
    gFromSlider = ReadDword(kSettingsKey, L"Slider", 0) != 0;
}

// One key open per save: SaveSettings runs on every level change.
void SaveSettings() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    WriteDword(key, L"Remember", g.remember);
    WriteDword(key, L"Startup", g.startup);
    WriteDword(key, L"DimAtLogin", g.dimAtLogin);
    if (g.remember) {
        WriteDword(key, L"Level", static_cast<DWORD>(g.level));
        WriteDword(key, L"Preset", static_cast<DWORD>(g.preset));
        WriteDword(key, L"Slider", gFromSlider);
    }
    RegCloseKey(key);
}

// Path of this exe; empty if it cannot be read in full (no truncated paths).
std::wstring ExePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD size = static_cast<DWORD>(buffer.size());
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), size);
        if (length == 0) return {};
        if (length < size) return std::wstring(buffer.data(), length);
        if (size >= 32768) return {};
        buffer.resize(size * 2);
    }
}

// Start with Windows: a value under HKCU\...\Run, for the current user, with
// /startup so the app knows it was launched at login.
void ApplyStartup() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (!g.startup) {
        RegDeleteValueW(key, kAppName);
    } else if (const std::wstring exe = ExePath(); !exe.empty()) {
        const std::wstring command = L"\"" + exe + L"\" /startup";
        RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
}

// ---- icons -----------------------------------------------------------------

// Two theme settings: "system" drives the taskbar (tray and thumbnail icons),
// "apps" drives app windows (the panel and its title bar).
bool SystemLight() { return ReadDword(kThemeKey, L"SystemUsesLightTheme", 0) != 0; }
bool AppsLight() { return ReadDword(kThemeKey, L"AppsUseLightTheme", 1) != 0; }

// White on a dark taskbar, near-black on a light one.
COLORREF Ink() { return SystemLight() ? RGB(32, 32, 32) : RGB(255, 255, 255); }

// Colours after Fluent 2 / Windows 11 (solid base background, strong control
// fill for the rail, solid control fill for the thumb). The buttons keep a
// grey ramp that previews each level.
struct Palette {
    COLORREF background;
    int shadeBase;      // button grey = shadeBase + level * shadeStep / 10
    int shadeStep;
    COLORREF edge;      // mixed into a button's grey for its 1 px border
    double edgeMix;
    COLORREF hover;     // mixed into a hovered button's grey
    double hoverMix;
    COLORREF separator;
    COLORREF rail;
    COLORREF thumb;
    COLORREF thumbBorder;
    COLORREF text;
};

const Palette kDark{RGB(32, 32, 32),    45, 20, RGB(255, 255, 255), 0.10,
                    RGB(255, 255, 255), 0.08,   RGB(52, 52, 52),     RGB(154, 154, 154),
                    RGB(69, 69, 69),    RGB(88, 88, 88), RGB(255, 255, 255)};
const Palette kLight{RGB(243, 243, 243), 40, 21, RGB(0, 0, 0), 0.12,
                     RGB(0, 0, 0),       0.05,   RGB(224, 224, 224), RGB(134, 134, 134),
                     RGB(255, 255, 255), RGB(209, 209, 209), RGB(27, 27, 27)};
const Palette* gPalette = &kDark;
COLORREF gAccent;  // Windows accent colour, in the shade the theme calls for

// The Windows accent palette holds 8 RGBA entries: light 3..1, accent, dark
// 1..3. Like Windows 11, dark themes use "light 2" and light themes "dark 1".
COLORREF ReadAccent(bool dark) {
    BYTE palette[32];
    DWORD size = sizeof palette;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, palette, &size) == ERROR_SUCCESS &&
        size >= sizeof palette) {
        const BYTE* c = palette + (dark ? 1 : 4) * 4;
        return RGB(c[0], c[1], c[2]);
    }
    return dark ? RGB(0x60, 0xCD, 0xFF) : RGB(0x00, 0x5F, 0xB8);  // default blue
}

int SmallIconSize() { return GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()); }

// A ring filled from the bottom up to `level` percent, like a gauge.
HICON MakeLevelIcon(int size, int level, COLORREF ink) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof header;
    header.bV5Width = size;
    header.bV5Height = -size;  // top-down rows
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);

    const double center = size / 2.0;
    const double outer = size / 2.0 - 0.5;
    const double ring = size >= 24 ? size / 12.0 : 1.25;
    const double inner = outer - ring - (size >= 24 ? size / 16.0 : 0.75);
    const double fillTop = center + inner - 2.0 * inner * level / 100.0;
    const DWORD rgb = (static_cast<DWORD>(GetRValue(ink)) << 16) |
                      (static_cast<DWORD>(GetGValue(ink)) << 8) | GetBValue(ink);

    auto* pixels = static_cast<DWORD*>(bits);
    constexpr int kSamples = 4;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int hits = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    double px = x + (sx + 0.5) / kSamples - center;
                    double py = y + (sy + 0.5) / kSamples;
                    double d = std::hypot(px, py - center);
                    bool inRing = d <= outer && d >= outer - ring;
                    bool inFill = d <= inner && py >= fillTop;
                    hits += inRing || inFill;
                }
            }
            DWORD alpha = static_cast<DWORD>(255 * hits / (kSamples * kSamples));
            pixels[y * size + x] = (alpha << 24) | (alpha ? rgb : 0);
        }
    }

    ICONINFO info{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// ---- gamma ramp ------------------------------------------------------------

struct Ramp {
    WORD v[3][256];
};

struct Screen {
    std::wstring device;  // \\.\DISPLAYn
    HDC dc = nullptr;
    bool valid = false;   // original read: the ramp can be driven
    Ramp original{};
    Ramp applied{};
};

std::vector<Screen> gScreens;
bool gGammaActive;  // the screens hold our dimmed tables
int gFloor;         // lowest level worth trying: Windows refused the ones below

// A screen whose ramp we changed before crashing gets its original back.
void RecoverGamma() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kGammaKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return;
    wchar_t device[64];
    Ramp ramp;
    for (DWORD i = 0;; ++i) {
        DWORD nameLength = ARRAYSIZE(device), size = sizeof ramp, type = 0;
        if (RegEnumValueW(key, i, device, &nameLength, nullptr, &type,
                          reinterpret_cast<BYTE*>(&ramp), &size) != ERROR_SUCCESS)
            break;
        if (type != REG_BINARY || size != sizeof ramp) continue;
        if (HDC dc = CreateDCW(device, nullptr, nullptr, nullptr)) {
            SetDeviceGammaRamp(dc, &ramp);
            DeleteDC(dc);
        }
    }
    RegCloseKey(key);
    RegDeleteKeyW(HKEY_CURRENT_USER, kGammaKey);
}

// Puts every screen's original ramp back.
void RestoreGamma() {
    if (!gGammaActive) return;
    for (Screen& s : gScreens)
        if (s.valid) SetDeviceGammaRamp(s.dc, &s.original);
    gGammaActive = false;
    RegDeleteKeyW(HKEY_CURRENT_USER, kGammaKey);
}

// Reads the monitors and their current (original) ramps. Called at start and
// when the displays change, with our tables taken off first.
void LoadScreens() {
    RestoreGamma();
    for (Screen& s : gScreens)
        if (s.dc) DeleteDC(s.dc);
    gScreens.clear();
    DISPLAY_DEVICEW dd{sizeof dd};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd = {sizeof dd}) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        Screen s;
        s.device = dd.DeviceName;
        s.dc = CreateDCW(dd.DeviceName, nullptr, nullptr, nullptr);
        s.valid = s.dc && GetDeviceGammaRamp(s.dc, &s.original);
        gScreens.push_back(s);
    }
    gFloor = 0;
}

// Scales every screen's original ramp to `level` %; false if Windows refuses.
bool ApplyRamps(int level) {
    for (Screen& s : gScreens) {
        if (!s.valid) continue;
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i)
                s.applied.v[c][i] = static_cast<WORD>(s.original.v[c][i] * level / 100);
        if (!SetDeviceGammaRamp(s.dc, &s.applied)) return false;
    }
    return true;
}

// Dims every screen to `level` % through its ramp, scaled from the original
// (a calibrated monitor keeps its calibration). Windows refuses tables below
// 50 % until GdiIcmGammaRange is set and Windows restarted: the screens then
// go as low as Windows allows.
void DimByGamma(int level) {
    if (!gGammaActive) {  // keep the originals until they are back
        HKEY key;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kGammaKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                            &key, nullptr) == ERROR_SUCCESS) {
            for (const Screen& s : gScreens)
                if (s.valid)
                    RegSetValueExW(key, s.device.c_str(), 0, REG_BINARY,
                                   reinterpret_cast<const BYTE*>(&s.original), sizeof s.original);
            RegCloseKey(key);
        }
        gGammaActive = true;
    }
    for (int l = std::max(level, gFloor); l < 100; ++l) {
        if (ApplyRamps(l)) return;
        gFloor = l + 1;
    }
    RestoreGamma();  // nothing accepted
}

// On the 2 s timer while dimmed: puts our tables back if something reset
// them (sleep, driver, another colour tool).
void CheckGamma() {
    if (!gGammaActive) return;
    for (Screen& s : gScreens) {
        Ramp current;
        if (s.valid && GetDeviceGammaRamp(s.dc, &current) &&
            std::memcmp(&current, &s.applied, sizeof current))
            SetDeviceGammaRamp(s.dc, &s.applied);
    }
}

// ---- tray and taskbar ------------------------------------------------------

void UpdateTray() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = gMain;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    HICON old = gTrayIcon;
    gTrayIcon = MakeLevelIcon(SmallIconSize(), g.level, Ink());
    nid.hIcon = gTrayIcon;
    swprintf_s(nid.szTip, gText->tip, g.level, g.level < 100 ? 100 : g.preset);
    if (!gTrayAdded) {
        gTrayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    if (old) DestroyIcon(old);
}

void RemoveTray() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = gMain;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    gTrayAdded = false;
}

// Two buttons under the taskbar thumbnail: last preset and 100 %. They exist
// only while the panel has a taskbar icon, so they are added again on each
// TaskbarButtonCreated (every time the panel comes back from the tray).
void UpdateThumbs() {
    if (!gTaskbar) return;
    HICON old[2] = {gThumbIcons[0], gThumbIcons[1]};
    const int size = SmallIconSize();
    gThumbIcons[0] = MakeLevelIcon(size, g.preset, Ink());
    gThumbIcons[1] = MakeLevelIcon(size, 100, Ink());

    THUMBBUTTON buttons[2]{};
    const UINT ids[2] = {kThumbPreset, kThumbFull};
    const int levels[2] = {g.preset, 100};
    for (int i = 0; i < 2; ++i) {
        buttons[i].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        buttons[i].iId = ids[i];
        buttons[i].hIcon = gThumbIcons[i];
        buttons[i].dwFlags = THBF_ENABLED;
        swprintf_s(buttons[i].szTip, L"%d%%", levels[i]);
    }
    if (gThumbsAdded) {
        gTaskbar->ThumbBarUpdateButtons(gMain, 2, buttons);
    } else {
        gThumbsAdded = SUCCEEDED(gTaskbar->ThumbBarAddButtons(gMain, 2, buttons));
    }
    for (HICON icon : old)
        if (icon) DestroyIcon(icon);
}

// ---- level -----------------------------------------------------------------

// `fromSlider` leaves the buttons unselected; `live` (while dragging) skips
// saving and the tray/taskbar icons, done once on release.
void SetLevel(int level, bool fromSlider = false, bool live = false) {
    g.level = Clamp(level);
    gFromSlider = fromSlider;
    if (g.level < 100) g.preset = g.level;

    // The 2 s timer only runs while dimmed: at 100 % nothing wakes the app.
    if (g.level >= 100) {
        RestoreGamma();
        KillTimer(gMain, kTimerGamma);
    } else {
        DimByGamma(g.level);
        SetTimer(gMain, kTimerGamma, 2000, nullptr);
    }

    if (!live) {
        SaveSettings();
        UpdateTray();
        UpdateThumbs();
    }
    InvalidateRect(gMain, nullptr, FALSE);
}

void Toggle() { SetLevel(g.level < 100 ? 100 : g.preset); }

// ---- panel -----------------------------------------------------------------

HFONT SegoeUI(int pixels) {
    return CreateFontW(-Px(pixels), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Segoe UI");
}

void MakeFont() {
    if (gFont) DeleteObject(gFont);
    if (gValueFont) DeleteObject(gValueFont);
    gFont = SegoeUI(20);       // Fluent 2 subtitle 1
    gValueFont = SegoeUI(16);  // Fluent 2 subtitle 2
}

int GridBottom() { return Px(kMargin) + kRows * Px(kCell) + (kRows - 1) * Px(kGap); }
int SeparatorY() { return GridBottom() + Px(kSection); }

SIZE ClientSize() {
    return {2 * Px(kMargin) + kCols * Px(kCell) + (kCols - 1) * Px(kGap),
            SeparatorY() + 1 + Px(kSection) + Px(kSliderRow) + Px(kMargin)};
}

RECT SliderRow() {
    const int top = SeparatorY() + 1 + Px(kSection);
    return {Px(kMargin), top, ClientSize().cx - Px(kMargin), top + Px(kSliderRow)};
}

// The thumb's centre runs from TrackLeft (10 %) to TrackRight (100 %).
int TrackLeft() { return SliderRow().left + Px(kThumb) / 2; }
int TrackRight() { return SliderRow().right - Px(kValue) - Px(kThumb) / 2; }

int ThumbX(int level) {
    return TrackLeft() + MulDiv(level - 10, TrackRight() - TrackLeft(), 90);
}

int LevelAtX(int x) {
    const int span = TrackRight() - TrackLeft();
    return Clamp(10 + static_cast<int>(std::lround((x - TrackLeft()) * 90.0 / span)));
}

bool OnSlider(POINT pt) {
    RECT row = SliderRow();
    row.right = TrackRight() + Px(kThumb) / 2;
    return PtInRect(&row, pt) != FALSE;
}

RECT CellRect(int index) {
    const int col = index % kCols;
    const int row = index / kCols;
    const int x = Px(kMargin) + col * (Px(kCell) + Px(kGap));
    const int y = Px(kMargin) + row * (Px(kCell) + Px(kGap));
    return {x, y, x + Px(kCell), y + Px(kCell)};
}

int CellAt(POINT pt) {
    for (int i = 0; i < kCols * kRows; ++i) {
        RECT r = CellRect(i);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

// ---- drawing ---------------------------------------------------------------
//
// Shapes are drawn by hand into a 32-bit DIB, anti-aliased from their signed
// distance (no GDI+ or Direct2D to load); GDI then writes the text on top.

struct Canvas {
    DWORD* pixels;  // 0x00RRGGBB, top-down
    int width;
    int height;
};

COLORREF Mix(COLORREF a, COLORREF b, double t) {
    auto channel = [t](int x, int y) { return static_cast<BYTE>(std::lround(x + (y - x) * t)); };
    return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)),
               channel(GetBValue(a), GetBValue(b)));
}

// A rounded rectangle (a circle or a capsule when r is half its height), in
// pixels with fractional edges. Each edge pixel gets the fraction it covers.
void FillRound(const Canvas& c, double x0, double y0, double x1, double y1, double r,
               COLORREF color) {
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    const double hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
    const int left = std::max(0, static_cast<int>(std::floor(x0)));
    const int right = std::min(c.width, static_cast<int>(std::ceil(x1)));
    const int top = std::max(0, static_cast<int>(std::floor(y0)));
    const int bottom = std::min(c.height, static_cast<int>(std::ceil(y1)));
    const int sr = GetRValue(color), sg = GetGValue(color), sb = GetBValue(color);
    for (int y = top; y < bottom; ++y) {
        const double qy = std::fabs(y + 0.5 - cy) - hy;
        for (int x = left; x < right; ++x) {
            const double qx = std::fabs(x + 0.5 - cx) - hx;
            const double d = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) +
                             std::min(std::max(qx, qy), 0.0) - r;
            double cover = 0.5 - d;
            if (cover <= 0) continue;
            if (cover > 1) cover = 1;
            DWORD& p = c.pixels[y * c.width + x];
            const int dr = (p >> 16) & 255, dg = (p >> 8) & 255, db = p & 255;
            p = (static_cast<DWORD>(std::lround(dr + (sr - dr) * cover)) << 16) |
                (static_cast<DWORD>(std::lround(dg + (sg - dg) * cover)) << 8) |
                static_cast<DWORD>(std::lround(db + (sb - db) * cover));
        }
    }
}

// A shape with a border: the outer shape in `border`, then the inner one,
// inset by `width`, in `fill`.
void FillRoundBordered(const Canvas& c, RECT r, double radius, double width, COLORREF border,
                       COLORREF fill) {
    FillRound(c, r.left, r.top, r.right, r.bottom, radius, border);
    FillRound(c, r.left + width, r.top + width, r.right - width, r.bottom - width,
              std::max(0.0, radius - width), fill);
}

double Dip(double dip) { return dip * gDpi / 96.0; }

void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;  // top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bitmap = client.right > 0 && client.bottom > 0
                         ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0)
                         : nullptr;
    if (!bitmap) {
        EndPaint(hwnd, &ps);
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
    const Canvas canvas{static_cast<DWORD*>(bits), client.right, client.bottom};
    const Palette& pal = *gPalette;
    const COLORREF bg = pal.background;
    std::fill_n(canvas.pixels, canvas.width * canvas.height,
                (static_cast<DWORD>(GetRValue(bg)) << 16) | (GetGValue(bg) << 8) | GetBValue(bg));

    // Buttons: a grey per level, 1 px border, the current level ringed with
    // the accent; lighter (dark theme) or darker (light) under the mouse,
    // sunk towards the background while held.
    COLORREF shades[kCols * kRows];
    for (int i = 0; i < kCols * kRows; ++i) {
        const int level = (i + 1) * 10;
        const int shade = std::min(255, pal.shadeBase + level * pal.shadeStep / 10);
        COLORREF fill = RGB(shade, shade, shade);
        if (gPressed == i) fill = Mix(fill, bg, 0.3);
        else if (gHover == i) fill = Mix(fill, pal.hover, pal.hoverMix);
        shades[i] = fill;
        const RECT r = CellRect(i);
        if (level == g.level && !gFromSlider) {
            // Like the Windows 11 focus ring: 2 px accent, then a 1 px gap in
            // the background colour so the ring stands out on any grey.
            FillRoundBordered(canvas, r, Dip(kRadius), Dip(2), gAccent, bg);
            const RECT inner{r.left + Px(2), r.top + Px(2), r.right - Px(2), r.bottom - Px(2)};
            FillRoundBordered(canvas, inner, std::max(0.0, Dip(kRadius - 2)), Dip(1), bg, fill);
        } else {
            FillRoundBordered(canvas, r, Dip(kRadius), Dip(1), Mix(fill, pal.edge, pal.edgeMix),
                              fill);
        }
    }

    // Separator.
    FillRound(canvas, Px(kMargin), SeparatorY(), client.right - Px(kMargin), SeparatorY() + 1, 0,
              pal.separator);

    // Slider: 4 px rail, accent up to the thumb; the thumb is a disc with an
    // accent dot that grows under the mouse and shrinks while dragged.
    const RECT row = SliderRow();
    const double midY = (row.top + row.bottom) / 2.0;
    const double railHalf = Dip(2);
    const double thumbR = Dip(kThumb) / 2;
    const double thumbX = ThumbX(g.level) + 0.5;
    const double railLeft = TrackLeft() - thumbR, railRight = TrackRight() + thumbR;
    FillRound(canvas, railLeft, midY - railHalf, railRight, midY + railHalf, railHalf, pal.rail);
    FillRound(canvas, railLeft, midY - railHalf, thumbX, midY + railHalf, railHalf, gAccent);
    FillRound(canvas, thumbX - thumbR, midY - thumbR, thumbX + thumbR, midY + thumbR, thumbR,
              pal.thumbBorder);
    const double discR = thumbR - Dip(1);
    FillRound(canvas, thumbX - discR, midY - discR, thumbX + discR, midY + discR, discR,
              pal.thumb);
    const double dotR = Dip(gDragging ? 5 : gHover == kHoverSlider ? 7 : 6);
    FillRound(canvas, thumbX - dotR, midY - dotR, thumbX + dotR, midY + dotR, dotR, gAccent);

    // Text, by GDI on the same bitmap.
    HGDIOBJ oldFont = SelectObject(mem, gFont);
    SetBkMode(mem, TRANSPARENT);
    for (int i = 0; i < kCols * kRows; ++i) {
        wchar_t label[8];
        swprintf_s(label, L"%d%%", (i + 1) * 10);
        RECT r = CellRect(i);
        SetTextColor(mem, GetRValue(shades[i]) > 140 ? RGB(27, 27, 27) : RGB(255, 255, 255));
        DrawTextW(mem, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    wchar_t value[8];
    swprintf_s(value, L"%d%%", g.level);
    RECT valueRect{TrackRight() + Px(kThumb) / 2, row.top, row.right, row.bottom};
    SelectObject(mem, gValueFont);
    SetTextColor(mem, pal.text);
    DrawTextW(mem, value, -1, &valueRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, oldFont);

    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void SetHover(int hover) {
    if (hover == gHover) return;
    gHover = hover;
    InvalidateRect(gMain, nullptr, FALSE);
}

// Follows the Windows light/dark settings; called at start and on change.
void ApplyTheme() {
    gPalette = AppsLight() ? &kLight : &kDark;
    // Attribute 20 = immersive dark mode (Windows 10 20H1 and later).
    const BOOL dark = gPalette == &kDark;
    gAccent = ReadAccent(dark != FALSE);
    DwmSetWindowAttribute(gMain, 20, &dark, sizeof dark);
    SetWindowPos(gMain, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(gMain, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
    if (gTrayAdded) UpdateTray();
    UpdateThumbs();
}

void ShowPanel() {
    ShowWindow(gMain, SW_RESTORE);  // shows it if hidden, restores it if minimized
    SetForegroundWindow(gMain);
}

// Hands our memory pages back to Windows while we sleep in the tray; they
// come back on demand (soft faults) when the panel or the menu opens.
void TrimMemory() { SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                                             static_cast<SIZE_T>(-1)); }

void HidePanel() {  // into the tray
    ShowWindow(gMain, SW_HIDE);
    TrimMemory();
}

void ShowMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    if (!IsWindowVisible(gMain) || IsIconic(gMain)) {
        AppendMenuW(menu, MF_STRING, kMenuOpen, gText->open);
        SetMenuDefaultItem(menu, kMenuOpen, FALSE);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    wchar_t preset[16];
    swprintf_s(preset, L"%d%%", g.preset);
    AppendMenuW(menu, MF_STRING | (g.level == g.preset ? MF_CHECKED : 0), kMenuPreset, preset);
    AppendMenuW(menu, MF_STRING | (g.level == 100 ? MF_CHECKED : 0), kMenuFull, L"100%");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g.remember ? MF_CHECKED : 0), kMenuRemember,
                gText->remember);
    AppendMenuW(menu, MF_STRING | (g.startup ? MF_CHECKED : 0), kMenuStartup, gText->startup);
    AppendMenuW(menu,
                MF_STRING | (g.dimAtLogin ? MF_CHECKED : 0) | (g.startup ? 0 : MF_GRAYED),
                kMenuDimAtLogin, gText->dimAtLogin);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, gText->quit);

    SetForegroundWindow(gMain);  // so the menu closes when clicking elsewhere
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        pt.x, pt.y, 0, gMain, nullptr);
    PostMessageW(gMain, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (command) {
    case kMenuOpen:
        ShowPanel();
        break;
    case kMenuPreset:
        SetLevel(g.preset);
        break;
    case kMenuFull:
        SetLevel(100);
        break;
    case kMenuRemember:
        g.remember = !g.remember;
        SaveSettings();
        break;
    case kMenuStartup:
        g.startup = !g.startup;
        ApplyStartup();
        SaveSettings();
        break;
    case kMenuDimAtLogin:
        g.dimAtLogin = !g.dimAtLogin;
        SaveSettings();
        break;
    case kMenuQuit:
        DestroyWindow(gMain);
        break;
    }
}

void FitToDpi(const RECT* suggested) {
    SIZE client = ClientSize();
    RECT r{0, 0, client.cx, client.cy};
    AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongW(gMain, GWL_STYLE)), FALSE, 0,
                             gDpi);
    const int x = suggested ? suggested->left : 0;
    const int y = suggested ? suggested->top : 0;
    SetWindowPos(gMain, nullptr, x, y, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | (suggested ? 0 : SWP_NOMOVE));
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == gMsgTaskbarCreated && msg != 0) {  // Explorer restarted
        gTrayAdded = false;
        UpdateTray();
        return 0;
    }
    if (msg == gMsgButtonCreated && msg != 0) {  // the panel got a taskbar icon
        if (!gTaskbar &&
            SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&gTaskbar))))
            gTaskbar->HrInit();
        gThumbsAdded = false;
        UpdateThumbs();
        return 0;
    }

    switch (msg) {
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (OnSlider(pt)) {
            gDragging = true;
            SetCapture(hwnd);
            SetLevel(LevelAtX(pt.x), true, true);
            return 0;
        }
        const int cell = CellAt(pt);
        if (cell >= 0) {
            gPressed = cell;
            SetLevel((cell + 1) * 10);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!gTracking) {
            TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, hwnd, 0};
            gTracking = TrackMouseEvent(&track) != FALSE;
        }
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (gDragging) SetLevel(LevelAtX(pt.x), true, true);
        const int cell = CellAt(pt);
        if (gPressed >= 0 && cell != gPressed) gPressed = -1;  // slid off the button
        SetHover(OnSlider(pt) ? kHoverSlider : cell);
        return 0;
    }
    case WM_MOUSELEAVE:
        gTracking = false;
        gPressed = -1;
        SetHover(-1);
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (gPressed >= 0) {
            gPressed = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (gDragging) {
            gDragging = false;  // before ReleaseCapture, which re-enters here
            if (GetCapture() == hwnd) ReleaseCapture();
            SetLevel(g.level, true);  // save, tray and taskbar icons
        }
        return 0;
    case WM_MOUSEWHEEL: {
        // 5 % per notch (WHEEL_DELTA); touchpads send many smaller deltas.
        gWheelDelta += GET_WHEEL_DELTA_WPARAM(wp);
        const int notches = gWheelDelta / WHEEL_DELTA;
        gWheelDelta %= WHEEL_DELTA;
        if (notches) SetLevel(g.level + 5 * notches, true);
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);
        ShowMenu(pt);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) HidePanel();
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == THBN_CLICKED) {
            if (LOWORD(wp) == kThumbPreset) SetLevel(g.preset);
            else if (LOWORD(wp) == kThumbFull) SetLevel(100);
            return 0;
        }
        break;
    case WM_SIZE:
        // Every minimize (button, taskbar, Win+D, Win+M, Win+Down) goes to the tray.
        if (wp == SIZE_MINIMIZED) HidePanel();
        return 0;
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
            Toggle();
            break;
        case NIN_KEYSELECT: {
            // Enter on the focused icon can send NIN_KEYSELECT twice.
            static DWORD last;
            const DWORD now = GetTickCount();
            if (now - last >= GetDoubleClickTime()) Toggle();
            last = now;
            break;
        }
        case WM_CONTEXTMENU:
            ShowMenu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
            break;
        }
        return 0;
    case WM_TIMER:
        if (wp == kTimerGamma) {
            CheckGamma();
        } else if (wp == kTimerTrim) {
            KillTimer(hwnd, kTimerTrim);
            TrimMemory();
        }
        return 0;
    case WM_DISPLAYCHANGE:
        LoadScreens();
        SetLevel(g.level, gFromSlider, true);
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0)
            ApplyTheme();
        break;
    case WM_DWMCOLORIZATIONCOLORCHANGED:  // accent colour changed
        ApplyTheme();
        break;
    case WM_DPICHANGED:
        gDpi = HIWORD(wp);
        MakeFont();
        FitToDpi(reinterpret_cast<const RECT*>(lp));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ENDSESSION:
        // Logoff, shutdown, or the installer closing the app (Restart Manager)
        // for an upgrade or an uninstall: quit cleanly.
        if (wp) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:  // the close button quits, like any app
        RestoreGamma();
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    gInst = instance;
    gText = &PickText();
    // No text field anywhere: skip the text input services (TSF, IME) that
    // Windows would otherwise load into every thread with a window.
    ImmDisableTextFrameService(static_cast<DWORD>(-1));
    ImmDisableIME(static_cast<DWORD>(-1));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\Penumbra.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // The running instance may still be creating its window: wait up to 3 s.
        for (int tries = 0; tries < 30; ++tries) {
            if (HWND other = FindWindowW(kMainClass, nullptr)) {
                ShowWindow(other, SW_RESTORE);
                SetForegroundWindow(other);
                break;
            }
            Sleep(100);
        }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadSettings();
    ApplyStartup();  // syncs the Run value with the setting and this exe's path
    // A login launch while "Start with Windows" is off comes from a stale Run
    // value (an MSI repair writes it back): ApplyStartup just removed it; quit.
    const bool atLogin = commandLine && wcsstr(commandLine, L"/startup");
    if (atLogin && !g.startup) return 0;
    SaveSettings();

    gMsgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    gMsgButtonCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");

    WNDCLASSEXW mainClass{sizeof mainClass};
    mainClass.lpfnWndProc = MainProc;
    mainClass.hInstance = instance;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    mainClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                      SmallIconSize(), SmallIconSize(), 0));
    mainClass.lpszClassName = kMainClass;
    RegisterClassExW(&mainClass);

    gDpi = GetDpiForSystem();
    gMain = CreateWindowExW(0, kMainClass, kAppName,
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, instance,
                            nullptr);
    if (!gMain) return 1;
    ApplyTheme();
    gDpi = GetDpiForWindow(gMain);
    MakeFont();
    FitToDpi(nullptr);
    ChangeWindowMessageFilterEx(gMain, gMsgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(gMain, gMsgButtonCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(gMain, WM_COMMAND, MSGFLT_ALLOW, nullptr);

    // At login the panel stays hidden in the tray, and the screen stays at
    // 100 % unless "Dim at Windows startup" is ticked. That 100 % is not saved
    // (live), so the saved level and the preset come back on a manual launch.
    if (!atLogin) ShowWindow(gMain, SW_SHOWNORMAL);
    RecoverGamma();  // a crash may have left our tables on the screens
    LoadScreens();
    if (atLogin && !g.dimAtLogin) {
        SetLevel(100, false, true);
        UpdateTray();
    } else {
        SetLevel(g.level, gFromSlider);
    }

    // Startup touched pages it will never use again (loading, settings, first
    // paint): hand them back, at once when hidden in the tray, else once the
    // panel is up.
    if (atLogin) TrimMemory();
    else SetTimer(gMain, kTimerTrim, 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (gTaskbar) gTaskbar->Release();
    CoUninitialize();
    CloseHandle(single);
    return 0;
}
