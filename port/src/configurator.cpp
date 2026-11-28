#include "configurator.h"
#include "config.h"
#include "dist_tools.h"
#include "updater.h"
#include "../assets/rr3_icon.h"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- Graphite Neutral Ramp + Single Brand Accent (unslop tokens) ----
constexpr COLORREF kColSidebar     = RGB(15, 15, 16);    // #0F0F10 deepest rail
constexpr COLORREF kColBg          = RGB(20, 20, 21);    // #141415 main pane
constexpr COLORREF kColCard        = RGB(26, 26, 28);    // #1A1A1C grouped surface
constexpr COLORREF kColRaised      = RGB(35, 35, 38);    // #232326 controls / inputs
constexpr COLORREF kColRaisedHov   = RGB(45, 45, 49);    // #2D2D31 hover step
constexpr COLORREF kColRaisedPress = RGB(30, 30, 33);    // #1E1E21 press step
constexpr COLORREF kColSegActive   = RGB(52, 52, 57);    // #343439 elevated active segment
constexpr COLORREF kColAccent      = RGB(47, 129, 247);  // #2F81F7 restrained RR3 cobalt
constexpr COLORREF kColAccentHov   = RGB(68, 147, 248);  // #4493F8
constexpr COLORREF kColGreen       = RGB(76, 195, 138);  // #4CC38A toned positive
constexpr COLORREF kColAmber       = RGB(229, 164, 59);  // #E5A43B toned warning
constexpr COLORREF kColRed         = RGB(229, 72, 77);   // #E5484D toned error
constexpr COLORREF kColText        = RGB(237, 237, 237); // #EDEDED primary
constexpr COLORREF kColSecondary   = RGB(161, 161, 166); // #A1A1A6 secondary
constexpr COLORREF kColMuted       = RGB(122, 122, 129); // #7A7A81 muted

// Base 4px-grid window geometry (800 x 560)
constexpr int kBaseW     = 800;
constexpr int kBaseH     = 560;
constexpr int kSidebarW  = 216;
constexpr int kContentX  = kSidebarW + 28; // 244
constexpr int kContentW  = kBaseW - kSidebarW - 56; // 528

enum EditId : int {
    IDC_EDIT_CUSTOM_W = 3001,
    IDC_EDIT_CUSTOM_H,
    IDC_EDIT_RAMP,
    IDC_EDIT_RETURN,
    IDC_EDIT_APK,
    IDC_EDIT_DATA,
};

struct ResolutionChoice {
    const wchar_t* label;
    int w;
    int h;
};
constexpr ResolutionChoice kResChoices[] = {
    {L"Auto-fit monitor", 0, 0},
    {L"1280 x 720",       1280, 720},
    {L"1366 x 768",       1366, 768},
    {L"1600 x 900",       1600, 900},
    {L"1920 x 1080",      1920, 1080},
    {L"2560 x 1440",      2560, 1440},
    {L"Custom",           -1, -1},
};

struct FpsChoice {
    const wchar_t* label;
    int fps;
};
constexpr FpsChoice kFpsChoices[] = {
    {L"Uncapped", 0},
    {L"30 FPS",   30},
    {L"60 FPS",   60},
    {L"90 FPS",   90},
    {L"120 FPS",  120},
    {L"144 FPS",  144},
    {L"165 FPS",  165},
    {L"240 FPS",  240},
};

struct ScaleChoice {
    const wchar_t* label;
    float scale;
};
constexpr ScaleChoice kScaleChoices[] = {
    {L"75%",  0.75f},
    {L"100%", 1.0f},
    {L"125%", 1.25f},
    {L"150%", 1.5f},
    {L"200%", 2.0f},
};

struct AnisoChoice {
    const wchar_t* label;
    int aniso;
};
constexpr AnisoChoice kAnisoChoices[] = {
    {L"Off", 1},
    {L"4x",  4},
    {L"8x",  8},
    {L"16x", 16},
};

struct KeyBindSlot {
    const wchar_t* label;
    std::string PcConfig::* field;
};
const KeyBindSlot kKeySlots[] = {
    {L"Throttle",  &PcConfig::key_throttle},
    {L"Brake",     &PcConfig::key_brake},
    {L"Alt brake", &PcConfig::key_handbrake},
    {L"Steer left",&PcConfig::key_left},
    {L"Steer right",&PcConfig::key_right},
    {L"Camera",    &PcConfig::key_camera},
    {L"Look back", &PcConfig::key_rear_view},
    {L"Pause",     &PcConfig::key_pause},
};

enum class HitKind {
    None,
    Nav,
    WindowMode,
    Quality,
    DropRes,
    DropFps,
    DropScale,
    DropAniso,
    ToggleVsync,
    ToggleMenuFps,
    ToggleVerbose,
    ToggleOnlineMode,
    KeyBind,
    BtnResetKeys,
    BtnBrowseApk,
    BtnExtractApk,
    BtnBrowseData,
    BtnImportSave,
    BtnExportSave,
    BtnOpenSaveDir,
    ToggleAutoUpdate,
    BtnCheckUpdate,
    BtnOpenUpdate,
    BtnPlay,
    BtnSave,
    BtnReset,
};

constexpr UINT WM_APP_UPDATE_CHECK_DONE = WM_APP + 10;
constexpr UINT WM_APP_UPDATE_PROGRESS   = WM_APP + 11;
constexpr UINT WM_APP_UPDATE_DONE       = WM_APP + 12;

struct HitZone {
    RECT rc{};
    HitKind kind = HitKind::None;
    int index = 0;
};

struct SpringState {
    float val = 0.0f;
    float vel = 0.0f;
    float target = 0.0f;

    void Snap(float v) { val = v; target = v; vel = 0.0f; }

    // Critically damped Apple-style spring (damping = 1.0, response in seconds).
    // Returns true while still moving.
    bool Step(float dt, float response = 0.22f) {
        float diff = val - target;
        if (std::fabs(diff) < 0.0008f && std::fabs(vel) < 0.004f) {
            val = target;
            vel = 0.0f;
            return false;
        }
        float omega = 6.2831853f / std::max(0.05f, response);
        float e = std::exp(-omega * dt);
        float new_val = target + (diff + (vel + omega * diff) * dt) * e;
        float new_vel = (vel - (vel + omega * diff) * omega * dt) * e;
        val = new_val;
        vel = new_vel;
        return true;
    }
};

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

Gdiplus::Color ToGdip(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

Gdiplus::Color BlendColor(COLORREF a, COLORREF b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    BYTE r = static_cast<BYTE>(std::round(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t));
    BYTE g = static_cast<BYTE>(std::round(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t));
    BYTE bl = static_cast<BYTE>(std::round(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
    return Gdiplus::Color(255, r, g, bl);
}

void AddRoundRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = std::max(1.0f, std::min(r * 2.0f, std::min(w, h)));
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRoundRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, const Gdiplus::Brush& br) {
    Gdiplus::GraphicsPath p;
    AddRoundRect(p, x, y, w, h, r);
    g.FillPath(&br, &p);
}

void DrawRoundRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, const Gdiplus::Pen& pen) {
    Gdiplus::GraphicsPath p;
    AddRoundRect(p, x, y, w, h, r);
    g.DrawPath(&pen, &p);
}

// Consistent 16px, 1.5px-stroke vector line icons for sidebar nav
void DrawNavIcon(Gdiplus::Graphics& g, int tab, float cx, float cy, float scale, const Gdiplus::Color& col) {
    Gdiplus::Pen pen(col, 1.5f * scale);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    float s = scale;

    if (tab == 0) {
        // Display (monitor)
        DrawRoundRect(g, cx - 7.0f * s, cy - 5.5f * s, 14.0f * s, 9.5f * s, 2.0f * s, pen);
        g.DrawLine(&pen, cx - 3.0f * s, cy + 6.5f * s, cx + 3.0f * s, cy + 6.5f * s);
        g.DrawLine(&pen, cx, cy + 4.0f * s, cx, cy + 6.5f * s);
    } else if (tab == 1) {
        // Graphics (sliders)
        g.DrawLine(&pen, cx - 6.5f * s, cy - 4.0f * s, cx + 6.5f * s, cy - 4.0f * s);
        g.DrawLine(&pen, cx - 6.5f * s, cy,            cx + 6.5f * s, cy);
        g.DrawLine(&pen, cx - 6.5f * s, cy + 4.0f * s, cx + 6.5f * s, cy + 4.0f * s);
        Gdiplus::SolidBrush br(ToGdip(kColSidebar));
        g.FillEllipse(&br,  cx + 1.0f * s, cy - 6.0f * s, 4.0f * s, 4.0f * s);
        g.DrawEllipse(&pen, cx + 1.0f * s, cy - 6.0f * s, 4.0f * s, 4.0f * s);
        g.FillEllipse(&br,  cx - 4.5f * s, cy - 2.0f * s, 4.0f * s, 4.0f * s);
        g.DrawEllipse(&pen, cx - 4.5f * s, cy - 2.0f * s, 4.0f * s, 4.0f * s);
        g.FillEllipse(&br,  cx + 0.5f * s, cy + 2.0f * s, 4.0f * s, 4.0f * s);
        g.DrawEllipse(&pen, cx + 0.5f * s, cy + 2.0f * s, 4.0f * s, 4.0f * s);
    } else if (tab == 2) {
        // Controls (gamepad)
        DrawRoundRect(g, cx - 7.5f * s, cy - 4.5f * s, 15.0f * s, 9.0f * s, 4.0f * s, pen);
        g.DrawLine(&pen, cx - 4.5f * s, cy, cx - 1.5f * s, cy);
        g.DrawLine(&pen, cx - 3.0f * s, cy - 1.5f * s, cx - 3.0f * s, cy + 1.5f * s);
        Gdiplus::SolidBrush dot(col);
        g.FillEllipse(&dot, cx + 2.2f * s, cy - 1.8f * s, 1.8f * s, 1.8f * s);
        g.FillEllipse(&dot, cx + 4.2f * s, cy + 0.2f * s, 1.8f * s, 1.8f * s);
    } else {
        // Files & saves (folder)
        Gdiplus::PointF pts[6] = {
            {cx - 7.0f * s, cy - 5.0f * s},
            {cx - 2.0f * s, cy - 5.0f * s},
            {cx - 0.5f * s, cy - 3.0f * s},
            {cx + 7.0f * s, cy - 3.0f * s},
            {cx + 7.0f * s, cy + 5.5f * s},
            {cx - 7.0f * s, cy + 5.5f * s},
        };
        g.DrawPolygon(&pen, pts, 6);
    }
}

std::string VkToSdlKeyName(WPARAM vk, LPARAM lparam) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    switch (vk) {
        case VK_SPACE:   return "Space";
        case VK_RETURN:  return "Return";
        case VK_ESCAPE:  return "Escape";
        case VK_TAB:     return "Tab";
        case VK_BACK:    return "Backspace";
        case VK_UP:      return "Up";
        case VK_DOWN:    return "Down";
        case VK_LEFT:    return "Left";
        case VK_RIGHT:   return "Right";
        case VK_SHIFT:   return ((MapVirtualKeyW((lparam >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT) ? "Right Shift" : "Left Shift");
        case VK_CONTROL: return ((lparam & (1 << 24)) ? "Right Ctrl" : "Left Ctrl");
        case VK_MENU:    return ((lparam & (1 << 24)) ? "Right Alt" : "Left Alt");
        case VK_OEM_COMMA:  return ",";
        case VK_OEM_PERIOD: return ".";
        case VK_OEM_MINUS:  return "-";
        case VK_OEM_PLUS:   return "=";
        case VK_OEM_4:      return "[";
        case VK_OEM_6:      return "]";
        case VK_OEM_1:      return ";";
        case VK_OEM_7:      return "'";
        case VK_OEM_2:      return "/";
        case VK_OEM_5:      return "\\";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
    wchar_t name[64]{};
    if (GetKeyNameTextW(static_cast<LONG>(lparam), name, static_cast<int>(std::size(name))) > 0) {
        return WideToUtf8(name);
    }
    return {};
}

bool PickFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter, std::filesystem::path& out_path) {
    wchar_t buf[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buf));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out_path = buf;
    return true;
}

bool PickSaveFilesDialog(HWND owner, const wchar_t* title, const wchar_t* filter,
                         std::vector<std::filesystem::path>& out_paths) {
    out_paths.clear();
    std::vector<wchar_t> buf(65536, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;

    const wchar_t* p = buf.data();
    std::wstring first(p);
    p += first.size() + 1;
    if (*p == L'\0') {
        out_paths.emplace_back(first);
    } else {
        std::filesystem::path dir(first);
        while (*p != L'\0') {
            std::wstring name(p);
            out_paths.push_back(dir / name);
            p += name.size() + 1;
        }
    }
    return !out_paths.empty();
}

bool SaveFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter,
                    const wchar_t* default_name, std::filesystem::path& out_path) {
    wchar_t buf[32768]{};
    if (default_name) wcscpy_s(buf, default_name);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buf));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    out_path = buf;
    if (!out_path.has_extension()) {
        out_path += (ofn.nFilterIndex == 2) ? L".zip" : L".dat";
    }
    return true;
}

