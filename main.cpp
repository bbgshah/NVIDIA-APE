#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <objidl.h>
#include <gdiplus.h>
#include "resource.h"
#include "audio_engine.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;

// ---- globals shared by engine + UI -----------------------------------------
NOTIFYICONDATA nid = {};
HWND hMainWnd = NULL, hSettingsWnd = NULL;
EngineSettings g_Settings;
bool g_IsMinimizedToTray = false;
char g_GpuName[256] = "Checking...";
int  g_InDev = -1, g_OutDev = -1;          // -1 = system default (WAVE_MAPPER)

// ============================================================================
//  Audio engine: waveIn (mic) -> CUDA -> waveOut (speakers / virtual cable)
// ============================================================================
static const int SR = 48000, BLOCK = 480, NBUF = 8;   // 10 ms blocks, mono 16-bit

struct Engine {
    HWAVEIN  hIn  = NULL;
    HWAVEOUT hOut = NULL;
    WAVEHDR  inHdr[NBUF], outHdr[NBUF];
    short    inBuf[NBUF][BLOCK], outBuf[NBUF][BLOCK];
    bool     outBusy[NBUF];
    int      inCur = 0, outCur = 0;
    HANDLE   hEvent = NULL, hThread = NULL;
    volatile bool run = false;
    volatile LONG blocks = 0;          // diagnostics
    volatile LONGLONG gpuTicks = 0;
    std::string err;

    Engine() { memset(inHdr, 0, sizeof(inHdr)); memset(outHdr, 0, sizeof(outHdr)); memset(outBusy, 0, sizeof(outBusy)); }

    bool Running() const { return run; }

    bool Start(int inDev, int outDev) {
        if (run) return true;
        if (!gpuEngineInit()) { err = "CUDA initialisation failed."; return false; }

        WAVEFORMATEX f; memset(&f, 0, sizeof(f));
        f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 1; f.nSamplesPerSec = SR;
        f.wBitsPerSample = 16; f.nBlockAlign = 2; f.nAvgBytesPerSec = SR * 2;

        hEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (waveInOpen(&hIn, (UINT)inDev, &f, (DWORD_PTR)hEvent, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
            err = "Could not open the input device."; Cleanup(); return false;
        }
        if (waveOutOpen(&hOut, (UINT)outDev, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            err = "Could not open the output device."; Cleanup(); return false;
        }

        for (int i = 0; i < NBUF; ++i) {
            inHdr[i].lpData = (LPSTR)inBuf[i];  inHdr[i].dwBufferLength = BLOCK * 2;
            waveInPrepareHeader(hIn, &inHdr[i], sizeof(WAVEHDR));
            waveInAddBuffer(hIn, &inHdr[i], sizeof(WAVEHDR));
            outHdr[i].lpData = (LPSTR)outBuf[i]; outHdr[i].dwBufferLength = BLOCK * 2;
            waveOutPrepareHeader(hOut, &outHdr[i], sizeof(WAVEHDR));
            outBusy[i] = false;
        }
        inCur = outCur = 0;
        for (int i = 0; i < 3; ++i) {              // small jitter buffer of silence
            memset(outBuf[outCur], 0, BLOCK * 2);
            waveOutWrite(hOut, &outHdr[outCur], sizeof(WAVEHDR));
            outBusy[outCur] = true; outCur = (outCur + 1) % NBUF;
        }
        run = true;
        hThread = CreateThread(NULL, 0, ThreadEntry, this, 0, NULL);
        waveInStart(hIn);
        return true;
    }

    void Stop() {
        if (!run && !hIn && !hOut) return;
        run = false;
        if (hEvent) SetEvent(hEvent);
        if (hThread) { WaitForSingleObject(hThread, 2000); CloseHandle(hThread); hThread = NULL; }
        Cleanup();
    }

    void Cleanup() {
        if (hIn) {
            waveInReset(hIn);
            for (int i = 0; i < NBUF; ++i) waveInUnprepareHeader(hIn, &inHdr[i], sizeof(WAVEHDR));
            waveInClose(hIn); hIn = NULL;
        }
        if (hOut) {
            waveOutReset(hOut);
            for (int i = 0; i < NBUF; ++i) waveOutUnprepareHeader(hOut, &outHdr[i], sizeof(WAVEHDR));
            waveOutClose(hOut); hOut = NULL;
        }
        if (hEvent) { CloseHandle(hEvent); hEvent = NULL; }
        run = false;
    }

    static DWORD WINAPI ThreadEntry(LPVOID p) { ((Engine*)p)->Loop(); return 0; }

    void Loop() {
        const int H = gpuHistoryLength();
        std::vector<float> in(H + BLOCK, 0.f), out(BLOCK, 0.f);
        float gate = 1.f; int hold = 0;

        while (run) {
            WaitForSingleObject(hEvent, 50);
            while (run && (inHdr[inCur].dwFlags & WHDR_DONE)) {
                WAVEHDR& h = inHdr[inCur];
                const short* pcm = inBuf[inCur];
                int got = (int)(h.dwBytesRecorded / 2); if (got > BLOCK) got = BLOCK;

                // slide history, append new block
                memmove(in.data(), in.data() + BLOCK, H * sizeof(float));
                double sq = 0.0;
                for (int i = 0; i < BLOCK; ++i) {
                    float v = (i < got) ? pcm[i] / 32768.0f : 0.0f;
                    in[H + i] = v; sq += (double)v * v;
                }

                EngineSettings s = g_Settings;      // snapshot

                // noise gate (decision on CPU, applied inside the CUDA kernel)
                float g0 = gate;
                if (s.enableNoiseGate) {
                    float rms = sqrtf((float)(sq / BLOCK));
                    float thr = powf(10.0f, s.gateThresholdDb / 20.0f);
                    if (rms > thr) hold = 15; else if (hold > 0) --hold;
                    gate = (hold > 0) ? 1.0f : fmaxf(0.0f, gate - 0.1f);
                } else gate = 1.0f;

                LARGE_INTEGER qa, qb; QueryPerformanceCounter(&qa);
                bool ok = gpuProcessBlock(in.data(), out.data(), BLOCK, &s, g0, gate);
                QueryPerformanceCounter(&qb);
                gpuTicks = gpuTicks + (qb.QuadPart - qa.QuadPart);
                blocks = blocks + 1;

                if (!outBusy[outCur] || (outHdr[outCur].dwFlags & WHDR_DONE)) {
                    float peak = 0.f; short* dst = outBuf[outCur];
                    for (int i = 0; i < BLOCK; ++i) {
                        float v = ok ? out[i] : 0.f;
                        if (v > 1.f) v = 1.f; if (v < -1.f) v = -1.f;
                        if (fabsf(v) > peak) peak = fabsf(v);
                        dst[i] = (short)(v * 32767.0f);
                    }
                    waveOutWrite(hOut, &outHdr[outCur], sizeof(WAVEHDR));
                    outBusy[outCur] = true; outCur = (outCur + 1) % NBUF;
                    g_Settings.livePeakLevel = peak;
                }   // else: output is backed up, drop this block

                if (waveInAddBuffer(hIn, &h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                    err = "Input device stopped delivering audio.";
                    run = false; break;                     // never spin on a dead buffer
                }
                inCur = (inCur + 1) % NBUF;
            }
        }
    }
};
Engine g_Engine;

// ============================================================================
//  Persistence + startup helpers
// ============================================================================
static std::string IniPath() {
    char p[MAX_PATH]; GetModuleFileNameA(NULL, p, MAX_PATH);
    std::string s(p); return s.substr(0, s.find_last_of('\\') + 1) + "ape_settings.ini";
}
static void SaveSettings() {
    std::string ini = IniPath();
    auto W = [&](const char* k, float v) { char b[32]; snprintf(b, 32, "%.3f", v); WritePrivateProfileStringA("APE", k, b, ini.c_str()); };
    EngineSettings& s = g_Settings;
    W("eq", s.enableEQ); W("gate", s.enableNoiseGate); W("sat", s.enableSaturation); W("rev", s.enableReverb);
    W("startup", s.startWithWindows); W("tray", s.minimizeToTray);
    W("low", s.lowGainDb); W("mid", s.midGainDb); W("high", s.highGainDb); W("drive", s.saturationDrive);
    W("gateDb", s.gateThresholdDb); W("revMix", s.reverbMix); W("outDb", s.outputGainDb);
    W("inDev", (float)g_InDev); W("outDev", (float)g_OutDev);
}
static void LoadSettings() {
    std::string ini = IniPath();
    auto R = [&](const char* k, float d) { char b[32]; GetPrivateProfileStringA("APE", k, "", b, 32, ini.c_str()); return b[0] ? (float)atof(b) : d; };
    EngineSettings& s = g_Settings;
    s.enableEQ = R("eq", 1) != 0; s.enableNoiseGate = R("gate", 1) != 0;
    s.enableSaturation = R("sat", 0) != 0; s.enableReverb = R("rev", 0) != 0;
    s.startWithWindows = R("startup", 0) != 0; s.minimizeToTray = R("tray", 1) != 0;
    s.lowGainDb = R("low", 0); s.midGainDb = R("mid", 0); s.highGainDb = R("high", 0);
    s.saturationDrive = R("drive", 1.5f); s.gateThresholdDb = R("gateDb", -50); s.reverbMix = R("revMix", 0.25f);
    s.outputGainDb = R("outDb", 0);
    g_InDev = (int)R("inDev", -1); g_OutDev = (int)R("outDev", -1);
    if (g_InDev  >= (int)waveInGetNumDevs())  g_InDev  = -1;
    if (g_OutDev >= (int)waveOutGetNumDevs()) g_OutDev = -1;
}

void SetStartupWithWindows(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            char szPath[MAX_PATH]; GetModuleFileNameA(NULL, szPath, MAX_PATH);
            std::string cmd = std::string("\"") + szPath + "\" --tray";
            RegSetValueExA(hKey, APP_NAME, 0, REG_SZ, (BYTE*)cmd.c_str(), (DWORD)(cmd.size() + 1));
        } else RegDeleteValueA(hKey, APP_NAME);
        RegCloseKey(hKey);
    }
}

void AddSystemTrayIcon(HWND hwnd) {
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd; nid.uID = IDI_TRAY_ICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_TRAY_ICON));
    lstrcpyA(nid.szTip, "NVIDIA APE (Audio Processing Engine)");
    Shell_NotifyIconA(NIM_ADD, &nid);
}
void RemoveSystemTrayIcon() { Shell_NotifyIconA(NIM_DELETE, &nid); }

// ============================================================================
//  UI toolkit (GDI+): palette, animation, drawing helpers, hit-testing
//  Immediate-mode: every paint rebuilds the list of clickable rectangles.
// ============================================================================
static float g_scale = 1.0f;                       // DPI scale; layout is in 96-dpi units
static ULONG_PTR g_gdipToken = 0;
static Font *fTitle, *fH, *fBody, *fSmall, *fBold, *fBig, *fVal;

static Color Cc(int r, int g, int b, int a = 255) { return Color((BYTE)a, (BYTE)r, (BYTE)g, (BYTE)b); }
static Color NV(int a = 255) { return Cc(118, 185, 0, a); }
static Color Lerp(Color a, Color b, float t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    return Color((BYTE)(a.GetA() + (b.GetA() - a.GetA()) * t), (BYTE)(a.GetR() + (b.GetR() - a.GetR()) * t),
                 (BYTE)(a.GetG() + (b.GetG() - a.GetG()) * t), (BYTE)(a.GetB() + (b.GetB() - a.GetB()) * t));
}
static std::wstring WS(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, NULL, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], n); w.resize(n - 1); return w;
}
static void InitFonts() {
    fTitle = new Font(L"Segoe UI", 26.f, FontStyleBold,     UnitPixel);
    fBig   = new Font(L"Segoe UI", 18.f, FontStyleBold,     UnitPixel);
    fH     = new Font(L"Segoe UI", 15.f, FontStyleBold,     UnitPixel);
    fBody  = new Font(L"Segoe UI", 13.f, FontStyleRegular,  UnitPixel);
    fBold  = new Font(L"Segoe UI", 13.f, FontStyleBold,     UnitPixel);
    fSmall = new Font(L"Segoe UI", 11.f, FontStyleRegular,  UnitPixel);
    fVal   = new Font(L"Consolas", 15.f, FontStyleBold,     UnitPixel);
}

// ---- animation: each key eases toward its target -------------------------------
struct An { float v = 0, t = 0; };
static std::map<int, An> g_an;
static float Anim(int key, float target) { An& a = g_an[key]; a.t = target; return a.v; }
static bool StepAnims() {
    bool ch = false;
    for (auto& kv : g_an) {
        An& a = kv.second; float d = a.t - a.v;
        if (fabsf(d) > 0.003f) { a.v += d * 0.26f; ch = true; } else a.v = a.t;
    }
    return ch;
}