bool PickFolderDialog(HWND owner, const wchar_t* title, std::filesystem::path& out_dir) {
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                out_dir = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

bool GetPngEncoderClsid(CLSID& out_clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<uint8_t> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            out_clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

bool CaptureWindowToPng(HWND hwnd, const std::filesystem::path& out_png) {
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC hdc_win = GetDC(nullptr);
    HDC hdc_mem = CreateCompatibleDC(hdc_win);
    HBITMAP hbm = CreateCompatibleBitmap(hdc_win, w, h);
    HGDIOBJ old = SelectObject(hdc_mem, hbm);
    BOOL ok = PrintWindow(hwnd, hdc_mem, 0x00000002u);
    if (!ok) ok = PrintWindow(hwnd, hdc_mem, 0);
    SelectObject(hdc_mem, old);
    DeleteDC(hdc_mem);
    ReleaseDC(nullptr, hdc_win);
    bool saved = false;
    if (ok) {
        CLSID clsid{};
        if (GetPngEncoderClsid(clsid)) {
            std::error_code ec;
            if (out_png.has_parent_path()) std::filesystem::create_directories(out_png.parent_path(), ec);
            Gdiplus::Bitmap bmp(hbm, nullptr);
            saved = (bmp.Save(out_png.c_str(), &clsid, nullptr) == Gdiplus::Ok);
        }
    }
    DeleteObject(hbm);
    return saved;
}

// ---- Custom Rounded Dark Dropdown Popup (zero Win95 square combo frames) ----
struct PopupState {
    std::vector<std::wstring> items;
    int selected = 0;
    int hovered = -1;
    int chosen = -1;
    UINT dpi = 96;
    bool done = false;
    HWND hwnd = nullptr;

    int S(int px) const { return MulDiv(px, static_cast<int>(dpi), 96); }
    float Sf(float px) const { return px * static_cast<float>(dpi) / 96.0f; }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* st = reinterpret_cast<PopupState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            st = static_cast<PopupState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            st->hwnd = h;
            return TRUE;
        }
        if (!st) return DefWindowProcW(h, msg, wp, lp);

        switch (msg) {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(h, &ps);
                RECT rc{};
                GetClientRect(h, &rc);
                int cw = rc.right - rc.left;
                int ch = rc.bottom - rc.top;

                HDC mdc = CreateCompatibleDC(hdc);
                HBITMAP mbm = CreateCompatibleBitmap(hdc, cw, ch);
                HGDIOBJ old = SelectObject(mdc, mbm);

                Gdiplus::Graphics g(mdc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

                Gdiplus::SolidBrush br_bg(ToGdip(RGB(30, 30, 34)));
                g.FillRectangle(&br_bg, 0, 0, cw, ch);
                Gdiplus::Pen pen_bd(Gdiplus::Color(36, 255, 255, 255), 1.0f);
                g.DrawRectangle(&pen_bd, 0.5f, 0.5f, static_cast<float>(cw) - 1.0f, static_cast<float>(ch) - 1.0f);

                Gdiplus::FontFamily ff(L"Segoe UI");
                Gdiplus::Font font(&ff, st->Sf(13.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
                Gdiplus::StringFormat sf;
                sf.SetAlignment(Gdiplus::StringAlignmentNear);
                sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

                int pad = st->S(4);
                int row_h = st->S(30);
                for (int i = 0; i < static_cast<int>(st->items.size()); ++i) {
                    int y = pad + i * row_h;
                    if (i == st->hovered) {
                        Gdiplus::SolidBrush br_h(ToGdip(RGB(48, 48, 54)));
                        FillRoundRect(g, static_cast<float>(pad), static_cast<float>(y),
                                      static_cast<float>(cw - pad * 2), static_cast<float>(row_h),
                                      st->Sf(6.0f), br_h);
                    }
                    if (i == st->selected) {
                        Gdiplus::Pen ck(ToGdip(kColAccent), 1.7f * st->dpi / 96.0f);
                        ck.SetStartCap(Gdiplus::LineCapRound);
                        ck.SetEndCap(Gdiplus::LineCapRound);
                        ck.SetLineJoin(Gdiplus::LineJoinRound);
                        float x0 = static_cast<float>(pad + st->S(10));
                        float y0 = static_cast<float>(y + row_h / 2);
                        float sc = static_cast<float>(st->dpi) / 96.0f;
                        g.DrawLine(&ck, x0, y0, x0 + 3.0f * sc, y0 + 3.0f * sc);
                        g.DrawLine(&ck, x0 + 3.0f * sc, y0 + 3.0f * sc, x0 + 8.5f * sc, y0 - 3.5f * sc);
                    }
                    Gdiplus::SolidBrush br_t(ToGdip(i == st->selected ? kColText : kColSecondary));
                    Gdiplus::RectF tr(static_cast<float>(pad + st->S(26)), static_cast<float>(y),
                                      static_cast<float>(cw - pad * 2 - st->S(32)), static_cast<float>(row_h));
                    g.DrawString(st->items[i].c_str(), -1, &font, tr, &sf, &br_t);
                }

                BitBlt(hdc, 0, 0, cw, ch, mdc, 0, 0, SRCCOPY);
                SelectObject(mdc, old);
                DeleteObject(mbm);
                DeleteDC(mdc);
                EndPaint(h, &ps);
                return 0;
            }
            case WM_MOUSEMOVE: {
                int my = GET_Y_LPARAM(lp);
                int idx = (my - st->S(4)) / st->S(30);
                if (idx < 0 || idx >= static_cast<int>(st->items.size())) idx = -1;
                if (idx != st->hovered) {
                    st->hovered = idx;
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            case WM_LBUTTONUP: {
                int mx = GET_X_LPARAM(lp);
                int my = GET_Y_LPARAM(lp);
                RECT rc{};
                GetClientRect(h, &rc);
                POINT pt{mx, my};
                if (PtInRect(&rc, pt)) {
                    int idx = (my - st->S(4)) / st->S(30);
                    if (idx >= 0 && idx < static_cast<int>(st->items.size())) {
                        st->chosen = idx;
                    }
                }
                st->done = true;
                return 0;
            }
            case WM_LBUTTONDOWN: {
                int mx = GET_X_LPARAM(lp);
                int my = GET_Y_LPARAM(lp);
                RECT rc{};
                GetClientRect(h, &rc);
                POINT pt{mx, my};
                if (!PtInRect(&rc, pt)) {
                    st->done = true;
                }
                return 0;
            }
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) st->done = true;
                return 0;
            case WM_KILLFOCUS:
            case WM_ACTIVATE:
                if (msg == WM_KILLFOCUS || LOWORD(wp) == WA_INACTIVE) {
                    st->done = true;
                }
                return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};

int ShowCustomDropdown(HWND owner, HINSTANCE hinst, UINT dpi, RECT anchor_client,
                       const std::vector<std::wstring>& items, int current_idx) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = PopupState::Proc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"RR3DropdownPopup";
        RegisterClassExW(&wc);
        registered = true;
    }

    PopupState st;
    st.items = items;
    st.selected = current_idx;
    st.hovered = current_idx;
    st.dpi = dpi;

    POINT pt{anchor_client.left, anchor_client.bottom + st.S(4)};
    ClientToScreen(owner, &pt);
    int w = std::max<int>(anchor_client.right - anchor_client.left, st.S(140));
    int h = st.S(8) + static_cast<int>(items.size()) * st.S(30);

    // Clamp inside monitor work area (flip upward if near screen bottom)
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(mon, &mi)) {
        if (pt.y + h > mi.rcWork.bottom) {
            POINT top_pt{anchor_client.left, anchor_client.top - h - st.S(4)};
            ClientToScreen(owner, &top_pt);
            pt.y = std::max<LONG>(mi.rcWork.top, top_pt.y);
        }
    }

    HWND popup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"RR3DropdownPopup", L"",
                                 WS_POPUP, pt.x, pt.y, w, h, owner, nullptr, hinst, &st);
    if (!popup) return -1;

    // Windows 11 rounded popup corners
    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(popup, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));

    ShowWindow(popup, SW_SHOW);
    SetFocus(popup);
    SetCapture(popup);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (GetCapture() == popup) ReleaseCapture();
    DestroyWindow(popup);
    return st.chosen;
}

// ---- Custom Dark GDI+ Update Pop-Up (Download, Progress Bar, Auto-Install & Self-Replace) ----
enum class UpdatePopupResult {
    Later,
    SkipVersion,
    ApplyingUpdate,
};

struct UpdatePopupState {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HINSTANCE hinst = nullptr;
    UINT dpi = 96;
    ReleaseInfo rel;
    bool auto_start = false;
    std::filesystem::path screenshot_out;

    enum class Phase { Idle, Downloading, Extracting, Error, Done };
    Phase phase = Phase::Idle;
    int progress_pct = 0;
    std::wstring status_text;
    UpdatePopupResult result = UpdatePopupResult::Later;
    bool closed = false;

    std::atomic<bool> cancel_flag{false};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::thread worker;

    enum class Btn { None, UpdateNow, Later, Skip, ReleaseLink, Cancel };
    struct Zone { RECT rc{}; Btn btn = Btn::None; };
    std::vector<Zone> zones;
    Btn hovered = Btn::None;
    Btn pressed = Btn::None;

    int S(int px) const { return MulDiv(px, static_cast<int>(dpi), 96); }
    float Sf(float px) const { return px * static_cast<float>(dpi) / 96.0f; }

    static std::wstring FormatBytes(uint64_t b) {
        wchar_t buf[48]{};
        if (b >= 1024ULL * 1024ULL) {
            swprintf_s(buf, L"%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
        } else if (b >= 1024ULL) {
            swprintf_s(buf, L"%.0f KB", static_cast<double>(b) / 1024.0);
        } else {
            swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(b));
        }
        return buf;
    }

    void StartDownload() {
        if (phase == Phase::Downloading || phase == Phase::Extracting) return;
        if (worker.joinable()) worker.join();
        cancel_flag.store(false, std::memory_order_relaxed);
        phase = Phase::Downloading;
        progress_pct = 0;
        status_text = L"Connecting to GitHub...";
        InvalidateRect(hwnd, nullptr, FALSE);

        HWND target = hwnd;
        auto alive_tok = alive;
        std::string url = rel.download_url;
        std::string asset_name = rel.asset_name.empty() ? "RealRacing3-PC-Port.zip" : rel.asset_name;
        uint64_t expected_sz = rel.asset_size;
        std::filesystem::path install_dir = GetExecutableDir();
        std::filesystem::path zip_path = install_dir / "_update_download.zip";
        std::filesystem::path staging_dir = install_dir / "_update_staging";

        worker = std::thread([this, target, alive_tok, url, asset_name, expected_sz, install_dir, zip_path, staging_dir]() {
            auto post_progress = [&](int pct, const std::wstring& txt) {
                if (!alive_tok->load(std::memory_order_acquire)) return;
                auto* msg = new std::wstring(txt);
                if (!PostMessageW(target, WM_APP_UPDATE_PROGRESS, static_cast<WPARAM>(pct),
                                  reinterpret_cast<LPARAM>(msg))) {
                    delete msg;
                }
            };
            auto post_done = [&](bool ok, const std::wstring& txt) {
                if (!alive_tok->load(std::memory_order_acquire)) return;
                auto* msg = new std::wstring(txt);
                if (!PostMessageW(target, WM_APP_UPDATE_DONE, ok ? 1 : 0, reinterpret_cast<LPARAM>(msg))) {
                    delete msg;
                }
            };

            std::string err;
            int last_pct = -1;
            bool ok = DownloadFileWithProgress(
                url, zip_path,
                [&](uint64_t got, uint64_t total) {
                    uint64_t denom = total > 0 ? total : expected_sz;
                    int pct = denom > 0 ? static_cast<int>(std::min<uint64_t>(100, (got * 100) / denom)) : 0;
                    if (pct != last_pct) {
                        last_pct = pct;
                        std::wstring s = L"Downloading " + Utf8ToWide(asset_name) + L" (" + FormatBytes(got);
                        if (denom > 0) s += L" / " + FormatBytes(denom);
                        s += L")";
                        post_progress(pct, s);
                    }
                },
                &cancel_flag, err);
            if (!ok) {
                post_done(false, Utf8ToWide(err));
                return;
            }

            post_progress(101, L"Extracting update files...");
            if (!ExtractUpdateZipToStaging(zip_path, staging_dir, install_dir, err)) {
                std::error_code ec;
                std::filesystem::remove(zip_path, ec);
                post_done(false, Utf8ToWide(err));
                return;
            }

            post_progress(101, L"Restarting to apply update...");
            if (!LaunchUpdateApplyHelper(staging_dir, zip_path, install_dir, err)) {
                post_done(false, Utf8ToWide(err));
                return;
            }
            post_done(true, L"Applying update...");
        });
    }

    Btn HitTest(int mx, int my) const {
        POINT pt{mx, my};
        for (const auto& z : zones) {
            if (PtInRect(&z.rc, pt)) return z.btn;
        }
        return Btn::None;
    }

    void Paint(HDC hdc) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int cw = rc.right - rc.left;
        int ch = rc.bottom - rc.top;
        if (cw <= 0 || ch <= 0) return;

        HDC mdc = CreateCompatibleDC(hdc);
        HBITMAP mbm = CreateCompatibleBitmap(hdc, cw, ch);
        HGDIOBJ old = SelectObject(mdc, mbm);
        zones.clear();