// ---- shapes & text --------------------------------------------------------------
static void RRPath(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    REAL d = r * 2; if (d > h) d = h; if (d > w) d = w;
    p.AddArc(x, y, d, d, 180.f, 90.f); p.AddArc(x + w - d, y, d, d, 270.f, 90.f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.f, 90.f); p.AddArc(x, y + h - d, d, d, 90.f, 90.f);
    p.CloseFigure();
}
static void FillRR(Graphics& g, const Brush& b, REAL x, REAL y, REAL w, REAL h, REAL r) {
    GraphicsPath p; RRPath(p, x, y, w, h, r); g.FillPath(&b, &p);
}
static void StrokeRR(Graphics& g, Color c, REAL pw, REAL x, REAL y, REAL w, REAL h, REAL r) {
    GraphicsPath p; RRPath(p, x, y, w, h, r); Pen pen(c, pw); g.DrawPath(&pen, &p);
}
static void GlowRR(Graphics& g, Color c, REAL x, REAL y, REAL w, REAL h, REAL r, int layers, float alpha) {
    if (alpha > 255) alpha = 255; if (alpha < 1) return;
    for (int i = layers; i >= 1; --i) {
        REAL s = i * 2.4f;
        SolidBrush b(Color((BYTE)(alpha / layers), c.GetR(), c.GetG(), c.GetB()));
        FillRR(g, b, x - s, y - s, w + 2 * s, h + 2 * s, r + s);
    }
}
static void DrawT(Graphics& g, const std::wstring& s, REAL x, REAL y, REAL w, REAL h, Font* f, Color c,
                  StringAlignment ha = StringAlignmentNear, StringAlignment va = StringAlignmentNear,
                  bool ellipsis = false, bool wrap = false) {
    SolidBrush b(c); StringFormat sf;
    sf.SetAlignment(ha); sf.SetLineAlignment(va);
    if (!wrap) sf.SetFormatFlags(StringFormatFlagsNoWrap);
    if (ellipsis) sf.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(s.c_str(), -1, f, RectF(x, y, w, h), &sf, &b);
}
static float TW(Graphics& g, const WCHAR* s, Font* f) {
    RectF b; g.MeasureString(s, -1, f, PointF(0, 0), StringFormat::GenericTypographic(), &b); return b.Width;
}
static void Background(Graphics& g, float w, float h) {
    LinearGradientBrush lb(PointF(0, 0), PointF(0, h), Cc(21, 23, 21), Cc(9, 10, 9));
    g.FillRectangle(&lb, 0.f, 0.f, w, h);
    Color sc = Cc(118, 185, 0, 0); int n = 1;
    GraphicsPath a; a.AddEllipse(-260.f, -300.f, 760.f, 520.f);
    PathGradientBrush pa(&a); pa.SetCenterColor(Cc(118, 185, 0, 34)); pa.SetSurroundColors(&sc, &n); g.FillPath(&pa, &a);
    GraphicsPath b; b.AddEllipse(w - 380.f, h - 260.f, 620.f, 420.f);
    PathGradientBrush pb(&b); pb.SetCenterColor(Cc(118, 185, 0, 16)); pb.SetSurroundColors(&sc, &n); g.FillPath(&pb, &b);
}
static void SectionLabel(Graphics& g, const WCHAR* t, REAL x, REAL y, REAL w) {
    DrawT(g, t, x, y, w, 16, fSmall, Cc(118, 185, 0));
    LinearGradientBrush lb(PointF(x, 0), PointF(x + w, 0), Cc(70, 76, 70), Cc(70, 76, 70, 0));
    g.FillRectangle(&lb, x + TW(g, t, fSmall) + 12, y + 8.f, w - TW(g, t, fSmall) - 12, 1.f);
}
static void CardBox(Graphics& g, REAL x, REAL y, REAL w, REAL h, REAL r, float hov, float on) {
    if (on > 0.01f) GlowRR(g, NV(), x, y, w, h, r, 6, 26 * on);
    Color top = Lerp(Lerp(Cc(29, 32, 29), Cc(35, 40, 35), hov), Cc(31, 44, 22), on * 0.7f);
    Color bot = Lerp(Lerp(Cc(20, 22, 20), Cc(25, 28, 25), hov), Cc(22, 30, 16), on * 0.7f);
    LinearGradientBrush lb(PointF(x, y), PointF(x, y + h), top, bot);
    FillRR(g, lb, x, y, w, h, r);
    StrokeRR(g, Lerp(Cc(48, 53, 48), NV(), (on * 0.75f > hov * 0.5f) ? on * 0.75f : hov * 0.5f), 1.f, x, y, w, h, r);
}
static void DrawToggle(Graphics& g, REAL x, REAL y, int key, bool on) {
    float a = Anim(key, on ? 1.f : 0.f);
    if (a > 0.02f) GlowRR(g, NV(), x, y, 44, 24, 12, 5, 40 * a);
    SolidBrush bg(Lerp(Cc(56, 61, 56), NV(), a)); FillRR(g, bg, x, y, 44, 24, 12);
    REAL kx = x + 3 + a * 20;
    SolidBrush sh(Cc(0, 0, 0, 60)); g.FillEllipse(&sh, kx, y + 4.f, 18.f, 18.f);
    SolidBrush kn(Lerp(Cc(200, 205, 200), Cc(255, 255, 255), a)); g.FillEllipse(&kn, kx, y + 3.f, 18.f, 18.f);
}
static void DrawIcon(Graphics& g, int kind, REAL cx, REAL cy, Color c) {
    Pen pen(c, 2.f); pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
    SolidBrush br(c);
    switch (kind) {
    case 0: { static const REAL ky[3] = { -2.f, 4.f, -5.f };
        for (int i = 0; i < 3; ++i) { REAL x = cx - 7 + i * 7; g.DrawLine(&pen, x, cy - 8, x, cy + 8); g.FillEllipse(&br, x - 3.f, cy + ky[i] - 3.f, 6.f, 6.f); }
        break; }
    case 1: { GraphicsPath p; RRPath(p, cx - 3.5f, cy - 10.f, 7.f, 12.f, 3.5f); g.DrawPath(&pen, &p);
        g.DrawArc(&pen, cx - 7.f, cy - 6.f, 14.f, 12.f, 0.f, 180.f); g.DrawLine(&pen, cx, cy + 6.f, cx, cy + 10.f); break; }
    case 2: g.DrawBezier(&pen, cx - 9.f, cy + 7.f, cx - 1.f, cy + 7.f, cx + 1.f, cy - 7.f, cx + 9.f, cy - 7.f); break;
    case 3: g.FillEllipse(&br, cx - 9.f, cy - 2.f, 4.f, 4.f);
        g.DrawArc(&pen, cx - 12.f, cy - 6.f, 12.f, 12.f, -50.f, 100.f);
        g.DrawArc(&pen, cx - 12.f, cy - 11.f, 22.f, 22.f, -50.f, 100.f); break;
    }
}

// ---- panes: double-buffered window + hit list --------------------------------------
struct Hit { RectF r; int id; };
struct Pane {
    std::vector<Hit> hits; int hover = -1, down = -1; bool tracking = false;
    HDC mdc = NULL; HBITMAP bmp = NULL; HGDIOBJ old = NULL; int bw = 0, bh = 0;
};
static void AddHit(Pane& p, REAL x, REAL y, REAL w, REAL h, int id) { Hit t; t.r = RectF(x, y, w, h); t.id = id; p.hits.push_back(t); }
static int HitTest(Pane& p, int mx, int my) {
    float x = mx / g_scale, y = my / g_scale;
    for (int i = (int)p.hits.size() - 1; i >= 0; --i) if (p.hits[i].r.Contains(x, y)) return p.hits[i].id;
    return -1;
}
static float Hv(Pane& p, int id) { return Anim(id, p.hover == id ? 1.f : 0.f); }
static void FreePane(Pane& p) {
    if (p.mdc) { SelectObject(p.mdc, p.old); DeleteObject(p.bmp); DeleteDC(p.mdc); p.mdc = NULL; p.bmp = NULL; }
    p.bw = p.bh = 0;
}
typedef void (*DrawFn)(Graphics&, Pane&, float, float);
static void PaintPane(HWND hwnd, Pane& p, DrawFn fn) {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw > 0 && ch > 0) {
        if (!p.mdc || cw != p.bw || ch != p.bh) {
            FreePane(p);
            p.mdc = CreateCompatibleDC(hdc); p.bmp = CreateCompatibleBitmap(hdc, cw, ch);
            p.old = SelectObject(p.mdc, p.bmp); p.bw = cw; p.bh = ch;
        }
        {
            Graphics g(p.mdc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            g.ScaleTransform(g_scale, g_scale);
            p.hits.clear();
            fn(g, p, cw / g_scale, ch / g_scale);
        }
        BitBlt(hdc, 0, 0, cw, ch, p.mdc, 0, 0, SRCCOPY);
    }
    EndPaint(hwnd, &ps);
}
static void TrackLeave(HWND h, Pane& p) {
    if (!p.tracking) { TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t); p.tracking = true; }
}
static void StyleWindow(HWND h) {          // dark title bar / rounded corners (Win10 1809+/Win11; ignored otherwise)
    BOOL dark = TRUE; DwmSetWindowAttribute(h, 20, &dark, sizeof(dark));
    COLORREF cap = RGB(14, 15, 14), txt = RGB(235, 235, 235), bor = RGB(52, 84, 0);
    DwmSetWindowAttribute(h, 35, &cap, sizeof(cap));
    DwmSetWindowAttribute(h, 36, &txt, sizeof(txt));
    DwmSetWindowAttribute(h, 34, &bor, sizeof(bor));
    int round = 2; DwmSetWindowAttribute(h, 33, &round, sizeof(round));
}
static HWND MakeWindow(const char* cls, const char* title, float lw, float lh, HWND owner, int x, int y) {
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, (LONG)(lw * g_scale), (LONG)(lh * g_scale) };
    AdjustWindowRect(&r, style, FALSE);
    return CreateWindowExA(0, cls, title, style, x, y, r.right - r.left, r.bottom - r.top, owner, NULL, GetModuleHandle(NULL), NULL);
}

// ============================================================================
//  Engine control + shared state
// ============================================================================
static float g_phase = 0, g_meterLvl = 0, g_meterHold = 0; static int g_holdT = 0;
static float g_stBlocks = 0, g_stRT = 0, g_stK = 0;

static bool StartEngine() {
    g_Engine.err.clear();
    bool ok = g_Engine.Start(g_InDev, g_OutDev);
    if (!ok && hMainWnd) InvalidateRect(hMainWnd, NULL, FALSE);
    return ok;
}
static void RestartIfRunning() { if (g_Engine.Running()) { g_Engine.Stop(); StartEngine(); } }
static void ToggleEngine() {
    if (g_Engine.Running()) { g_Engine.Stop(); g_Settings.livePeakLevel = 0; g_stBlocks = g_stRT = g_stK = 0; }
    else StartEngine();
}

// ============================================================================
//  Main window
// ============================================================================
static Pane g_mainPane;
static const float MAIN_W = 760.f, MAIN_H = 548.f;

struct ModDef { const WCHAR* name; const WCHAR* desc; int id; int icon; bool* flag; };
static ModDef g_Mods[4] = {
    { L"Parametric EQ",        L"3-band linear-phase FIR \u00B7 low / mid / high", 11, 0, &g_Settings.enableEQ },
    { L"Voice Isolation Gate", L"Smooth noise gate with hold and ramp",            12, 1, &g_Settings.enableNoiseGate },
    { L"Soft Saturation",      L"tanh wave-shaper \u00B7 adjustable drive",        13, 2, &g_Settings.enableSaturation },
    { L"Convolution Reverb",   L"GPU convolution \u00B7 100 ms room response",     14, 3, &g_Settings.enableReverb },
};

static void DrawStat(Graphics& g, REAL x, REAL y, const WCHAR* label, const std::wstring& val) {
    DrawT(g, label, x, y, 130, 14, fSmall, Cc(120, 126, 120));
    DrawT(g, val, x, y + 15, 130, 22, fVal, Cc(235, 238, 235));
}