        Gdiplus::Graphics g(mdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        Gdiplus::SolidBrush br_bg(ToGdip(kColBg));
        g.FillRectangle(&br_bg, 0, 0, cw, ch);

        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::FontFamily ff_mono(L"Consolas");
        Gdiplus::Font f_title(&ff, Sf(18.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font f_body(&ff, Sf(13.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font f_semi(&ff, Sf(13.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font f_sub(&ff, Sf(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font f_mono(&ff_mono, Sf(11.5f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

        Gdiplus::SolidBrush br_txt(ToGdip(kColText));
        Gdiplus::SolidBrush br_sec(ToGdip(kColSecondary));
        Gdiplus::SolidBrush br_mut(ToGdip(kColMuted));
        Gdiplus::StringFormat sf_left;
        sf_left.SetAlignment(Gdiplus::StringAlignmentNear);
        sf_left.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::StringFormat sf_center;
        sf_center.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf_center.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        // Header + Version Pill
        g.DrawString(L"Update available", -1, &f_title,
                     Gdiplus::RectF(Sf(22.0f), Sf(16.0f), Sf(240.0f), Sf(28.0f)), &sf_left, &br_txt);

        std::wstring pill_txt = L"v" + Utf8ToWide(rel.current_version) + L"  ->  v" + Utf8ToWide(rel.latest_version);
        {
            float px = Sf(290.0f), py = Sf(18.0f), pw = Sf(148.0f), ph = Sf(24.0f);
            Gdiplus::SolidBrush pbg(Gdiplus::Color(44, 47, 129, 247));
            Gdiplus::Pen pbd(Gdiplus::Color(110, 47, 129, 247), 1.0f);
            FillRoundRect(g, px, py, pw, ph, Sf(12.0f), pbg);
            DrawRoundRect(g, px, py, pw, ph, Sf(12.0f), pbd);
            Gdiplus::SolidBrush ptxt(ToGdip(RGB(130, 184, 255)));
            g.DrawString(pill_txt.c_str(), -1, &f_mono, Gdiplus::RectF(px, py, pw, ph), &sf_center, &ptxt);
        }

        std::wstring sub = Utf8ToWide(rel.release_name);
        if (rel.asset_size > 0) sub += L"  ·  " + FormatBytes(rel.asset_size);
        g.DrawString(sub.c_str(), -1, &f_sub,
                     Gdiplus::RectF(Sf(22.0f), Sf(46.0f), Sf(416.0f), Sf(20.0f)), &sf_left, &br_sec);

        // Release notes card (y=76..252)
        {
            Gdiplus::SolidBrush cbg(ToGdip(kColCard));
            Gdiplus::Pen cbd(Gdiplus::Color(22, 255, 255, 255), 1.0f);
            FillRoundRect(g, Sf(22.0f), Sf(76.0f), Sf(416.0f), Sf(174.0f), Sf(10.0f), cbg);
            DrawRoundRect(g, Sf(22.0f), Sf(76.0f), Sf(416.0f), Sf(174.0f), Sf(10.0f), cbd);

            std::wstring notes = rel.release_notes.empty()
                                     ? L"A new version of Real Racing 3 PC Port is ready to install.\nYour saves, settings, and Android game files are kept untouched."
                                     : Utf8ToWide(rel.release_notes);
            Gdiplus::StringFormat sf_wrap;
            sf_wrap.SetAlignment(Gdiplus::StringAlignmentNear);
            sf_wrap.SetLineAlignment(Gdiplus::StringAlignmentNear);
            sf_wrap.SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);
            sf_wrap.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
            g.DrawString(notes.c_str(), -1, &f_sub,
                         Gdiplus::RectF(Sf(36.0f), Sf(88.0f), Sf(388.0f), Sf(150.0f)), &sf_wrap, &br_sec);
        }

        // Progress / Status row (y=262..294)
        if (phase == Phase::Downloading || phase == Phase::Extracting) {
            g.DrawString(status_text.c_str(), -1, &f_sub,
                         Gdiplus::RectF(Sf(22.0f), Sf(260.0f), Sf(416.0f), Sf(18.0f)), &sf_left, &br_txt);
            Gdiplus::SolidBrush trk(ToGdip(kColRaised));
            FillRoundRect(g, Sf(22.0f), Sf(284.0f), Sf(416.0f), Sf(6.0f), Sf(3.0f), trk);
            float frac = (phase == Phase::Extracting) ? 1.0f : std::clamp(progress_pct / 100.0f, 0.04f, 1.0f);
            Gdiplus::SolidBrush fill(ToGdip(phase == Phase::Extracting ? kColGreen : kColAccent));
            FillRoundRect(g, Sf(22.0f), Sf(284.0f), Sf(416.0f * frac), Sf(6.0f), Sf(3.0f), fill);
        } else if (phase == Phase::Error) {
            Gdiplus::SolidBrush br_err(ToGdip(kColRed));
            g.DrawString(status_text.c_str(), -1, &f_sub,
                         Gdiplus::RectF(Sf(22.0f), Sf(264.0f), Sf(416.0f), Sf(24.0f)), &sf_left, &br_err);
        } else if (!rel.html_url.empty()) {
            zones.push_back({{S(22), S(264), S(220), S(288)}, Btn::ReleaseLink});
            Gdiplus::SolidBrush br_lnk(ToGdip(hovered == Btn::ReleaseLink ? kColAccentHov : kColAccent));
            g.DrawString(L"View release notes on GitHub", -1, &f_sub,
                         Gdiplus::RectF(Sf(22.0f), Sf(264.0f), Sf(220.0f), Sf(24.0f)), &sf_left, &br_lnk);
        }

        // Bottom action buttons (y=304..338)
        auto draw_btn = [&](int x, int y, int w, int h, const wchar_t* txt, Btn b, bool primary) {
            zones.push_back({{S(x), S(y), S(x + w), S(y + h)}, b});
            bool hov = (hovered == b);
            bool prs = (pressed == b);
            COLORREF bg = primary ? (prs ? RGB(214, 214, 218) : (hov ? RGB(252, 252, 253) : RGB(237, 237, 239)))
                                  : (prs ? kColRaisedPress : (hov ? kColRaisedHov : kColRaised));
            COLORREF fg = primary ? RGB(17, 17, 19) : kColText;
            Gdiplus::SolidBrush br(ToGdip(bg));
            FillRoundRect(g, Sf(static_cast<float>(x)), Sf(static_cast<float>(y)),
                          Sf(static_cast<float>(w)), Sf(static_cast<float>(h)), Sf(8.0f), br);
            if (!primary) {
                Gdiplus::Pen bd(Gdiplus::Color(hov ? 38 : 22, 255, 255, 255), 1.0f);
                DrawRoundRect(g, Sf(static_cast<float>(x)), Sf(static_cast<float>(y)),
                              Sf(static_cast<float>(w)), Sf(static_cast<float>(h)), Sf(8.0f), bd);
            }
            Gdiplus::SolidBrush tb(ToGdip(fg));
            g.DrawString(txt, -1, primary ? &f_semi : &f_body,
                         Gdiplus::RectF(Sf(static_cast<float>(x)), Sf(static_cast<float>(y)),
                                        Sf(static_cast<float>(w)), Sf(static_cast<float>(h))),
                         &sf_center, &tb);
        };

        if (phase == Phase::Downloading || phase == Phase::Extracting) {
            draw_btn(342, 304, 96, 34, L"Cancel", Btn::Cancel, false);
        } else {
            draw_btn(22, 304, 106, 34, L"Skip version", Btn::Skip, false);
            draw_btn(232, 304, 78, 34, L"Later", Btn::Later, false);
            draw_btn(318, 304, 120, 34, phase == Phase::Error ? L"Retry" : L"Update now", Btn::UpdateNow, true);
        }

        BitBlt(hdc, 0, 0, cw, ch, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old);
        DeleteObject(mbm);
        DeleteDC(mdc);
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* st = reinterpret_cast<UpdatePopupState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            st = static_cast<UpdatePopupState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            st->hwnd = h;
            return TRUE;
        }
        if (!st) return DefWindowProcW(h, msg, wp, lp);

        switch (msg) {
            case WM_CREATE: {
                BOOL dark = TRUE;
                DwmSetWindowAttribute(h, 20, &dark, sizeof(dark));
                COLORREF cap = kColBg;
                DwmSetWindowAttribute(h, 35, &cap, sizeof(cap));
                if (!st->screenshot_out.empty()) {
                    SetTimer(h, 1, 180, nullptr);
                } else if (st->auto_start) {
                    st->StartDownload();
                }
                return 0;
            }
            case WM_TIMER:
                if (wp == 1) {
                    KillTimer(h, 1);
                    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                    CaptureWindowToPng(h, st->screenshot_out);
                    st->closed = true;
                }
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(h, &ps);
                st->Paint(hdc);
                EndPaint(h, &ps);
                return 0;
            }
            case WM_APP_UPDATE_PROGRESS: {
                auto* txt = reinterpret_cast<std::wstring*>(lp);
                if (wp >= 101) {
                    st->phase = Phase::Extracting;
                    st->progress_pct = 100;
                } else {
                    st->phase = Phase::Downloading;
                    st->progress_pct = static_cast<int>(wp);
                }
                if (txt) { st->status_text = *txt; delete txt; }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            case WM_APP_UPDATE_DONE: {
                auto* txt = reinterpret_cast<std::wstring*>(lp);
                std::wstring m = txt ? *txt : L"";
                delete txt;
                if (wp != 0) {
                    st->phase = Phase::Done;
                    st->result = UpdatePopupResult::ApplyingUpdate;
                    st->closed = true;
                } else {
                    st->phase = Phase::Error;
                    st->status_text = m.empty() ? L"Update failed." : m;
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            case WM_MOUSEMOVE: {
                Btn nb = st->HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (nb != st->hovered) {
                    st->hovered = nb;
                    InvalidateRect(h, nullptr, FALSE);
                }
                SetCursor(LoadCursorW(nullptr, nb != Btn::None ? IDC_HAND : IDC_ARROW));
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                return 0;
            }
            case WM_MOUSELEAVE:
                if (st->hovered != Btn::None) { st->hovered = Btn::None; InvalidateRect(h, nullptr, FALSE); }
                return 0;
            case WM_LBUTTONDOWN:
                st->pressed = st->HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (st->pressed != Btn::None) { SetCapture(h); InvalidateRect(h, nullptr, FALSE); }
                return 0;
            case WM_LBUTTONUP: {
                Btn pb = st->pressed;
                st->pressed = Btn::None;
                if (GetCapture() == h) ReleaseCapture();
                Btn hb = st->HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (pb != Btn::None && pb == hb) {
                    if (pb == Btn::UpdateNow) {
                        st->StartDownload();
                    } else if (pb == Btn::Later) {
                        st->result = UpdatePopupResult::Later;
                        st->closed = true;
                    } else if (pb == Btn::Skip) {
                        st->result = UpdatePopupResult::SkipVersion;
                        st->closed = true;
                    } else if (pb == Btn::Cancel) {
                        st->cancel_flag.store(true, std::memory_order_relaxed);
                        st->status_text = L"Cancelling...";
                        InvalidateRect(h, nullptr, FALSE);
                    } else if (pb == Btn::ReleaseLink && !st->rel.html_url.empty()) {
                        ShellExecuteW(h, L"open", Utf8ToWide(st->rel.html_url).c_str(),
                                      nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE && st->phase != Phase::Downloading && st->phase != Phase::Extracting) {
                    st->result = UpdatePopupResult::Later;
                    st->closed = true;
                }
                return 0;
            case WM_CLOSE:
                if (st->phase == Phase::Downloading) {
                    st->cancel_flag.store(true, std::memory_order_relaxed);
                }
                st->closed = true;
                return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};

UpdatePopupResult ShowUpdatePopup(HWND owner, HINSTANCE hinst, UINT dpi,
                                  const ReleaseInfo& rel, bool auto_start,
                                  const std::filesystem::path& screenshot_out = {}) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = UpdatePopupState::Proc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"RR3UpdatePopupWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    UpdatePopupState st;
    st.owner = owner;
    st.hinst = hinst;
    st.dpi = dpi;
    st.rel = rel;
    st.auto_start = auto_start;
    st.screenshot_out = screenshot_out;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT rc{0, 0, st.S(460), st.S(356)};
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    RECT orc{};
    GetWindowRect(owner, &orc);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    EnableWindow(owner, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"RR3UpdatePopupWnd", L"Real Racing 3 PC — Update",
                               style, x, y, w, h, owner, nullptr, hinst, &st);
    if (!dlg) {
        EnableWindow(owner, TRUE);
        return UpdatePopupResult::Later;
    }
    ShowWindow(dlg, SW_SHOWNORMAL);
    UpdateWindow(dlg);

    MSG msg{};
    while (!st.closed && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    st.alive->store(false, std::memory_order_release);
    st.cancel_flag.store(true, std::memory_order_relaxed);
    if (st.worker.joinable()) st.worker.join();
    DestroyWindow(dlg);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return st.result;
}

class ConfiguratorApp {
public:
    HWND hwnd = nullptr;
    HINSTANCE hinst = nullptr;
    UINT dpi = 96;
    bool reduced_motion = false;

    std::filesystem::path config_path;
    PcConfig cfg;
    GameDataStatus data_status;

    int active_tab = 0; // 0=Display, 1=Graphics, 2=Controls, 3=Files & saves
    int res_idx = 0;
    int fps_idx = 0;
    int scale_idx = 1;
    int aniso_idx = 3;

    int listening_key_slot = -1;
    HitKind hover_kind = HitKind::None;
    int hover_index = -1;
    HitKind pressed_kind = HitKind::None;
    int pressed_index = -1;
    std::vector<HitZone> hit_zones;

    std::wstring status_toast;
    COLORREF status_toast_col = kColSecondary;
    bool dirty = false;

    // Smooth interruptible springs (zero idle CPU; timer stops when settled)
    SpringState sp_nav_y;
    SpringState sp_winmode;
    SpringState sp_quality;
    SpringState sp_vsync;
    SpringState sp_menu_fps;
    SpringState sp_verbose;
    SpringState sp_online_mode;
    SpringState sp_auto_update;
    SpringState sp_press;
    bool anim_timer_on = false;
    LARGE_INTEGER qpc_freq{};
    LARGE_INTEGER qpc_last{};

    // Background update state
    ReleaseInfo latest_release;
    bool update_checking = false;
    bool update_restarting = false;
    std::wstring update_status_line;
    std::string effective_version = kPortVersion;
    bool disable_update_check = false;
    std::filesystem::path update_popup_screenshot;
    std::shared_ptr<std::atomic<bool>> alive_flag = std::make_shared<std::atomic<bool>>(true);

    HICON h_icon_large = nullptr;
    HICON h_icon_small = nullptr;
    HFONT h_ui_font = nullptr;
    HFONT h_mono_font = nullptr;
    HBRUSH h_br_input = nullptr;

    // Borderless native edits embedded inside anti-aliased GDI+ rounded shells
    HWND ed_custom_w = nullptr;
    HWND ed_custom_h = nullptr;
    HWND ed_ramp = nullptr;
    HWND ed_return = nullptr;
    HWND ed_apk = nullptr;
    HWND ed_data = nullptr;

    std::filesystem::path screenshot_out;
    bool launch_requested = false;

    int S(int px) const { return MulDiv(px, static_cast<int>(dpi), 96); }
    float Sf(float px) const { return px * static_cast<float>(dpi) / 96.0f; }

    void StartAnimTimer() {
        if (reduced_motion) {
            SnapSprings();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (!anim_timer_on && hwnd) {
            QueryPerformanceCounter(&qpc_last);
            SetTimer(hwnd, 2, 12, nullptr);
            anim_timer_on = true;
        }
    }

    void SnapSprings() {
        sp_nav_y.Snap(static_cast<float>(active_tab));
        sp_winmode.Snap(static_cast<float>(static_cast<int>(cfg.window_mode)));
        sp_quality.Snap(static_cast<float>(static_cast<int>(cfg.quality_preset)));
        sp_vsync.Snap(cfg.vsync ? 1.0f : 0.0f);
        sp_menu_fps.Snap(cfg.unlock_menu_fps ? 1.0f : 0.0f);
        sp_verbose.Snap(cfg.verbose_logs ? 1.0f : 0.0f);
        sp_online_mode.Snap(cfg.online_mode ? 1.0f : 0.0f);
        sp_auto_update.Snap(cfg.auto_install_updates ? 1.0f : 0.0f);
        sp_press.Snap(1.0f);
    }

    void StepAnimations() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - qpc_last.QuadPart) / static_cast<float>(qpc_freq.QuadPart);
        qpc_last = now;
        dt = std::clamp(dt, 0.001f, 0.05f);

        bool moving = false;
        moving |= sp_nav_y.Step(dt, 0.22f);
        moving |= sp_winmode.Step(dt, 0.22f);
        moving |= sp_quality.Step(dt, 0.22f);
        moving |= sp_vsync.Step(dt, 0.18f);
        moving |= sp_menu_fps.Step(dt, 0.18f);
        moving |= sp_verbose.Step(dt, 0.18f);
        moving |= sp_online_mode.Step(dt, 0.18f);
        moving |= sp_auto_update.Step(dt, 0.18f);
        moving |= sp_press.Step(dt, 0.14f);

        InvalidateRect(hwnd, nullptr, FALSE);
        if (!moving && anim_timer_on) {
            KillTimer(hwnd, 2);
            anim_timer_on = false;
        }
    }

    void StartUpdateCheckAsync(bool manual) {
        if (update_checking || !hwnd) return;
        update_checking = true;
        update_status_line = L"v" + Utf8ToWide(effective_version) + L" · Checking...";
        if (manual) SetToast(L"Checking for updates...");
        InvalidateRect(hwnd, nullptr, FALSE);

        HWND target = hwnd;
        auto alive = alive_flag;
        std::string ver = effective_version;
        std::thread([target, alive, ver, manual]() {
            ReleaseInfo info = CheckForUpdates(ver, kGithubRepoSlug);
            if (!alive->load(std::memory_order_acquire)) return;
            auto* heap_info = new ReleaseInfo(std::move(info));
            if (!PostMessageW(target, WM_APP_UPDATE_CHECK_DONE, manual ? 1 : 0,
                              reinterpret_cast<LPARAM>(heap_info))) {
                delete heap_info;
            }
        }).detach();
    }

    void OpenUpdateDialog(bool auto_start) {
        if (!latest_release.update_available) return;
        UpdatePopupResult res = ShowUpdatePopup(hwnd, hinst, dpi, latest_release,
                                                auto_start, update_popup_screenshot);
        if (!update_popup_screenshot.empty()) {
            DestroyWindow(hwnd);
            return;
        }
        if (res == UpdatePopupResult::SkipVersion) {
            cfg.skipped_update_version = latest_release.latest_version;
            SaveConfig(config_path, cfg);
            SetToast(L"Skipped v" + Utf8ToWide(latest_release.latest_version));
        } else if (res == UpdatePopupResult::ApplyingUpdate) {
            update_restarting = true;
            launch_requested = false;
            DestroyWindow(hwnd);
        }
    }

    void SetToast(const std::wstring& msg, COLORREF col = kColSecondary) {
        status_toast = msg;
        status_toast_col = col;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void CreateControls() {
        QueryPerformanceFrequency(&qpc_freq);
        BOOL client_anim = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &client_anim, 0)) {
            reduced_motion = !client_anim;
        }

        LOGFONTW lf{};
        lf.lfHeight = -S(13);
        lf.lfWeight = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        h_ui_font = CreateFontIndirectW(&lf);

        LOGFONTW mf = lf;
        wcscpy_s(mf.lfFaceName, L"Consolas");
        h_mono_font = CreateFontIndirectW(&mf);

        h_br_input = CreateSolidBrush(kColRaised);

        auto mk_edit = [&](int id, int x, int y, int w, int h, HFONT fnt, DWORD extra = 0) {
            HWND e = CreateWindowExW(0, WC_EDITW, L"",
                                     WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP | extra,
                                     S(x), S(y), S(w), S(h), hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hinst, nullptr);
            SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(fnt), TRUE);
            return e;
        };

        // Tab 0 custom resolution inputs (borderless inside GDI+ rounded boxes)
        ed_custom_w = mk_edit(IDC_EDIT_CUSTOM_W, 608, 138, 52, 18, h_mono_font, ES_NUMBER | ES_CENTER);
        ed_custom_h = mk_edit(IDC_EDIT_CUSTOM_H, 682, 138, 52, 18, h_mono_font, ES_NUMBER | ES_CENTER);

        // Tab 2 steering rates (aligned cleanly to the two keybind columns)
        ed_ramp   = mk_edit(IDC_EDIT_RAMP,   440, 108, 50, 18, h_mono_font, ES_CENTER);
        ed_return = mk_edit(IDC_EDIT_RETURN, 700, 108, 50, 18, h_mono_font, ES_CENTER);

        // Tab 3 game paths
        ed_apk  = mk_edit(IDC_EDIT_APK,  358, 134, 232, 18, h_ui_font);
        ed_data = mk_edit(IDC_EDIT_DATA, 358, 186, 312, 18, h_ui_font);

        SyncIndicesFromConfig();
        PopulateEditsFromConfig();
        SnapSprings();
        UpdateTabVisibility();
    }

    void SyncIndicesFromConfig() {
        res_idx = static_cast<int>(std::size(kResChoices)) - 1;
        for (size_t i = 0; i + 1 < std::size(kResChoices); ++i) {
            if (cfg.width == kResChoices[i].w && cfg.height == kResChoices[i].h) {
                res_idx = static_cast<int>(i);
                break;
            }
        }
        fps_idx = 0;
        for (size_t i = 0; i < std::size(kFpsChoices); ++i) {
            if (cfg.fps_cap == kFpsChoices[i].fps) { fps_idx = static_cast<int>(i); break; }
        }
        scale_idx = 1;
        float best = 999.0f;
        for (size_t i = 0; i < std::size(kScaleChoices); ++i) {
            float d = std::fabs(cfg.render_scale - kScaleChoices[i].scale);
            if (d < best) { best = d; scale_idx = static_cast<int>(i); }
        }
        aniso_idx = 3;
        for (size_t i = 0; i < std::size(kAnisoChoices); ++i) {
            if (cfg.anisotropy <= kAnisoChoices[i].aniso) { aniso_idx = static_cast<int>(i); break; }
        }
    }

    void PopulateEditsFromConfig() {
        SetWindowTextW(ed_custom_w, std::to_wstring(cfg.width > 0 ? cfg.width : 1280).c_str());
        SetWindowTextW(ed_custom_h, std::to_wstring(cfg.height > 0 ? cfg.height : 720).c_str());
        wchar_t buf[32]{};
        swprintf_s(buf, L"%.1f", cfg.steer_ramp_rate);
        SetWindowTextW(ed_ramp, buf);
        swprintf_s(buf, L"%.1f", cfg.steer_return_rate);
        SetWindowTextW(ed_return, buf);
        SetWindowTextW(ed_apk, Utf8ToWide(cfg.apk_path).c_str());
        SetWindowTextW(ed_data, Utf8ToWide(cfg.readonly_files_dir).c_str());
        data_status = CheckGameDataStatus(cfg);
    }

    bool IsCustomRes() const {
        return res_idx == static_cast<int>(std::size(kResChoices)) - 1;
    }

    void ReadEditsToConfig() {
        auto read_str = [](HWND h) -> std::string {
            int len = GetWindowTextLengthW(h);
            if (len <= 0) return {};
            std::wstring w(static_cast<size_t>(len) + 1, L'\0');
            GetWindowTextW(h, w.data(), len + 1);
            w.resize(static_cast<size_t>(len));
            return WideToUtf8(w);
        };

        if (!IsCustomRes()) {
            cfg.width = kResChoices[res_idx].w;
            cfg.height = kResChoices[res_idx].h;
        } else {
            try {
                int w = std::stoi(read_str(ed_custom_w));
                int h = std::stoi(read_str(ed_custom_h));
                cfg.width = (w <= 0) ? 0 : std::max(640, w);
                cfg.height = (h <= 0) ? 0 : std::max(360, h);
            } catch (...) {}
        }
        cfg.fps_cap = kFpsChoices[fps_idx].fps;
        cfg.render_scale = kScaleChoices[scale_idx].scale;
        cfg.anisotropy = kAnisoChoices[aniso_idx].aniso;

        try { cfg.steer_ramp_rate = std::clamp(std::stof(read_str(ed_ramp)), 0.5f, 20.0f); } catch (...) {}
        try { cfg.steer_return_rate = std::clamp(std::stof(read_str(ed_return)), 0.5f, 30.0f); } catch (...) {}

        cfg.apk_path = read_str(ed_apk);
        std::string dd = read_str(ed_data);
        if (!dd.empty()) cfg.readonly_files_dir = dd;

        cfg.key_back = cfg.key_pause;
        cfg.high_quality_preset = (cfg.quality_preset == QualityPreset::Ultra);
        cfg.fullscreen = (cfg.window_mode == WindowMode::Fullscreen);
        cfg.borderless = (cfg.window_mode == WindowMode::Borderless);
        ApplyPortableDefaultPaths(cfg);
    }

    void UpdateTabVisibility() {
        auto show = [](HWND h, bool v) { ShowWindow(h, v ? SW_SHOW : SW_HIDE); };
        show(ed_custom_w, active_tab == 0 && IsCustomRes());
        show(ed_custom_h, active_tab == 0 && IsCustomRes());
        show(ed_ramp,     active_tab == 2);
        show(ed_return,   active_tab == 2);
        show(ed_apk,      active_tab == 3);
        show(ed_data,     active_tab == 3);
        listening_key_slot = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void AddZone(int x, int y, int w, int h, HitKind kind, int idx = 0) {
        HitZone z;
        z.rc = {S(x), S(y), S(x + w), S(y + h)};
        z.kind = kind;
        z.index = idx;
        hit_zones.push_back(z);
    }

    RECT FindZoneRect(HitKind kind, int idx = 0) const {
        for (const auto& z : hit_zones) {
            if (z.kind == kind && z.index == idx) return z.rc;
        }
        return {};
    }

    bool IsHovered(HitKind k, int idx = 0) const { return hover_kind == k && hover_index == idx; }
    bool IsPressed(HitKind k, int idx = 0) const { return pressed_kind == k && pressed_index == idx; }

    void DrawInputShell(Gdiplus::Graphics& g, int x, int y, int w, int h, bool focused = false) {
        Gdiplus::SolidBrush br(ToGdip(kColRaised));
        Gdiplus::Pen bd(focused ? ToGdip(kColAccent, 200) : Gdiplus::Color(24, 255, 255, 255), 1.0f);
        FillRoundRect(g, Sf(static_cast<float>(x)), Sf(static_cast<float>(y)),
                      Sf(static_cast<float>(w)), Sf(static_cast<float>(h)), Sf(8.0f), br);
        DrawRoundRect(g, Sf(static_cast<float>(x)), Sf(static_cast<float>(y)),
                      Sf(static_cast<float>(w)), Sf(static_cast<float>(h)), Sf(8.0f), bd);
    }

    void DrawButton(Gdiplus::Graphics& g, const Gdiplus::Font& font,
                    int x, int y, int w, int h, const wchar_t* text,
                    HitKind kind, int idx = 0, bool primary = false, bool highlight_amber = false) {
        AddZone(x, y, w, h, kind, idx);
        bool hov = IsHovered(kind, idx);
        bool prs = IsPressed(kind, idx);

        float scale = prs ? sp_press.val : 1.0f;
        float fx = Sf(static_cast<float>(x));
        float fy = Sf(static_cast<float>(y));
        float fw = Sf(static_cast<float>(w));
        float fh = Sf(static_cast<float>(h));

        Gdiplus::GraphicsState gs = g.Save();
        if (std::fabs(scale - 1.0f) > 0.001f) {
            float cx = fx + fw * 0.5f;
            float cy = fy + fh * 0.5f;
            g.TranslateTransform(cx, cy);
            g.ScaleTransform(scale, scale);
            g.TranslateTransform(-cx, -cy);
        }

        COLORREF bg = primary ? (prs ? RGB(214, 214, 218) : (hov ? RGB(252, 252, 253) : RGB(237, 237, 239)))
                              : (highlight_amber ? RGB(54, 40, 18)
                                                 : (prs ? kColRaisedPress : (hov ? kColRaisedHov : kColRaised)));
        COLORREF fg = primary ? RGB(17, 17, 19) : (highlight_amber ? RGB(248, 215, 134) : kColText);

        Gdiplus::SolidBrush br_bg(ToGdip(bg));
        FillRoundRect(g, fx, fy, fw, fh, Sf(8.0f), br_bg);

        if (!primary) {
            Gdiplus::Pen bd(highlight_amber ? ToGdip(kColAmber, 180)
                                            : Gdiplus::Color(hov ? 36 : 22, 255, 255, 255), 1.0f);
            DrawRoundRect(g, fx, fy, fw, fh, Sf(8.0f), bd);
            // Subtle top inner light rim for tactile depth (unslop dark button token)
            Gdiplus::Pen rim(Gdiplus::Color(24, 255, 255, 255), 1.0f);
            g.DrawLine(&rim, fx + Sf(8.0f), fy + 1.0f, fx + fw - Sf(8.0f), fy + 1.0f);
        }

        Gdiplus::SolidBrush br_txt(ToGdip(fg));
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(text, -1, &font, Gdiplus::RectF(fx, fy, fw, fh), &sf, &br_txt);
        g.Restore(gs);
    }

    // Physical keycap button for Controls tab (signature detail)
    void DrawKeycapButton(Gdiplus::Graphics& g, const Gdiplus::Font& font,
                          int x, int y, int w, int h, const wchar_t* text,
                          HitKind kind, int idx, bool listening) {
        AddZone(x, y, w, h, kind, idx);
        bool hov = IsHovered(kind, idx);
        bool prs = IsPressed(kind, idx);

        float fx = Sf(static_cast<float>(x));
        float fy = Sf(static_cast<float>(y)) + (prs ? Sf(1.0f) : 0.0f);
        float fw = Sf(static_cast<float>(w));
        float fh = Sf(static_cast<float>(h));

        // Subtle keycap bottom edge (2px lip)
        if (!prs) {
            Gdiplus::SolidBrush lip(Gdiplus::Color(255, 11, 11, 13));
            FillRoundRect(g, fx, fy + Sf(1.5f), fw, fh, Sf(6.0f), lip);
        }

        COLORREF bg = listening ? RGB(52, 38, 17) : (hov ? kColRaisedHov : kColRaised);
        Gdiplus::SolidBrush br(ToGdip(bg));
        FillRoundRect(g, fx, fy, fw, fh, Sf(6.0f), br);

        Gdiplus::Pen bd(listening ? ToGdip(kColAmber, 190) : Gdiplus::Color(hov ? 44 : 28, 255, 255, 255), 1.0f);
        DrawRoundRect(g, fx, fy, fw, fh, Sf(6.0f), bd);

        Gdiplus::Pen top(Gdiplus::Color(28, 255, 255, 255), 1.0f);
        g.DrawLine(&top, fx + Sf(6.0f), fy + 1.0f, fx + fw - Sf(6.0f), fy + 1.0f);

        Gdiplus::SolidBrush br_t(ToGdip(listening ? RGB(248, 215, 134) : kColText));
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(text, -1, &font, Gdiplus::RectF(fx, fy, fw, fh), &sf, &br_t);
    }

    void DrawDropdownButton(Gdiplus::Graphics& g, const Gdiplus::Font& font,
                            int x, int y, int w, int h, const wchar_t* text, HitKind kind) {
        AddZone(x, y, w, h, kind, 0);
        bool hov = IsHovered(kind, 0);
        bool prs = IsPressed(kind, 0);

        float fx = Sf(static_cast<float>(x));
        float fy = Sf(static_cast<float>(y));
        float fw = Sf(static_cast<float>(w));
        float fh = Sf(static_cast<float>(h));

        Gdiplus::SolidBrush br(ToGdip(prs ? kColRaisedPress : (hov ? kColRaisedHov : kColRaised)));
        Gdiplus::Pen bd(Gdiplus::Color(hov ? 38 : 22, 255, 255, 255), 1.0f);
        FillRoundRect(g, fx, fy, fw, fh, Sf(8.0f), br);
        DrawRoundRect(g, fx, fy, fw, fh, Sf(8.0f), bd);

        Gdiplus::SolidBrush br_t(ToGdip(kColText));
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentNear);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(text, -1, &font, Gdiplus::RectF(fx + Sf(12.0f), fy, fw - Sf(32.0f), fh), &sf, &br_t);

        // Crisp 1.5px chevron
        float cx = fx + fw - Sf(16.0f);
        float cy = fy + fh * 0.5f;
        float sc = static_cast<float>(dpi) / 96.0f;
        Gdiplus::Pen ch(ToGdip(kColMuted), 1.5f * sc);
        ch.SetStartCap(Gdiplus::LineCapRound);
        ch.SetEndCap(Gdiplus::LineCapRound);
        ch.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawLine(&ch, cx - 3.5f * sc, cy - 1.5f * sc, cx, cy + 2.0f * sc);
        g.DrawLine(&ch, cx, cy + 2.0f * sc, cx + 3.5f * sc, cy - 1.5f * sc);
    }

    void DrawSegmented(Gdiplus::Graphics& g, const Gdiplus::Font& font,
                       int x, int y, int w, int h,
                       const std::vector<const wchar_t*>& labels,
                       int selected, float animated_pos, HitKind kind) {
        float fx = Sf(static_cast<float>(x));
        float fy = Sf(static_cast<float>(y));
        float fw = Sf(static_cast<float>(w));
        float fh = Sf(static_cast<float>(h));

        Gdiplus::SolidBrush track(ToGdip(RGB(17, 17, 19)));
        Gdiplus::Pen bd(Gdiplus::Color(20, 255, 255, 255), 1.0f);
        FillRoundRect(g, fx, fy, fw, fh, Sf(8.0f), track);
        DrawRoundRect(g, fx, fy, fw, fh, Sf(8.0f), bd);

        int n = static_cast<int>(labels.size());
        float pad = Sf(3.0f);
        float seg_w = (fw - pad * 2.0f) / static_cast<float>(n);

        // Spring-sliding elevated graphite pill (nested radius 8 - 3 = 5)
        float pill_x = fx + pad + animated_pos * seg_w;
        Gdiplus::SolidBrush pill_bg(ToGdip(kColSegActive));
        Gdiplus::Pen pill_bd(Gdiplus::Color(30, 255, 255, 255), 1.0f);
        FillRoundRect(g, pill_x, fy + pad, seg_w, fh - pad * 2.0f, Sf(5.5f), pill_bg);
        DrawRoundRect(g, pill_x, fy + pad, seg_w, fh - pad * 2.0f, Sf(5.5f), pill_bd);
        Gdiplus::Pen top_hl(Gdiplus::Color(30, 255, 255, 255), 1.0f);
        g.DrawLine(&top_hl, pill_x + Sf(5.0f), fy + pad + 1.0f, pill_x + seg_w - Sf(5.0f), fy + pad + 1.0f);

        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        for (int i = 0; i < n; ++i) {
            int zx = x + static_cast<int>(std::round(3.0f + i * (w - 6.0f) / n));
            int zw = static_cast<int>(std::round((w - 6.0f) / n));
            AddZone(zx, y + 2, zw, h - 4, kind, i);

            bool sel = (i == selected);
            bool hov = IsHovered(kind, i);
            Gdiplus::SolidBrush txt(ToGdip(sel ? kColText : (hov ? RGB(208, 208, 214) : kColMuted)));
            Gdiplus::RectF r(fx + pad + i * seg_w, fy, seg_w, fh);
            g.DrawString(labels[i], -1, &font, r, &sf, &txt);
        }
    }

    void DrawToggleSwitch(Gdiplus::Graphics& g, int x, int y, float t, HitKind kind) {
        const int w = 44;
        const int h = 24;
        AddZone(x - 4, y - 4, w + 8, h + 8, kind, 0);

        float fx = Sf(static_cast<float>(x));
        float fy = Sf(static_cast<float>(y));
        float fw = Sf(static_cast<float>(w));
        float fh = Sf(static_cast<float>(h));

        bool hov = IsHovered(kind, 0);
        COLORREF off_col = hov ? RGB(64, 64, 71) : RGB(52, 52, 58);
        COLORREF on_col  = hov ? kColAccentHov : kColAccent;
        Gdiplus::SolidBrush track(BlendColor(off_col, on_col, t));
        FillRoundRect(g, fx, fy, fw, fh, fh * 0.5f, track);

        float pad = Sf(3.0f);
        float d = fh - pad * 2.0f;
        float kx = fx + pad + (fw - pad * 2.0f - d) * std::clamp(t, 0.0f, 1.0f);
        Gdiplus::SolidBrush knob(Gdiplus::Color(255, 245, 245, 247));
        g.FillEllipse(&knob, kx, fy + pad, d, d);
    }

    void DrawGroupedContainer(Gdiplus::Graphics& g, int x, int y, int w, int h,
                              const std::vector<int>& divider_ys = {}) {
        float fx = Sf(static_cast<float>(x));
        float fy = Sf(static_cast<float>(y));
        float fw = Sf(static_cast<float>(w));
        float fh = Sf(static_cast<float>(h));

        Gdiplus::SolidBrush bg(ToGdip(kColCard));
        Gdiplus::Pen bd(Gdiplus::Color(20, 255, 255, 255), 1.0f);
        FillRoundRect(g, fx, fy, fw, fh, Sf(12.0f), bg);
        DrawRoundRect(g, fx, fy, fw, fh, Sf(12.0f), bd);

        Gdiplus::Pen div(Gdiplus::Color(15, 255, 255, 255), 1.0f);
        for (int dy : divider_ys) {
            float y_line = Sf(static_cast<float>(dy));
            g.DrawLine(&div, fx + Sf(16.0f), y_line, fx + fw - Sf(16.0f), y_line);
        }
    }

    void DrawMiniKeycap(Gdiplus::Graphics& g, const Gdiplus::Font& f_mono, int x, int y, int w, const wchar_t* key) {
        float fx = Sf(static_cast<float>(x));
        float fy = Sf(static_cast<float>(y));
        float fw = Sf(static_cast<float>(w));
        float fh = Sf(22.0f);
        Gdiplus::SolidBrush bg(ToGdip(kColRaised));
        Gdiplus::Pen bd(Gdiplus::Color(28, 255, 255, 255), 1.0f);
        FillRoundRect(g, fx, fy, fw, fh, Sf(5.0f), bg);
        DrawRoundRect(g, fx, fy, fw, fh, Sf(5.0f), bd);
        Gdiplus::SolidBrush txt(ToGdip(kColSecondary));
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(key, -1, &f_mono, Gdiplus::RectF(fx, fy, fw, fh), &sf, &txt);
    }

    void Paint(HDC hdc) {
        RECT client{};
        GetClientRect(hwnd, &client);
        int cw = client.right - client.left;
        int ch = client.bottom - client.top;
        if (cw <= 0 || ch <= 0) return;

        HDC mem_dc = CreateCompatibleDC(hdc);
        HBITMAP mem_bm = CreateCompatibleBitmap(hdc, cw, ch);
        HGDIOBJ old_bm = SelectObject(mem_dc, mem_bm);

        hit_zones.clear();

        Gdiplus::Graphics g(mem_dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        // Clean type scale: Segoe UI (weights 400 & 600) + Consolas for figures/keycaps
        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::FontFamily ff_mono(L"Consolas");
        Gdiplus::Font f_page_title(&ff, Sf(20.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font f_brand(&ff, Sf(15.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font f_body(&ff, Sf(13.5f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font f_body_semi(&ff, Sf(13.5f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font f_sub(&ff, Sf(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font f_sub_semi(&ff, Sf(12.5f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font f_mono(&ff_mono, Sf(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font f_mono_semi(&ff_mono, Sf(12.5f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

        Gdiplus::SolidBrush br_text(ToGdip(kColText));
        Gdiplus::SolidBrush br_sec(ToGdip(kColSecondary));
        Gdiplus::SolidBrush br_mut(ToGdip(kColMuted));
        Gdiplus::StringFormat sf_left;
        sf_left.SetAlignment(Gdiplus::StringAlignmentNear);
        sf_left.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::StringFormat sf_right;
        sf_right.SetAlignment(Gdiplus::StringAlignmentFar);
        sf_right.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        auto draw_txt = [&](int x, int y, int w, int h, const wchar_t* s, const Gdiplus::Font& f,
                            const Gdiplus::Brush& b, const Gdiplus::StringFormat* fmt = nullptr) {
            g.DrawString(s, -1, &f,
                         Gdiplus::RectF(Sf(static_cast<float>(x)), Sf(static_cast<float>(y)),
                                        Sf(static_cast<float>(w)), Sf(static_cast<float>(h))),
                         fmt ? fmt : &sf_left, &b);
        };

        // ---- Main Background & Left Rail ----
        Gdiplus::SolidBrush br_bg(ToGdip(kColBg));
        g.FillRectangle(&br_bg, 0, 0, cw, ch);

        Gdiplus::SolidBrush br_rail(ToGdip(kColSidebar));
        g.FillRectangle(&br_rail, 0.0f, 0.0f, Sf(static_cast<float>(kSidebarW)), static_cast<float>(ch));
        Gdiplus::Pen rail_div(Gdiplus::Color(18, 255, 255, 255), 1.0f);
        g.DrawLine(&rail_div, Sf(static_cast<float>(kSidebarW)), 0.0f,
                   Sf(static_cast<float>(kSidebarW)), static_cast<float>(ch));

        // ---- Sidebar Brand ----
        if (h_icon_large) {
            DrawIconEx(mem_dc, S(20), S(20), h_icon_large, S(40), S(40), 0, nullptr, DI_NORMAL);
        }
        draw_txt(72, 20, 132, 22, L"Real Racing 3", f_brand, br_text);
        std::wstring ver_sub = L"v" + Utf8ToWide(effective_version) + L" · Win64";
        draw_txt(72, 42, 132, 18, ver_sub.c_str(), f_mono, br_mut);

        // ---- Sidebar Navigation (4 clean 36px rows, 4px gap) ----
        const wchar_t* nav_labels[] = {L"Display", L"Graphics", L"Controls", L"Files & saves"};
        const int nav_top = 88;
        const int nav_h = 36;
        const int nav_step = 40;

        // Animated active row pill
        {
            float ay = Sf(static_cast<float>(nav_top) + sp_nav_y.val * static_cast<float>(nav_step));
            Gdiplus::SolidBrush br_act(ToGdip(RGB(30, 30, 34)));
            FillRoundRect(g, Sf(12.0f), ay, Sf(static_cast<float>(kSidebarW - 24)), Sf(static_cast<float>(nav_h)),
                          Sf(8.0f), br_act);
            Gdiplus::Pen rim(Gdiplus::Color(22, 255, 255, 255), 1.0f);
            DrawRoundRect(g, Sf(12.0f), ay, Sf(static_cast<float>(kSidebarW - 24)), Sf(static_cast<float>(nav_h)),
                          Sf(8.0f), rim);
            // Subtle 3px cobalt selection indicator
            Gdiplus::SolidBrush ind(ToGdip(kColAccent));
            FillRoundRect(g, Sf(12.0f), ay + Sf(9.0f), Sf(3.0f), Sf(18.0f), Sf(1.5f), ind);
        }

        for (int i = 0; i < 4; ++i) {
            int ry = nav_top + i * nav_step;
            AddZone(12, ry, kSidebarW - 24, nav_h, HitKind::Nav, i);
            bool active = (active_tab == i);
            bool hov = IsHovered(HitKind::Nav, i);
            if (hov && !active) {
                Gdiplus::SolidBrush br_h(ToGdip(RGB(23, 23, 26)));
                FillRoundRect(g, Sf(12.0f), Sf(static_cast<float>(ry)),
                              Sf(static_cast<float>(kSidebarW - 24)), Sf(static_cast<float>(nav_h)),
                              Sf(8.0f), br_h);
            }
            COLORREF icon_col = active ? kColText : (hov ? kColSecondary : kColMuted);
            DrawNavIcon(g, i, Sf(32.0f), Sf(static_cast<float>(ry + nav_h / 2)),
                        static_cast<float>(dpi) / 96.0f, ToGdip(icon_col));
            Gdiplus::SolidBrush tbr(ToGdip(active ? kColText : (hov ? kColText : kColSecondary)));
            draw_txt(50, ry, kSidebarW - 68, nav_h, nav_labels[i], active ? f_body_semi : f_body, tbr);
        }

        // ---- Sidebar Bottom Action Rail ----
        {
            if (latest_release.update_available) {
                std::wstring up_lbl = L"Update v" + Utf8ToWide(latest_release.latest_version);
                DrawButton(g, f_sub_semi, 16, 388, kSidebarW - 32, 28, up_lbl.c_str(),
                           HitKind::BtnOpenUpdate, 0, false, true);
            }
            g.DrawLine(&rail_div, Sf(16.0f), Sf(428.0f), Sf(static_cast<float>(kSidebarW - 16)), Sf(428.0f));
            bool ready = data_status.has_libs && data_status.has_depot;
            Gdiplus::SolidBrush dot(ToGdip(ready ? kColGreen : kColAmber));
            g.FillEllipse(&dot, Sf(20.0f), Sf(447.0f), Sf(7.0f), Sf(7.0f));
            draw_txt(35, 440, 165, 22, ready ? L"Ready to launch" : L"Game files needed", f_sub, br_sec);

            DrawButton(g, f_body_semi, 16, 470, kSidebarW - 32, 38, L"Play", HitKind::BtnPlay, 0, true);
            DrawButton(g, f_sub, 16, 516, 88, 28, L"Save", HitKind::BtnSave);
            DrawButton(g, f_sub, 112, 516, 88, 28, L"Reset", HitKind::BtnReset);
        }

        // ---- Right Content Header ----
        draw_txt(kContentX, 22, 260, 28, nav_labels[active_tab], f_page_title, br_text);
        if (!status_toast.empty()) {
            Gdiplus::SolidBrush br_toast(ToGdip(status_toast_col));
            draw_txt(kContentX + 240, 24, kContentW - 240, 24, status_toast.c_str(), f_sub, br_toast, &sf_right);
        }

        // ---- Active Pane ----
        if (active_tab == 0) {
            // Group 1: Windowing (2 rows x 52px = 104px, y=68..172)
            DrawGroupedContainer(g, kContentX, 68, kContentW, 104, {120});
            draw_txt(kContentX + 16, 68, 160, 52, L"Window mode", f_body, br_text);
            DrawSegmented(g, f_sub_semi, kContentX + 216, 78, 296, 32,
                          {L"Windowed", L"Borderless", L"Fullscreen"},
                          static_cast<int>(cfg.window_mode), sp_winmode.val, HitKind::WindowMode);

            draw_txt(kContentX + 16, 120, 160, 52, L"Resolution", f_body, br_text);
            if (IsCustomRes()) {
                DrawDropdownButton(g, f_body, kContentX + 216, 130, 132, 32, kResChoices[res_idx].label, HitKind::DropRes);
                DrawInputShell(g, 602, 131, 64, 30);
                draw_txt(668, 131, 12, 30, L"x", f_sub, br_mut);
                DrawInputShell(g, 676, 131, 64, 30);
                draw_txt(744, 131, 20, 30, L"px", f_sub, br_mut);
            } else {
                DrawDropdownButton(g, f_body, kContentX + 296, 130, 216, 32, kResChoices[res_idx].label, HitKind::DropRes);
            }

            // Group 2: Frame pacing (3 rows x 52px = 156px, y=188..344)
            DrawGroupedContainer(g, kContentX, 188, kContentW, 156, {240, 292});
            draw_txt(kContentX + 16, 188, 200, 52, L"V-Sync", f_body, br_text);
            DrawToggleSwitch(g, kContentX + kContentW - 60, 202, sp_vsync.val, HitKind::ToggleVsync);

            draw_txt(kContentX + 16, 240, 200, 52, L"Frame rate cap", f_body, br_text);
            DrawDropdownButton(g, f_body, kContentX + 336, 250, 176, 32, kFpsChoices[fps_idx].label, HitKind::DropFps);

            draw_txt(kContentX + 16, 298, 240, 20, L"Menu frame rate", f_body, br_text);
            draw_txt(kContentX + 16, 318, 280, 18, L"Remove the 30 FPS menu limit", f_sub, br_mut);
            DrawToggleSwitch(g, kContentX + kContentW - 60, 306, sp_menu_fps.val, HitKind::ToggleMenuFps);

            // Quiet in-game shortcuts with crisp keycaps
            DrawMiniKeycap(g, f_mono, kContentX, 368, 34, L"F10");
            draw_txt(kContentX + 42, 368, 148, 22, L"Cycle window mode", f_sub, br_mut);
            DrawMiniKeycap(g, f_mono, kContentX + 206, 368, 34, L"F11");
            draw_txt(kContentX + 248, 368, 160, 22, L"Toggle fullscreen", f_sub, br_mut);
        } else if (active_tab == 1) {
            // Group 1: Rendering (3 rows x 52px = 156px, y=68..224)
            DrawGroupedContainer(g, kContentX, 68, kContentW, 156, {120, 172});
            draw_txt(kContentX + 16, 68, 150, 52, L"Quality", f_body, br_text);
            DrawSegmented(g, f_sub_semi, kContentX + 196, 78, 316, 32,
                          {L"Low", L"Medium", L"High", L"Ultra"},
                          static_cast<int>(cfg.quality_preset), sp_quality.val, HitKind::Quality);

            draw_txt(kContentX + 16, 120, 240, 52, L"Render scale", f_body, br_text);
            DrawDropdownButton(g, f_body, kContentX + 372, 130, 140, 32, kScaleChoices[scale_idx].label, HitKind::DropScale);

            draw_txt(kContentX + 16, 172, 240, 52, L"Anisotropic filtering", f_body, br_text);
            DrawDropdownButton(g, f_body, kContentX + 372, 182, 140, 32, kAnisoChoices[aniso_idx].label, HitKind::DropAniso);

            // Group 2: Verbose logs + Online mode (2 rows: y=240..356)
            DrawGroupedContainer(g, kContentX, 240, kContentW, 112, {292});
            draw_txt(kContentX + 16, 240, 240, 52, L"Verbose logs", f_body, br_text);
            DrawToggleSwitch(g, kContentX + kContentW - 60, 254, sp_verbose.val, HitKind::ToggleVerbose);

            draw_txt(kContentX + 16, 300, 260, 20, L"Online mode", f_body, br_text);
            draw_txt(kContentX + 16, 322, 430, 18, L"Allow internet access for private servers (Project Resurrection)", f_sub, br_mut);
            DrawToggleSwitch(g, kContentX + kContentW - 60, 310, sp_online_mode.val, HitKind::ToggleOnlineMode);
        } else if (active_tab == 2) {
            // Section: Keyboard
            draw_txt(kContentX, 64, 200, 20, L"Keyboard", f_sub_semi, br_sec);
            DrawButton(g, f_sub, kContentX + kContentW - 92, 58, 92, 26, L"Reset keys", HitKind::BtnResetKeys);

            // Steering tuning row (y=92..144)
            DrawGroupedContainer(g, kContentX, 92, kContentW, 52);
            draw_txt(kContentX + 16, 92, 168, 52, L"Steering speed", f_body, br_text);
            DrawInputShell(g, 434, 103, 62, 30);
            draw_txt(kContentX + 276, 92, 168, 52, L"Steering return", f_body, br_text);
            DrawInputShell(g, 694, 103, 62, 30);

            // Keybind 2-column grid (y=156..340, 4 rows x 46px)
            DrawGroupedContainer(g, kContentX, 156, kContentW, 184, {202, 248, 294});
            for (int i = 0; i < static_cast<int>(std::size(kKeySlots)); ++i) {
                int col = i / 4;
                int row = i % 4;
                int rx = (col == 0) ? (kContentX + 16) : (kContentX + 276);
                int kx = (col == 0) ? (kContentX + 128) : (kContentX + 388);
                int ry = 156 + row * 46;
                draw_txt(rx, ry, 108, 46, kKeySlots[i].label, f_body, br_text);
                bool waiting = (listening_key_slot == i);
                std::wstring key_txt = waiting ? L"Press key" : Utf8ToWide(cfg.*(kKeySlots[i].field));
                DrawKeycapButton(g, f_mono_semi, kx, ry + 9, 124, 28, key_txt.c_str(), HitKind::KeyBind, i, waiting);
            }

            // Section: Gamepad (no red controller pointer note as requested)
            draw_txt(kContentX, 360, 140, 20, L"Gamepad", f_sub_semi, br_sec);
            draw_txt(kContentX + 180, 360, kContentW - 180, 20, L"Xbox · PlayStation · Switch Pro", f_sub, br_mut, &sf_right);

            DrawGroupedContainer(g, kContentX, 386, kContentW, 120, {426, 466});
            auto draw_pad_cell = [&](int x, int y, const wchar_t* act, const wchar_t* bind) {
                draw_txt(x, y, 76, 40, act, f_sub, br_mut);
                draw_txt(x + 76, y, 174, 40, bind, f_sub, br_text);
            };
            draw_pad_cell(kContentX + 16,  386, L"Throttle",  L"A / Cross, RT / R2");
            draw_pad_cell(kContentX + 276, 386, L"Camera",    L"LB / L1, Y / Triangle");
            draw_pad_cell(kContentX + 16,  426, L"Brake",     L"X / Square, LT / L2");
            draw_pad_cell(kContentX + 276, 426, L"Look back", L"RB / R1, B / Circle");
            draw_pad_cell(kContentX + 16,  466, L"Steer",     L"Left stick, D-Pad");
            draw_pad_cell(kContentX + 276, 466, L"Pause",     L"Menu / Options");
        } else {
            // Pane 3: Files & saves
            Gdiplus::StringFormat sf_wrap;
            sf_wrap.SetAlignment(Gdiplus::StringAlignmentNear);
            sf_wrap.SetLineAlignment(Gdiplus::StringAlignmentNear);
            g.DrawString(L"Provide your own Android files. The APK supplies the game engine, and the data folder holds tracks, cars, and audio. Original files stay untouched.",
                         -1, &f_sub,
                         Gdiplus::RectF(Sf(static_cast<float>(kContentX)), Sf(62.0f),
                                        Sf(static_cast<float>(kContentW)), Sf(40.0f)),
                         &sf_wrap, &br_sec);

            // Group 1: Android APK + Data folder + verification row (y=114..274)
            DrawGroupedContainer(g, kContentX, 114, kContentW, 160, {168, 222});
            draw_txt(kContentX + 16, 114, 96, 54, L"Android APK", f_body, br_text);
            DrawInputShell(g, 352, 127, 244, 30);
            DrawButton(g, f_sub, 604, 127, 70, 30, L"Browse", HitKind::BtnBrowseApk);
            DrawButton(g, f_sub_semi, 682, 127, 74, 30, L"Extract", HitKind::BtnExtractApk);

            draw_txt(kContentX + 16, 168, 96, 54, L"Data folder", f_body, br_text);
            DrawInputShell(g, 352, 181, 324, 30);
            DrawButton(g, f_sub, 684, 181, 72, 30, L"Browse", HitKind::BtnBrowseData);

            bool ready = data_status.has_libs && data_status.has_depot;
            Gdiplus::SolidBrush dot(ToGdip(ready ? kColGreen : kColAmber));
            g.FillEllipse(&dot, Sf(static_cast<float>(kContentX + 18)), Sf(244.0f), Sf(7.0f), Sf(7.0f));
            std::wstring st_main = Utf8ToWide(data_status.summary);
            draw_txt(kContentX + 34, 226, kContentW - 50, 44, st_main.c_str(), f_sub, ready ? br_text : br_sec);

            // Section: Save data (y=298..452)
            draw_txt(kContentX, 296, 160, 20, L"Save data", f_sub_semi, br_sec);
            std::wstring cnt = std::to_wstring(data_status.save_file_count) +
                               (data_status.save_file_count == 1 ? L" file" : L" files");
            draw_txt(kContentX + 200, 296, kContentW - 200, 20, cnt.c_str(), f_mono, br_mut, &sf_right);

            DrawGroupedContainer(g, kContentX, 322, kContentW, 130, {386});
            std::wstring save_path = Utf8ToWide((std::filesystem::path(cfg.userdata_dir) / "doc").string());
            Gdiplus::StringFormat sf_path;
            sf_path.SetAlignment(Gdiplus::StringAlignmentNear);
            sf_path.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            sf_path.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            sf_path.SetTrimming(Gdiplus::StringTrimmingEllipsisPath);
            draw_txt(kContentX + 16, 330, kContentW - 32, 22, save_path.c_str(), f_mono, br_text, &sf_path);
            draw_txt(kContentX + 16, 354, kContentW - 32, 20,
                     L"Supports character.2.dat or .zip backups. Existing saves are backed up first.", f_sub, br_mut);

            DrawButton(g, f_sub_semi, kContentX + 16,  402, 160, 34, L"Import save", HitKind::BtnImportSave);
            DrawButton(g, f_sub_semi, kContentX + 184, 402, 160, 34, L"Export save", HitKind::BtnExportSave);
            DrawButton(g, f_sub,      kContentX + 352, 402, 160, 34, L"Open folder", HitKind::BtnOpenSaveDir);

            // Section: Port updates (y=470..532)
            DrawGroupedContainer(g, kContentX, 470, kContentW, 62);
            draw_txt(kContentX + 16, 479, 200, 20, L"Updates", f_body, br_text);
            std::wstring ust = update_status_line.empty()
                                   ? (L"v" + Utf8ToWide(effective_version))
                                   : update_status_line;
            Gdiplus::SolidBrush br_ust(ToGdip(latest_release.update_available ? kColAmber : kColMuted));
            draw_txt(kContentX + 16, 501, 224, 18, ust.c_str(), f_sub, br_ust);

            draw_txt(kContentX + 248, 470, 88, 62, L"Auto-update", f_sub, br_sec, &sf_right);
            DrawToggleSwitch(g, kContentX + 346, 489, sp_auto_update.val, HitKind::ToggleAutoUpdate);

            const wchar_t* chk_lbl = update_checking ? L"Checking..."
                                                     : (latest_release.update_available ? L"Update" : L"Check now");
            DrawButton(g, f_sub_semi, kContentX + 412, 485, 100, 32, chk_lbl,
                       HitKind::BtnCheckUpdate, 0, false, latest_release.update_available);
        }

        BitBlt(hdc, 0, 0, cw, ch, mem_dc, 0, 0, SRCCOPY);
        SelectObject(mem_dc, old_bm);
        DeleteObject(mem_bm);
        DeleteDC(mem_dc);
    }

    const HitZone* HitTest(int mx, int my) const {
        POINT pt{mx, my};
        for (const auto& z : hit_zones) {
            if (PtInRect(&z.rc, pt)) return &z;
        }
        return nullptr;
    }

    void HandleAction(HitKind kind, int idx) {
        if (listening_key_slot >= 0 && kind != HitKind::KeyBind) {
            listening_key_slot = -1;
        }
        switch (kind) {
            case HitKind::Nav:
                if (active_tab != idx) {
                    ReadEditsToConfig();
                    active_tab = idx;
                    sp_nav_y.target = static_cast<float>(active_tab);
                    UpdateTabVisibility();
                    StartAnimTimer();
                }
                break;
            case HitKind::WindowMode:
                cfg.window_mode = static_cast<WindowMode>(idx);
                sp_winmode.target = static_cast<float>(idx);
                dirty = true;
                SetToast(L"Unsaved changes");
                StartAnimTimer();
                break;
            case HitKind::Quality:
                cfg.quality_preset = static_cast<QualityPreset>(idx);
                sp_quality.target = static_cast<float>(idx);
                dirty = true;
                SetToast(L"Unsaved changes");
                StartAnimTimer();
                break;
            case HitKind::DropRes: {
                std::vector<std::wstring> items;
                for (const auto& r : kResChoices) items.emplace_back(r.label);
                int pick = ShowCustomDropdown(hwnd, hinst, dpi, FindZoneRect(kind), items, res_idx);
                if (pick >= 0 && pick != res_idx) {
                    res_idx = pick;
                    dirty = true;
                    UpdateTabVisibility();
                    SetToast(L"Unsaved changes");
                }
                break;
            }
            case HitKind::DropFps: {
                std::vector<std::wstring> items;
                for (const auto& f : kFpsChoices) items.emplace_back(f.label);
                int pick = ShowCustomDropdown(hwnd, hinst, dpi, FindZoneRect(kind), items, fps_idx);
                if (pick >= 0 && pick != fps_idx) {
                    fps_idx = pick;
                    cfg.fps_cap = kFpsChoices[fps_idx].fps;
                    dirty = true;
                    SetToast(L"Unsaved changes");
                }
                break;
            }
            case HitKind::DropScale: {
                std::vector<std::wstring> items;
                for (const auto& s : kScaleChoices) items.emplace_back(s.label);
                int pick = ShowCustomDropdown(hwnd, hinst, dpi, FindZoneRect(kind), items, scale_idx);
                if (pick >= 0 && pick != scale_idx) {
                    scale_idx = pick;
                    cfg.render_scale = kScaleChoices[scale_idx].scale;
                    dirty = true;
                    SetToast(L"Unsaved changes");
                }
                break;
            }
            case HitKind::DropAniso: {
                std::vector<std::wstring> items;
                for (const auto& a : kAnisoChoices) items.emplace_back(a.label);
                int pick = ShowCustomDropdown(hwnd, hinst, dpi, FindZoneRect(kind), items, aniso_idx);
                if (pick >= 0 && pick != aniso_idx) {
                    aniso_idx = pick;
                    cfg.anisotropy = kAnisoChoices[aniso_idx].aniso;
                    dirty = true;
                    SetToast(L"Unsaved changes");
                }
                break;
            }
            case HitKind::ToggleVsync:
                cfg.vsync = !cfg.vsync;
                sp_vsync.target = cfg.vsync ? 1.0f : 0.0f;
                dirty = true;
                SetToast(L"Unsaved changes");
                StartAnimTimer();
                break;
            case HitKind::ToggleMenuFps:
                cfg.unlock_menu_fps = !cfg.unlock_menu_fps;
                sp_menu_fps.target = cfg.unlock_menu_fps ? 1.0f : 0.0f;
                dirty = true;
                SetToast(L"Unsaved changes");
                StartAnimTimer();
                break;
            case HitKind::ToggleVerbose:
                cfg.verbose_logs = !cfg.verbose_logs;
                sp_verbose.target = cfg.verbose_logs ? 1.0f : 0.0f;
                dirty = true;
                SetToast(L"Unsaved changes");
                StartAnimTimer();
                break;
            case HitKind::ToggleOnlineMode:
                cfg.online_mode = !cfg.online_mode;
                sp_online_mode.target = cfg.online_mode ? 1.0f : 0.0f;
                dirty = true;
                SetToast(L"Unsaved changes");
                StartAnimTimer();
                break;
            case HitKind::KeyBind:
                listening_key_slot = (listening_key_slot == idx) ? -1 : idx;
                SetToast(listening_key_slot >= 0 ? L"Press a key to bind" : L"", kColAmber);
                SetFocus(hwnd);
                break;
            case HitKind::BtnResetKeys: {
                PcConfig def;
                cfg.steer_ramp_rate = def.steer_ramp_rate;
                cfg.steer_return_rate = def.steer_return_rate;
                cfg.key_throttle = def.key_throttle;
                cfg.key_brake = def.key_brake;
                cfg.key_left = def.key_left;
                cfg.key_right = def.key_right;
                cfg.key_handbrake = def.key_handbrake;
                cfg.key_camera = def.key_camera;
                cfg.key_rear_view = def.key_rear_view;
                cfg.key_confirm = def.key_confirm;
                cfg.key_back = def.key_back;
                cfg.key_pause = def.key_pause;
                PopulateEditsFromConfig();
                dirty = true;
                SetToast(L"Keys reset", kColGreen);
                break;
            }
            case HitKind::BtnBrowseApk: {
                std::filesystem::path p;
                if (PickFileDialog(hwnd, L"Select Real Racing 3 APK",
                                   L"Android APK (*.apk)\0*.apk\0All Files (*.*)\0*.*\0", p)) {
                    cfg.apk_path = p.string();
                    SetWindowTextW(ed_apk, p.c_str());
                    SetToast(L"APK selected");
                }
                break;
            }
            case HitKind::BtnExtractApk: {
                ReadEditsToConfig();
                if (cfg.apk_path.empty()) {
                    SetToast(L"Select a Real Racing 3 APK first", kColRed);
                    break;
                }
                SetToast(L"Extracting libraries...");
                UpdateWindow(hwnd);
                std::string st;
                bool ok = ExtractArm64LibsFromApk(cfg.apk_path, cfg.work_bins_dir, st);
                if (ok) {
                    std::string res_st;
                    if (EnsureApkResExtracted(cfg.apk_path, cfg.readonly_files_dir, cfg.userdata_dir, res_st)) {
                        st += "\n" + res_st;
                    }
                }
                data_status = CheckGameDataStatus(cfg);
                if (ok) {
                    if (data_status.is_patched_14_0_1 && !cfg.online_mode) {
                        cfg.online_mode = true;
                        sp_online_mode.target = 1.0f;
                        st += "\nOnline mode enabled automatically for patched build.";
                    }
                    SaveConfig(config_path, cfg);
                    SetToast(L"Libraries verified", kColGreen);
                    MessageBoxW(hwnd, Utf8ToWide(st).c_str(), L"Extraction complete", MB_OK | MB_ICONINFORMATION);
                } else {
                    SetToast(L"Extraction failed", kColRed);
                    MessageBoxW(hwnd, Utf8ToWide(st).c_str(), L"Extraction failed", MB_OK | MB_ICONERROR);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
            case HitKind::BtnBrowseData: {
                std::filesystem::path dir;
                if (PickFolderDialog(hwnd, L"Select com.ea.games.r3_row folder", dir)) {
                    std::filesystem::path norm;
                    std::string st;
                    if (NormalizeGameDataDir(dir, norm, st)) {
                        cfg.readonly_files_dir = norm.string();
                        SetWindowTextW(ed_data, norm.c_str());
                        data_status = CheckGameDataStatus(cfg);
                        SaveConfig(config_path, cfg);
                        SetToast(L"Data folder verified", kColGreen);
                    } else {
                        SetToast(L".depot folder not found", kColRed);
                        MessageBoxW(hwnd, Utf8ToWide(st).c_str(), L"Data folder", MB_OK | MB_ICONWARNING);
                    }
                }
                break;
            }
            case HitKind::BtnImportSave: {
                ReadEditsToConfig();
                std::vector<std::filesystem::path> files;
                if (PickSaveFilesDialog(hwnd, L"Import save file",
                                        L"RR3 Save Files (*.dat;*.zip)\0*.dat;*.zip\0All Files (*.*)\0*.*\0", files)) {
                    std::string st;
                    bool ok = ImportSaveFiles(files, cfg.userdata_dir, st);
                    data_status = CheckGameDataStatus(cfg);
                    SetToast(ok ? L"Save imported" : L"Import failed", ok ? kColGreen : kColRed);
                    MessageBoxW(hwnd, Utf8ToWide(st).c_str(), ok ? L"Import complete" : L"Import failed",
                                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
                }
                break;
            }
            case HitKind::BtnExportSave: {
                ReadEditsToConfig();
                std::filesystem::path dst;
                if (SaveFileDialog(hwnd, L"Export save file",
                                   L"RR3 Save File (*.dat)\0*.dat\0Full Save Backup (*.zip)\0*.zip\0",
                                   L"character.2.dat", dst)) {
                    std::string st;
                    bool ok = ExportSaveFile(cfg.userdata_dir, dst, st);
                    SetToast(ok ? L"Save exported" : L"Export failed", ok ? kColGreen : kColRed);
                    MessageBoxW(hwnd, Utf8ToWide(st).c_str(), ok ? L"Export complete" : L"Export failed",
                                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
                }
                break;
            }
            case HitKind::BtnOpenSaveDir: {
                ReadEditsToConfig();
                auto doc = std::filesystem::path(cfg.userdata_dir) / "doc";
                std::error_code ec;
                std::filesystem::create_directories(doc, ec);
                ShellExecuteW(hwnd, L"open", doc.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
            case HitKind::ToggleAutoUpdate:
                cfg.auto_install_updates = !cfg.auto_install_updates;
                sp_auto_update.target = cfg.auto_install_updates ? 1.0f : 0.0f;
                dirty = true;
                SaveConfig(config_path, cfg);
                SetToast(cfg.auto_install_updates ? L"Auto-update enabled" : L"Auto-update off", kColGreen);
                StartAnimTimer();
                break;
            case HitKind::BtnCheckUpdate:
                if (latest_release.update_available) {
                    OpenUpdateDialog(false);
                } else {
                    StartUpdateCheckAsync(true);
                }
                break;
            case HitKind::BtnOpenUpdate:
                OpenUpdateDialog(false);
                break;
            case HitKind::BtnReset: {
                std::string k_apk = cfg.apk_path, k_ro = cfg.readonly_files_dir;
                std::string k_ud = cfg.userdata_dir, k_wb = cfg.work_bins_dir;
                cfg = PcConfig{};
                cfg.apk_path = k_apk;
                cfg.readonly_files_dir = k_ro;
                cfg.userdata_dir = k_ud;
                cfg.work_bins_dir = k_wb;
                ApplyPortableDefaultPaths(cfg);
                SyncIndicesFromConfig();
                PopulateEditsFromConfig();
                sp_winmode.target = static_cast<float>(static_cast<int>(cfg.window_mode));
                sp_quality.target = static_cast<float>(static_cast<int>(cfg.quality_preset));
                sp_vsync.target = cfg.vsync ? 1.0f : 0.0f;
                sp_menu_fps.target = cfg.unlock_menu_fps ? 1.0f : 0.0f;
                sp_verbose.target = cfg.verbose_logs ? 1.0f : 0.0f;
                sp_online_mode.target = cfg.online_mode ? 1.0f : 0.0f;
                sp_auto_update.target = cfg.auto_install_updates ? 1.0f : 0.0f;
                dirty = true;
                UpdateTabVisibility();
                SetToast(L"Defaults restored", kColGreen);
                StartAnimTimer();
                break;
            }
            case HitKind::BtnSave: {
                ReadEditsToConfig();
                bool ok = SaveConfig(config_path, cfg);
                data_status = CheckGameDataStatus(cfg);
                dirty = !ok;
                SetToast(ok ? L"Saved" : L"Save failed", ok ? kColGreen : kColRed);
                break;
            }
            case HitKind::BtnPlay: {
                ReadEditsToConfig();
                SaveConfig(config_path, cfg);
                data_status = CheckGameDataStatus(cfg);
                if (!data_status.has_libs || !data_status.has_depot) {
                    active_tab = 3;
                    sp_nav_y.target = 3.0f;
                    UpdateTabVisibility();
                    StartAnimTimer();
                    SetToast(L"Add Android game files first", kColAmber);
                    break;
                }
                launch_requested = true;
                DestroyWindow(hwnd);
                break;
            }
            default: break;
        }
    }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        ConfiguratorApp* app = nullptr;
        if (msg == WM_NCCREATE) {
            app = static_cast<ConfiguratorApp*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->hwnd = hwnd;
        } else {
            app = reinterpret_cast<ConfiguratorApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return app ? app->WndProc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                BOOL dark = TRUE;
                DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
                COLORREF caption_col = kColSidebar;
                DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption_col, sizeof(caption_col));
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(h_icon_large));
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(h_icon_small));
                CreateControls();
                CleanupLeftoverUpdateFiles(GetExecutableDir());
                if (!screenshot_out.empty()) {
                    SetTimer(hwnd, 1, 200, nullptr);
                } else if (!disable_update_check && (cfg.auto_check_updates || !update_popup_screenshot.empty())) {
                    StartUpdateCheckAsync(!update_popup_screenshot.empty());
                }
                return 0;
            }
            case WM_APP_UPDATE_CHECK_DONE: {
                update_checking = false;
                bool manual = (wp != 0);
                auto* info = reinterpret_cast<ReleaseInfo*>(lp);
                if (info) {
                    latest_release = std::move(*info);
                    delete info;
                }
                if (!latest_release.ok) {
                    update_status_line = L"v" + Utf8ToWide(effective_version) + L" · Offline";
                    if (manual) SetToast(Utf8ToWide(latest_release.error), kColAmber);
                } else if (latest_release.update_available) {
                    update_status_line = L"v" + Utf8ToWide(latest_release.latest_version) + L" available";
                    SetToast(L"Update v" + Utf8ToWide(latest_release.latest_version) + L" available", kColAmber);
                    bool skipped = (!manual && !cfg.skipped_update_version.empty() &&
                                    latest_release.latest_version == cfg.skipped_update_version);
                    if (!skipped) {
                        OpenUpdateDialog(cfg.auto_install_updates && !manual && update_popup_screenshot.empty());
                    }
                } else {
                    update_status_line = L"v" + Utf8ToWide(effective_version) + L" · Up to date";
                    if (manual) SetToast(L"You're up to date (v" + Utf8ToWide(effective_version) + L")", kColGreen);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            case WM_TIMER: {
                if (wp == 1) {
                    KillTimer(hwnd, 1);
                    SnapSprings();
                    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                    CaptureWindowToPng(hwnd, screenshot_out);
                    PostQuitMessage(0);
                } else if (wp == 2) {
                    StepAnimations();
                }
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd, &ps);
                Paint(hdc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_CTLCOLOREDIT: {
                HDC hdc = reinterpret_cast<HDC>(wp);
                SetTextColor(hdc, kColText);
                SetBkColor(hdc, kColRaised);
                return reinterpret_cast<LRESULT>(h_br_input);
            }
            case WM_MOUSEMOVE: {
                int mx = GET_X_LPARAM(lp);
                int my = GET_Y_LPARAM(lp);
                const HitZone* z = HitTest(mx, my);
                HitKind nk = z ? z->kind : HitKind::None;
                int ni = z ? z->index : -1;
                if (nk != hover_kind || ni != hover_index) {
                    hover_kind = nk;
                    hover_index = ni;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                SetCursor(LoadCursorW(nullptr, nk != HitKind::None ? IDC_HAND : IDC_ARROW));
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                return 0;
            }
            case WM_MOUSELEAVE:
                if (hover_kind != HitKind::None) {
                    hover_kind = HitKind::None;
                    hover_index = -1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_LBUTTONDOWN: {
                int mx = GET_X_LPARAM(lp);
                int my = GET_Y_LPARAM(lp);
                if (const HitZone* z = HitTest(mx, my)) {
                    pressed_kind = z->kind;
                    pressed_index = z->index;
                    sp_press.target = 0.97f;
                    SetCapture(hwnd);
                    StartAnimTimer();
                }
                return 0;
            }
            case WM_LBUTTONUP: {
                int mx = GET_X_LPARAM(lp);
                int my = GET_Y_LPARAM(lp);
                HitKind pk = pressed_kind;
                int pi = pressed_index;
                pressed_kind = HitKind::None;
                pressed_index = -1;
                sp_press.target = 1.0f;
                if (GetCapture() == hwnd) ReleaseCapture();
                if (const HitZone* z = HitTest(mx, my)) {
                    if (z->kind == pk && z->index == pi) {
                        HandleAction(pk, pi);
                    }
                }
                StartAnimTimer();
                return 0;
            }
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: {
                if (listening_key_slot >= 0 && listening_key_slot < static_cast<int>(std::size(kKeySlots))) {
                    std::string kn = VkToSdlKeyName(wp, lp);
                    if (!kn.empty()) {
                        cfg.*(kKeySlots[listening_key_slot].field) = kn;
                        if (listening_key_slot == 7) cfg.key_back = kn;
                        dirty = true;
                        SetToast(std::wstring(kKeySlots[listening_key_slot].label) + L" bound to " + Utf8ToWide(kn), kColGreen);
                        listening_key_slot = -1;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
                break;
            }
            case WM_DESTROY:
                alive_flag->store(false, std::memory_order_release);
                if (anim_timer_on) KillTimer(hwnd, 2);
                if (h_ui_font) DeleteObject(h_ui_font);
                if (h_mono_font) DeleteObject(h_mono_font);
                if (h_br_input) DeleteObject(h_br_input);
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

} // namespace

bool RunLauncherUi(const LauncherOptions& opts, PcConfig& out_cfg) {
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    ULONG_PTR gdip_token = 0;
    Gdiplus::GdiplusStartupInput gdip_in;
    Gdiplus::GdiplusStartup(&gdip_token, &gdip_in, nullptr);

    ConfiguratorApp app;
    app.hinst = hinst;
    app.config_path = opts.config_path.empty() ? ResolveDefaultConfigPath() : opts.config_path;
    app.active_tab = std::clamp(opts.initial_tab, 0, 3);
    app.screenshot_out = opts.screenshot_out;
    app.update_popup_screenshot = opts.update_popup_screenshot;
    app.disable_update_check = opts.disable_update_check;
    if (!opts.simulate_version.empty()) {
        app.effective_version = opts.simulate_version;
    }

    app.cfg = LoadConfig(app.config_path);
    SaveDefaultConfigIfMissing(app.config_path, app.cfg);
    app.data_status = CheckGameDataStatus(app.cfg);

    HDC screen_dc = GetDC(nullptr);
    app.dpi = static_cast<UINT>(GetDeviceCaps(screen_dc, LOGPIXELSX));
    ReleaseDC(nullptr, screen_dc);
    if (app.dpi < 96) app.dpi = 96;

    app.h_icon_large = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APP_ICON),
                                                     IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR));
    app.h_icon_small = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APP_ICON),
                                                     IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));

    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ConfiguratorApp::StaticWndProc;
        wc.hInstance = hinst;
        wc.hIcon = app.h_icon_large;
        wc.hIconSm = app.h_icon_small;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"RR3PcConfiguratorWnd";
        RegisterClassExW(&wc);
        class_registered = true;
    }

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, app.S(kBaseW), app.S(kBaseH)};
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int wx = work.left + std::max(0L, (work.right - work.left - win_w) / 2);
    int wy = work.top + std::max(0L, (work.bottom - work.top - win_h) / 2);

    HWND hwnd = CreateWindowExW(0, L"RR3PcConfiguratorWnd", L"Real Racing 3 PC",
                                style, wx, wy, win_w, win_h, nullptr, nullptr, hinst, &app);
    if (!hwnd) {
        if (app.h_icon_large) DestroyIcon(app.h_icon_large);
        if (app.h_icon_small) DestroyIcon(app.h_icon_small);
        Gdiplus::GdiplusShutdown(gdip_token);
        if (SUCCEEDED(com_hr)) CoUninitialize();
        return false;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (app.listening_key_slot < 0 && IsDialogMessageW(hwnd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    bool should_launch = app.launch_requested;
    if (should_launch) {
        out_cfg = app.cfg;
    }

    if (app.h_icon_large) DestroyIcon(app.h_icon_large);
    if (app.h_icon_small) DestroyIcon(app.h_icon_small);
    Gdiplus::GdiplusShutdown(gdip_token);
    if (SUCCEEDED(com_hr)) CoUninitialize();
    return should_launch;
}