static void DrawMain(Graphics& g, Pane& p, float W, float H) {
    bool run = g_Engine.Running();
    Background(g, W, H);

    // --- header: logo mark (bars animate while live) ---
    {
        REAL lx = 28, ly = 18;
        if (run) GlowRR(g, NV(), lx, ly, 44, 44, 11, 7, 44);
        LinearGradientBrush lb(PointF(lx, ly), PointF(lx, ly + 44), Cc(150, 214, 20), Cc(84, 140, 0));
        FillRR(g, lb, lx, ly, 44, 44, 11);
        SolidBrush dark(Cc(12, 16, 6)); static const float base[5] = { 0.35f, 0.7f, 1.0f, 0.6f, 0.4f };
        for (int i = 0; i < 5; ++i) {
            float k = base[i]; if (run) k *= 0.55f + 0.45f * sinf(g_phase * 1.6f + i * 1.3f);
            if (k < 0.18f) k = 0.18f; float bh = 26 * k;
            FillRR(g, dark, lx + 9 + i * 6.2f, ly + 22 - bh / 2, 3.6f, bh, 1.8f);
        }
    }
    DrawT(g, L"NVIDIA", 84, 14, 200, 32, fTitle, Cc(240, 242, 240));
    DrawT(g, L"APE", 84 + TW(g, L"NVIDIA", fTitle) + 9, 14, 100, 32, fTitle, NV());
    DrawT(g, L"AUDIO PROCESSING ENGINE", 85, 46, 300, 16, fSmall, Cc(130, 136, 130));

    // status pill
    {
        REAL px = W - 64 - 12 - 100, py = 26;
        float pulse = 0.5f + 0.5f * sinf(g_phase * 1.4f);
        SolidBrush pb(run ? Cc(30, 44, 18) : Cc(28, 31, 28)); FillRR(g, pb, px, py, 100, 28, 14);
        StrokeRR(g, run ? NV(150) : Cc(52, 57, 52), 1.f, px, py, 100, 28, 14);
        Color dc = run ? NV() : Cc(110, 116, 110);
        if (run) { SolidBrush gl(Cc(118, 185, 0, (int)(70 * pulse))); g.FillEllipse(&gl, px + 10.f, py + 6.f, 16.f, 16.f); }
        SolidBrush dot(dc); g.FillEllipse(&dot, px + 14.f, py + 10.f, 8.f, 8.f);
        DrawT(g, run ? L"LIVE" : L"IDLE", px + 30, py, 66, 28, fBold, run ? NV() : Cc(150, 156, 150), StringAlignmentNear, StringAlignmentCenter);
    }
    // settings button
    {
        REAL bx = W - 64, by = 22; float hv = Hv(p, 2); AddHit(p, bx, by, 36, 36, 2);
        SolidBrush bg(Lerp(Cc(28, 31, 28), Cc(42, 48, 38), hv)); FillRR(g, bg, bx, by, 36, 36, 10);
        StrokeRR(g, Lerp(Cc(52, 57, 52), NV(), hv), 1.f, bx, by, 36, 36, 10);
        Pen pen(Lerp(Cc(190, 196, 190), NV(), hv), 1.8f); pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
        SolidBrush kb(Lerp(Cc(190, 196, 190), NV(), hv)); static const REAL kx[3] = { -3.f, 4.f, -1.f };
        for (int i = 0; i < 3; ++i) { REAL yy = by + 11 + i * 7; g.DrawLine(&pen, bx + 10, yy, bx + 26, yy); g.FillEllipse(&kb, bx + 18 + kx[i] - 2.5f, yy - 2.5f, 5.f, 5.f); }
    }
    { LinearGradientBrush lb(PointF(28, 0), PointF(W - 28, 0), NV(130), Cc(118, 185, 0, 0)); g.FillRectangle(&lb, 28.f, 80.f, W - 56, 1.f); }

    // --- GPU card ---
    CardBox(g, 28, 98, 430, 124, 14, 0, 0);
    DrawT(g, L"GPU TARGET", 48, 112, 200, 14, fSmall, Cc(118, 185, 0));
    DrawT(g, WS(g_GpuName), 48, 128, 392, 26, fBig, Cc(240, 242, 240), StringAlignmentNear, StringAlignmentNear, true);
    {
        wchar_t b1[32] = L"\u2014", b2[32] = L"\u2014", b3[32] = L"\u2014";
        if (run && g_stBlocks > 0) { swprintf(b1, 32, L"%.0f", g_stBlocks); swprintf(b2, 32, L"%.2f ms", g_stRT); swprintf(b3, 32, L"%.3f ms", g_stK); }
        DrawStat(g, 48, 168, L"BLOCKS / SEC", b1); DrawStat(g, 190, 168, L"ROUND TRIP", b2); DrawStat(g, 322, 168, L"KERNEL", b3);
    }

    // --- power card ---
    CardBox(g, 474, 98, 258, 124, 14, 0, run ? 0.6f : 0);
    {
        REAL bx = 494, by = 118, bw = 218, bh = 52; float hv = Hv(p, 1); AddHit(p, bx, by, bw, bh, 1);
        if (!run) {
            GlowRR(g, NV(), bx, by, bw, bh, 26, 8, 30 + 50 * hv);
            LinearGradientBrush lb(PointF(bx, by), PointF(bx, by + bh), Lerp(Cc(138, 205, 10), Cc(160, 225, 30), hv), Lerp(Cc(98, 160, 0), Cc(118, 185, 0), hv));
            FillRR(g, lb, bx, by, bw, bh, 26);
            PointF tri[3] = { PointF(bx + 38, by + 17), PointF(bx + 38, by + 35), PointF(bx + 52, by + 26) };
            SolidBrush d(Cc(14, 20, 6)); g.FillPolygon(&d, tri, 3);
            DrawT(g, L"START PROCESSING", bx + 58, by, bw - 58, bh, fBold, Cc(14, 20, 6), StringAlignmentNear, StringAlignmentCenter);
        } else {
            float pulse = 0.5f + 0.5f * sinf(g_phase * 1.4f);
            GlowRR(g, NV(), bx, by, bw, bh, 26, 8, 22 + 40 * pulse + 30 * hv);
            SolidBrush bgb(Lerp(Cc(22, 30, 16), Cc(30, 42, 20), hv)); FillRR(g, bgb, bx, by, bw, bh, 26);
            StrokeRR(g, NV(), 2.f, bx, by, bw, bh, 26);
            SolidBrush sq(NV()); FillRR(g, sq, bx + 38, by + 18, 15, 15, 3);
            DrawT(g, L"STOP PROCESSING", bx + 62, by, bw - 62, bh, fBold, NV(), StringAlignmentNear, StringAlignmentCenter);
        }
        if (!run && !g_Engine.err.empty())
            DrawT(g, WS(g_Engine.err), 484, 180, 238, 32, fSmall, Cc(236, 96, 84), StringAlignmentCenter, StringAlignmentNear, false, true);
        else
            DrawT(g, L"48 kHz \u00B7 mono \u00B7 10 ms blocks", 484, 184, 238, 16, fSmall, Cc(120, 126, 120), StringAlignmentCenter);
    }

    // --- modules ---
    SectionLabel(g, L"MODULES", 28, 238, 704);
    for (int i = 0; i < 4; ++i) {
        const ModDef& m = g_Mods[i];
        REAL x = (i % 2) ? 386.f : 28.f, y = 262.f + (i / 2) * 96.f, w = 346, h = 84;
        float hv = Hv(p, m.id); bool on = *m.flag; float a = Anim(m.id + 5000, on ? 1.f : 0.f);
        AddHit(p, x, y, w, h, m.id);
        CardBox(g, x, y, w, h, 14, hv, a);
        SolidBrush ib(Lerp(Cc(36, 40, 36), Cc(40, 62, 14), a)); g.FillEllipse(&ib, x + 18, y + 22.f, 40.f, 40.f);
        DrawIcon(g, m.icon, x + 38, y + 42, Lerp(Cc(150, 156, 150), NV(), a));
        DrawT(g, m.name, x + 74, y + 21, 200, 20, fH, Cc(240, 242, 240));
        DrawT(g, m.desc, x + 74, y + 44, 196, 30, fSmall, Cc(128, 134, 128), StringAlignmentNear, StringAlignmentNear, false, true);
        DrawToggle(g, x + w - 64, y + 30, m.id + 5000, on);
    }

    // --- meter ---
    DrawT(g, L"OUTPUT LEVEL", 28, 462, 200, 16, fSmall, Cc(118, 185, 0));
    {
        wchar_t db[32] = L"\u2014"; if (run && g_meterLvl > 0.001f) swprintf(db, 32, L"%.1f dB", g_meterLvl * 60.f - 60.f);
        DrawT(g, db, W - 28 - 120, 462, 120, 16, fSmall, Cc(200, 206, 200), StringAlignmentFar);
        const int N = 56; REAL x0 = 28, y0 = 484, mw = 704, mh = 14, gap = 3.f, sw = (mw - gap * (N - 1)) / N;
        int lit = (int)(g_meterLvl * N + 0.5f), hs = (int)(g_meterHold * N);
        for (int i = 0; i < N; ++i) {
            float f = (float)i / N;
            Color on = f < 0.70f ? Cc(118, 185, 0) : f < 0.88f ? Cc(222, 176, 0) : Cc(226, 62, 52);
            bool isHold = (i == hs && hs >= lit && g_meterHold > 0.03f);
            SolidBrush b((i < lit || isHold) ? on : Cc(33, 37, 33));
            FillRR(g, b, x0 + i * (sw + gap), y0, sw, mh, 2.f);
        }
    }

    // --- footer ---
    DrawT(g, WS(std::string(APP_AUTHOR) + "   \u00B7   v" + APP_VERSION), 28, 518, 400, 16, fSmall, Cc(120, 126, 120));
    {
        std::wstring gh = WS(std::string(APP_GITHUB)); float hv = Hv(p, 20);
        REAL tw = TW(g, gh.c_str(), fSmall) + 4, gx = W - 28 - tw;
        AddHit(p, gx - 4, 514, tw + 8, 22, 20);
        DrawT(g, gh, gx, 518, tw + 10, 16, fSmall, Lerp(Cc(150, 156, 150), NV(), hv));
        if (hv > 0.05f) { SolidBrush u(NV((int)(200 * hv))); g.FillRectangle(&u, gx, 534.f, tw - 4, 1.f); }
    }
}

static void MainActivate(HWND hwnd, int id) {
    if (id == 1) { ToggleEngine(); }
    else if (id == 2) { extern void OpenSettings(); OpenSettings(); }
    else if (id >= 11 && id <= 14) { bool& f = *g_Mods[id - 11].flag; f = !f; }
    else if (id == 20) ShellExecuteA(hwnd, "open", "https://" APP_GITHUB, NULL, NULL, SW_SHOWNORMAL);
    InvalidateRect(hwnd, NULL, FALSE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Pane& p = g_mainPane;
    switch (msg) {
    case WM_CREATE:
        AddSystemTrayIcon(hwnd); StyleWindow(hwnd); SetTimer(hwnd, 1, 16, NULL); return 0;

    case WM_TIMER: {
        static int n = 0; static bool lastRun = false; static DWORD lastT = 0; static LONG lastB = 0; static LONGLONG lastTk = 0;
        ++n; bool run = g_Engine.Running();
        g_phase += 0.09f;
        // meter ballistics
        float pk = run ? g_Settings.livePeakLevel : 0.f;
        float db = 20.f * log10f(pk + 1e-6f), lvl = (db + 60.f) / 60.f; if (lvl < 0) lvl = 0; if (lvl > 1) lvl = 1;
        g_meterLvl = (lvl > g_meterLvl) ? lvl : g_meterLvl * 0.90f;
        if (g_meterLvl < 0.002f) g_meterLvl = 0;
        if (g_meterLvl > g_meterHold) { g_meterHold = g_meterLvl; g_holdT = 40; }
        else if (g_holdT > 0) --g_holdT; else g_meterHold *= 0.96f;
        // 1 s stats
        DWORD now = GetTickCount();
        if (run && now - lastT >= 1000) {
            LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
            LONG b = g_Engine.blocks; LONGLONG t = g_Engine.gpuTicks; LONG db2 = b - lastB; LONGLONG dt = t - lastTk;
            if (lastT && db2 > 0) {
                g_stBlocks = db2 / ((now - lastT) / 1000.f); g_stRT = (float)((dt * 1000.0 / fq.QuadPart) / db2); g_stK = gpuTakeKernelAvgMs();
            }
            lastT = now; lastB = b; lastTk = t;
        } else if (!run) { lastT = 0; lastB = g_Engine.blocks; lastTk = g_Engine.gpuTicks; }
        bool animCh = StepAnims();
        bool changed = animCh || (lastRun != run) || ((run || g_meterLvl > 0.f || g_meterHold > 0.03f) && (n & 1));
        lastRun = run;
        if (changed && IsWindowVisible(hwnd) && !IsIconic(hwnd)) InvalidateRect(hwnd, NULL, FALSE);
        if (animCh && hSettingsWnd) InvalidateRect(hSettingsWnd, NULL, FALSE);
        return 0;
    }
    case WM_PAINT: PaintPane(hwnd, p, DrawMain); return 0;
    case WM_ERASEBKGND: return 1;

    case WM_MOUSEMOVE: {
        TrackLeave(hwnd, p); int h = HitTest(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h != p.hover) { p.hover = h; InvalidateRect(hwnd, NULL, FALSE); } return 0; }
    case WM_MOUSELEAVE: p.hover = -1; p.tracking = false; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(NULL, p.hover >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: p.down = HitTest(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONUP: {
        int id = HitTest(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (id >= 0 && id == p.down) MainActivate(hwnd, id);
        p.down = -1; return 0; }

    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); g_IsMinimizedToTray = false;
        } else if (lp == WM_RBUTTONUP) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuA(hMenu, MF_STRING, ID_TRAY_RESTORE, "Open Control Panel");
            AppendMenuA(hMenu, MF_STRING, ID_TRAY_EXIT, "Exit");
            POINT pt; GetCursorPos(&pt); SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_TRAY_RESTORE) { ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); g_IsMinimizedToTray = false; }
        else if (LOWORD(wp) == ID_TRAY_EXIT) DestroyWindow(hwnd);
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED && g_Settings.minimizeToTray) { ShowWindow(hwnd, SW_HIDE); g_IsMinimizedToTray = true; }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (hSettingsWnd) DestroyWindow(hSettingsWnd);
        g_Engine.Stop(); gpuEngineShutdown(); SaveSettings();
        FreePane(g_mainPane); RemoveSystemTrayIcon(); PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ============================================================================
//  Settings window
// ============================================================================
static Pane g_setPane;
static const float SET_W = 780.f, SET_H = 568.f;
static int g_open = 0, g_scroll = 0, g_drag = -1;           // g_open: 0 none, 30 input list, 31 output list
static std::vector<std::wstring> g_inNames, g_outNames;
struct Trk { float x, w; }; static Trk g_trk[8];

struct SliderDef { const char* name; float* field; int mn, mx; float scale, dispDiv; const char* fmt; };
static SliderDef g_Sliders[] = {
    { "Low   (below 250 Hz)",   &g_Settings.lowGainDb,        -12,  12,   1,   1, "%+.0f dB" },
    { "Mid   (250 Hz - 4 kHz)", &g_Settings.midGainDb,        -12,  12,   1,   1, "%+.0f dB" },
    { "High  (above 4 kHz)",    &g_Settings.highGainDb,       -12,  12,   1,   1, "%+.0f dB" },
    { "Saturation drive",       &g_Settings.saturationDrive,   10, 100,  10,  10, "%.1fx"    },
    { "Noise gate threshold",   &g_Settings.gateThresholdDb,  -70, -20,   1,   1, "%.0f dB"  },
    { "Reverb mix",             &g_Settings.reverbMix,          0, 100, 100,   1, "%.0f%%"   },
    { "Output gain",            &g_Settings.outputGainDb,     -12,  12,   1,   1, "%+.0f dB" },
};

static void BuildDeviceLists() {
    g_inNames.clear(); g_outNames.clear();
    g_inNames.push_back(L"System default device"); g_outNames.push_back(L"System default device");
    for (UINT i = 0; i < waveInGetNumDevs(); ++i)  { WAVEINCAPSA c;  std::string n = "Unknown device"; if (waveInGetDevCapsA(i, &c, sizeof(c)) == MMSYSERR_NOERROR) n = c.szPname; g_inNames.push_back(WS(n)); }
    for (UINT i = 0; i < waveOutGetNumDevs(); ++i) { WAVEOUTCAPSA c; std::string n = "Unknown device"; if (waveOutGetDevCapsA(i, &c, sizeof(c)) == MMSYSERR_NOERROR) n = c.szPname; g_outNames.push_back(WS(n)); }
}

static void DrawField(Graphics& g, Pane& p, REAL x, REAL y, REAL w, int id, const std::wstring& txt) {
    float hv = Hv(p, id); float op = Anim(id + 5000, g_open == id ? 1.f : 0.f); AddHit(p, x, y, w, 36, id);
    SolidBrush bg(Lerp(Cc(24, 27, 24), Cc(32, 37, 32), hv)); FillRR(g, bg, x, y, w, 36, 10);
    StrokeRR(g, Lerp(Cc(52, 57, 52), NV(), (hv * 0.6f > op) ? hv * 0.6f : op), 1.f, x, y, w, 36, 10);
    DrawT(g, txt, x + 14, y, w - 48, 36, fBody, Cc(228, 232, 228), StringAlignmentNear, StringAlignmentCenter, true);
    Pen pen(Lerp(Cc(150, 156, 150), NV(), op), 1.8f); pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
    REAL cx = x + w - 20, cy = y + 18, d = 3.5f * (1 - 2 * op);
    g.DrawLine(&pen, cx - 5, cy - d, cx, cy + d); g.DrawLine(&pen, cx, cy + d, cx + 5, cy - d);
}

static void DrawSlider(Graphics& g, Pane& p, REAL x, REAL y, REAL w, int idx) {
    const SliderDef& d = g_Sliders[idx]; int id = 50 + idx;
    float hv = Hv(p, id), dr = Anim(id + 5000, g_drag == idx ? 1.f : 0.f);
    int pos = (int)lroundf(*d.field * d.scale);
    char buf[48]; snprintf(buf, 48, d.fmt, pos / d.dispDiv);
    DrawT(g, WS(d.name), x, y, w * 0.68f, 18, fBody, Cc(205, 210, 205));
    DrawT(g, WS(buf), x + w * 0.5f, y, w * 0.5f, 18, fBold, NV(), StringAlignmentFar);
    REAL tx = x + 9, tw = w - 18, ty = y + 32;
    g_trk[idx].x = tx; g_trk[idx].w = tw;
    AddHit(p, x - 4, y + 16, w + 8, 30, id);
    float frac = (float)(pos - d.mn) / (d.mx - d.mn);
    float z = (d.mn < 0 && d.mx > 0) ? (float)(0 - d.mn) / (d.mx - d.mn) : 0.f;
    SolidBrush track(Cc(46, 51, 46)); FillRR(g, track, tx, ty - 2, tw, 4, 2);
    float a = z < frac ? z : frac, b = z < frac ? frac : z;
    if (b - a > 0.001f) { LinearGradientBrush lb(PointF(tx + tw * a, 0), PointF(tx + tw * b + 1, 0), Cc(96, 150, 0), NV()); FillRR(g, lb, tx + tw * a, ty - 2, tw * (b - a), 4, 2); }
    if (z > 0 && z < 1) { SolidBrush zt(Cc(90, 96, 90)); g.FillRectangle(&zt, tx + tw * z - 0.5f, ty - 6, 1.f, 12.f); }
    REAL cx = tx + tw * frac, cy = ty, r = 8.f + 1.5f * dr;
    GlowRR(g, NV(), cx - r, cy - r, r * 2, r * 2, r, 4, 30 + 70 * (hv > dr ? hv : dr));
    SolidBrush ring(NV()); g.FillEllipse(&ring, cx - r, cy - r, r * 2, r * 2);
    SolidBrush inner(Cc(18, 20, 18)); g.FillEllipse(&inner, cx - r + 3, cy - r + 3, r * 2 - 6, r * 2 - 6);
}

static void DrawToggleRow(Graphics& g, Pane& p, REAL x, REAL y, REAL w, int id, const WCHAR* label, bool on) {
    float hv = Hv(p, id); AddHit(p, x, y, w, 44, id);
    CardBox(g, x, y, w, 44, 12, hv, 0);
    DrawT(g, label, x + 16, y, w - 90, 44, fBody, Cc(228, 232, 228), StringAlignmentNear, StringAlignmentCenter);
    DrawToggle(g, x + w - 60, y + 10, id + 5000, on);
}

static void DrawSettings(Graphics& g, Pane& p, float W, float H) {
    Background(g, W, H);
    DrawT(g, L"Settings", 28, 16, 300, 30, fBig, Cc(240, 242, 240));
    DrawT(g, L"Changes apply instantly and are saved automatically", 28, 44, 400, 16, fSmall, Cc(128, 134, 128));
    { REAL bx = W - 28 - 32, by = 18; float hv = Hv(p, 99); AddHit(p, bx, by, 32, 32, 99);
      SolidBrush bg(Lerp(Cc(28, 31, 28), Cc(60, 30, 28), hv)); FillRR(g, bg, bx, by, 32, 32, 9);
      Pen pen(Lerp(Cc(190, 196, 190), Cc(255, 120, 108), hv), 1.8f); pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
      g.DrawLine(&pen, bx + 11, by + 11, bx + 21, by + 21); g.DrawLine(&pen, bx + 21, by + 11, bx + 11, by + 21); }
    { LinearGradientBrush lb(PointF(28, 0), PointF(W - 28, 0), NV(130), Cc(118, 185, 0, 0)); g.FillRectangle(&lb, 28.f, 72.f, W - 56, 1.f); }

    // ---- left column ----
    SectionLabel(g, L"AUDIO DEVICES", 28, 90, 340);
    DrawT(g, L"MICROPHONE INPUT", 28, 116, 300, 14, fSmall, Cc(130, 136, 130));
    DrawField(g, p, 28, 134, 340, 30, g_inNames[(size_t)(g_InDev + 1) < g_inNames.size() ? g_InDev + 1 : 0]);
    DrawT(g, L"OUTPUT", 28, 182, 300, 14, fSmall, Cc(130, 136, 130));
    DrawField(g, p, 28, 200, 340, 31, g_outNames[(size_t)(g_OutDev + 1) < g_outNames.size() ? g_OutDev + 1 : 0]);
    CardBox(g, 28, 248, 340, 72, 12, 0, 0.5f);
    DrawT(g, L"To feed Discord or OBS, install VB-Cable and set Output to \"CABLE Input\". Then choose \"CABLE Output\" as the microphone in that app.",
          42, 254, 314, 62, fSmall, Cc(176, 182, 176), StringAlignmentNear, StringAlignmentCenter, false, true);

    SectionLabel(g, L"GENERAL", 28, 340, 340);
    DrawToggleRow(g, p, 28, 364, 340, 40, L"Start with Windows (minimized)", g_Settings.startWithWindows);
    DrawToggleRow(g, p, 28, 414, 340, 41, L"Minimize to system tray", g_Settings.minimizeToTray);

    SectionLabel(g, L"ABOUT", 28, 476, 340);
    DrawT(g, WS(std::string("Author:  ") + APP_AUTHOR), 28, 498, 340, 16, fSmall, Cc(200, 206, 200));
    DrawT(g, WS(std::string("Version:  ") + APP_VERSION), 28, 516, 340, 16, fSmall, Cc(150, 156, 150));
    DrawT(g, WS(APP_COPYRIGHT), 28, 534, 360, 16, fSmall, Cc(110, 116, 110));

    // ---- right column ----
    SectionLabel(g, L"EQUALIZER", 412, 90, 340);
    for (int i = 0; i < 3; ++i) DrawSlider(g, p, 412, 116.f + i * 48, 340, i);
    SectionLabel(g, L"EFFECTS", 412, 272, 340);
    for (int i = 0; i < 4; ++i) DrawSlider(g, p, 412, 298.f + i * 48, 340, 3 + i);

    // done button
    { REAL bx = W - 28 - 130, by = H - 60; float hv = Hv(p, 98); AddHit(p, bx, by, 130, 38, 98);
      GlowRR(g, NV(), bx, by, 130, 38, 19, 6, 24 + 40 * hv);
      LinearGradientBrush lb(PointF(bx, by), PointF(bx, by + 38), Lerp(Cc(138, 205, 10), Cc(160, 225, 30), hv), Lerp(Cc(98, 160, 0), Cc(118, 185, 0), hv));
      FillRR(g, lb, bx, by, 130, 38, 19);
      DrawT(g, L"DONE", bx, by, 130, 38, fBold, Cc(14, 20, 6), StringAlignmentCenter, StringAlignmentCenter); }

    // ---- dropdown overlay (drawn last so it sits on top) ----
    if (g_open) {
        const std::vector<std::wstring>& L = (g_open == 30) ? g_inNames : g_outNames;
        int cur = (g_open == 30 ? g_InDev : g_OutDev) + 1;
        REAL fx = 28, fy = (g_open == 30) ? 134.f : 200.f, fw = 340;
        int total = (int)L.size(), vis = total < 6 ? total : 6;
        if (g_scroll > total - vis) g_scroll = total - vis; if (g_scroll < 0) g_scroll = 0;
        REAL lh = vis * 30.f + 8;
        AddHit(p, 0, 0, W, H, 999);                                   // click-catcher: closes the list
        GlowRR(g, Cc(0, 0, 0), fx, fy + 40, fw, lh, 10, 7, 150);
        SolidBrush bg(Cc(23, 26, 23)); FillRR(g, bg, fx, fy + 40, fw, lh, 10);
        StrokeRR(g, NV(160), 1.f, fx, fy + 40, fw, lh, 10);
        for (int r = 0; r < vis; ++r) {
            int idx = g_scroll + r; int id = 1000 + idx; REAL iy = fy + 44 + r * 30;
            AddHit(p, fx + 4, iy, fw - 8, 30, id); float hv = Hv(p, id);
            if (hv > 0.01f) { SolidBrush hb(Cc(118, 185, 0, (int)(38 * hv))); FillRR(g, hb, fx + 4, iy, fw - 8, 30, 7); }
            bool sel = (idx == cur);
            if (sel) { SolidBrush dot(NV()); g.FillEllipse(&dot, fx + 14.f, iy + 12.f, 6.f, 6.f); }
            DrawT(g, L[idx], fx + 28, iy, fw - 44, 30, fBody, sel ? NV() : Cc(222, 226, 222), StringAlignmentNear, StringAlignmentCenter, true);
        }
        if (total > vis) {
            REAL th = (lh - 16) * vis / total, ty = fy + 48 + (lh - 16 - th) * g_scroll / (total - vis);
            SolidBrush sb(Cc(118, 185, 0, 120)); FillRR(g, sb, fx + fw - 8, ty, 3, th, 1.5f);
        }
    }
}

static void SliderFromX(int idx, float lx) {
    const SliderDef& d = g_Sliders[idx];
    float f = (lx - g_trk[idx].x) / g_trk[idx].w; if (f < 0) f = 0; if (f > 1) f = 1;
    int pos = (int)lroundf(d.mn + f * (d.mx - d.mn)); *d.field = pos / d.scale;
}
static void SettingsActivate(HWND hwnd, int id) {
    if (id == 30 || id == 31) {
        if (g_open == id) g_open = 0;
        else { g_open = id; int cur = (id == 30 ? g_InDev : g_OutDev) + 1; g_scroll = cur > 2 ? cur - 2 : 0; }
    } else if (id == 999) g_open = 0;
    else if (id >= 1000) {
        int idx = id - 1000;
        if (g_open == 30) g_InDev = idx - 1; else g_OutDev = idx - 1;
        g_open = 0; RestartIfRunning();
    } else if (id == 40) { g_Settings.startWithWindows = !g_Settings.startWithWindows; SetStartupWithWindows(g_Settings.startWithWindows); }
    else if (id == 41) g_Settings.minimizeToTray = !g_Settings.minimizeToTray;
    else if (id == 98 || id == 99) { DestroyWindow(hwnd); return; }
    InvalidateRect(hwnd, NULL, FALSE);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Pane& p = g_setPane;
    switch (msg) {
    case WM_CREATE: StyleWindow(hwnd); BuildDeviceLists(); g_open = 0; g_drag = -1; return 0;
    case WM_PAINT: PaintPane(hwnd, p, DrawSettings); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        TrackLeave(hwnd, p);
        if (g_drag >= 0) { SliderFromX(g_drag, GET_X_LPARAM(lp) / g_scale); InvalidateRect(hwnd, NULL, FALSE); return 0; }
        int h = HitTest(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h != p.hover) { p.hover = h; InvalidateRect(hwnd, NULL, FALSE); } return 0; }
    case WM_MOUSELEAVE: p.hover = -1; p.tracking = false; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(NULL, (p.hover >= 0 && p.hover != 999) || g_drag >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: {
        int id = HitTest(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); p.down = id;
        if (id >= 50 && id < 57) { g_drag = id - 50; SetCapture(hwnd); SliderFromX(g_drag, GET_X_LPARAM(lp) / g_scale); InvalidateRect(hwnd, NULL, FALSE); }
        return 0; }
    case WM_LBUTTONUP: {
        if (g_drag >= 0) { g_drag = -1; ReleaseCapture(); p.down = -1; InvalidateRect(hwnd, NULL, FALSE); return 0; }
        int id = HitTest(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (id >= 0 && id == p.down) { p.down = -1; SettingsActivate(hwnd, id); return 0; }
        p.down = -1; return 0; }
    case WM_CAPTURECHANGED: g_drag = -1; return 0;
    case WM_MOUSEWHEEL:
        if (g_open) { g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { if (g_open) { g_open = 0; InvalidateRect(hwnd, NULL, FALSE); } else DestroyWindow(hwnd); }
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: hSettingsWnd = NULL; g_open = 0; FreePane(g_setPane); SaveSettings(); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void OpenSettings() {
    if (hSettingsWnd) { SetForegroundWindow(hSettingsWnd); return; }
    RECT r; GetWindowRect(hMainWnd, &r);
    hSettingsWnd = MakeWindow("ApeSettingsClass", APP_NAME " - Settings", SET_W, SET_H, hMainWnd, r.left + 40, r.top + 40);
    ShowWindow(hSettingsWnd, SW_SHOW); SetFocus(hSettingsWnd);
}

// ============================================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (!verifyNvidiaGPU(g_GpuName, sizeof(g_GpuName))) {
        MessageBoxA(NULL,
            "CRITICAL HARDWARE ERROR:\nThis application requires a dedicated NVIDIA GPU to run.\n"
            "No compatible NVIDIA CUDA compute device was detected on this system.",
            "NVIDIA APE - Hardware Error", MB_ICONERROR | MB_OK);
        return -1;
    }
    SetProcessDPIAware();
    { HDC dc = GetDC(NULL); g_scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f; ReleaseDC(NULL, dc); }
    GdiplusStartupInput gsi; GdiplusStartup(&g_gdipToken, &gsi, NULL);
    InitFonts();
    LoadSettings();

    HICON icon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TRAY_ICON));
    HBRUSH bg = CreateSolidBrush(RGB(14, 15, 14));
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, WndProc, 0, 0, hInstance, icon, LoadCursor(NULL, IDC_ARROW), bg, NULL, "NvidiaAudioEngineClass", icon };
    RegisterClassExA(&wc);
    WNDCLASSEXA ws = { sizeof(WNDCLASSEXA), 0, SettingsProc, 0, 0, hInstance, icon, LoadCursor(NULL, IDC_ARROW), bg, NULL, "ApeSettingsClass", icon };
    RegisterClassExA(&ws);

    hMainWnd = MakeWindow("NvidiaAudioEngineClass", APP_NAME " - " APP_NAME_FULL, MAIN_W, MAIN_H, NULL, 100, 60);

    bool startHidden = lpCmdLine && strstr(lpCmdLine, "--tray") != NULL;
    if (startHidden) { g_IsMinimizedToTray = true; ShowWindow(hMainWnd, SW_HIDE); StartEngine(); }
    else { ShowWindow(hMainWnd, nCmdShow); UpdateWindow(hMainWnd); }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }

    UnregisterClassA("NvidiaAudioEngineClass", hInstance);
    GdiplusShutdown(g_gdipToken);
    return 0;
}
