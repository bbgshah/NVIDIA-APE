#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <mmsystem.h>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "resource.h"
#include "audio_engine.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

// ---- control IDs (high values so they cannot clash with resource.h) --------
enum {
    IDC_BTN_START = 3001, IDC_BTN_SETTINGS, IDC_CHK_EQ, IDC_CHK_GATE, IDC_CHK_SAT,
    IDC_CHK_REV, IDC_METER, IDC_STATUS, IDC_GITHUB,
    IDC_COMBO_IN = 3101, IDC_COMBO_OUT, IDC_CHK_STARTUP, IDC_CHK_TRAY, IDC_BTN_CLOSE,
    IDC_SLIDER0 = 3200,   // + index
    IDC_SLABEL0 = 3300    // + index
};
#define TIMER_METER 1

static const COLORREF NV_GREEN = RGB(118, 185, 0);
static const COLORREF BG_COL   = RGB(17, 17, 17);

// ---- globals ---------------------------------------------------------------
NOTIFYICONDATA nid = {};
HWND hMainWnd = NULL, hSettingsWnd = NULL, hMeter = NULL, hStartBtn = NULL, hStatus = NULL;
EngineSettings g_Settings;
bool g_IsMinimizedToTray = false;
char g_GpuName[256] = "Checking...";
int  g_InDev = -1, g_OutDev = -1;          // -1 = system default (WAVE_MAPPER)
HFONT g_FontUI, g_FontTitle, g_FontSub;
HBRUSH g_BgBrush;
float g_MeterDisp = 0.f;

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

                bool ok = gpuProcessBlock(in.data(), out.data(), BLOCK, &s, g0, gate);

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

                waveInAddBuffer(hIn, &h, sizeof(WAVEHDR));
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
//  UI helpers
// ============================================================================
static HWND Mk(HWND parent, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
    if (g_FontUI) SendMessageA(c, WM_SETFONT, (WPARAM)g_FontUI, TRUE);
    return c;
}
static void Txt(HDC dc, int x, int y, const std::string& s) { TextOutA(dc, x, y, s.c_str(), (int)s.size()); }

static void UpdateStartUI() {
    bool r = g_Engine.Running();
    SetWindowTextA(hStartBtn, r ? "Stop Processing" : "Start Processing");
    SetWindowTextA(hStatus, r ? "Processing  (48 kHz mono)" : "Stopped");
    InvalidateRect(hStatus, NULL, TRUE);
}
static bool StartEngine() {
    if (!g_Engine.Start(g_InDev, g_OutDev)) {
        MessageBoxA(hMainWnd, g_Engine.err.c_str(), "NVIDIA APE", MB_ICONERROR);
        UpdateStartUI(); return false;
    }
    UpdateStartUI(); return true;
}
static void RestartIfRunning() {
    if (g_Engine.Running()) { g_Engine.Stop(); StartEngine(); }
}

// ============================================================================
//  Settings window (separate top-level window)
// ============================================================================
struct SliderDef { const char* name; float* field; int mn, mx; float scale, dispDiv; const char* fmt; };
static SliderDef g_Sliders[] = {
    { "Low  (below 250 Hz)",     &g_Settings.lowGainDb,       -12,  12,   1,   1, "%+.0f dB" },
    { "Mid  (250 Hz - 4 kHz)",   &g_Settings.midGainDb,       -12,  12,   1,   1, "%+.0f dB" },
    { "High (above 4 kHz)",      &g_Settings.highGainDb,      -12,  12,   1,   1, "%+.0f dB" },
    { "Saturation drive",        &g_Settings.saturationDrive,  10, 100,  10,  10, "%.1f x"   },
    { "Noise gate threshold",    &g_Settings.gateThresholdDb, -70, -20,   1,   1, "%.0f dB"  },
    { "Reverb mix",              &g_Settings.reverbMix,         0, 100, 100,   1, "%.0f %%"  },
    { "Output gain",             &g_Settings.outputGainDb,    -12,  12,   1,   1, "%+.0f dB" },
};
static const int N_SLIDERS = sizeof(g_Sliders) / sizeof(g_Sliders[0]);

static void FillDevices(HWND combo, bool input, int sel) {
    SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"System default device");
    UINT n = input ? waveInGetNumDevs() : waveOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        char name[64] = "Unknown device";
        if (input) { WAVEINCAPSA c;  if (waveInGetDevCapsA(i, &c, sizeof(c)) == MMSYSERR_NOERROR)  lstrcpynA(name, c.szPname, 64); }
        else       { WAVEOUTCAPSA c; if (waveOutGetDevCapsA(i, &c, sizeof(c)) == MMSYSERR_NOERROR) lstrcpynA(name, c.szPname, 64); }
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)name);
    }
    SendMessageA(combo, CB_SETCURSEL, (WPARAM)(sel + 1), 0);
}
static void SetSliderLabel(HWND wnd, int idx, int pos) {
    char b[48]; snprintf(b, 48, g_Sliders[idx].fmt, pos / g_Sliders[idx].dispDiv);
    SetDlgItemTextA(wnd, IDC_SLABEL0 + idx, b);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        Mk(hwnd, "STATIC", "Input device (microphone)", 0, 20, 15, 420, 18, -1);
        HWND ci = Mk(hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 20, 35, 420, 200, IDC_COMBO_IN);
        Mk(hwnd, "STATIC", "Output device", 0, 20, 70, 420, 18, -1);
        HWND co = Mk(hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 20, 90, 420, 200, IDC_COMBO_OUT);
        FillDevices(ci, true, g_InDev);
        FillDevices(co, false, g_OutDev);
        Mk(hwnd, "STATIC", "Tip: to feed Discord/OBS, install VB-Cable, set Output to \"CABLE Input\", "
                           "then pick \"CABLE Output\" as the microphone in that app.", 0, 20, 122, 420, 40, -1);

        for (int i = 0; i < N_SLIDERS; ++i) {
            int y = 175 + i * 32;
            const SliderDef& d = g_Sliders[i];
            Mk(hwnd, "STATIC", d.name, 0, 20, y + 3, 130, 18, -1);
            HWND tb = Mk(hwnd, TRACKBAR_CLASSA, "", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 150, y, 210, 26, IDC_SLIDER0 + i);
            SendMessageA(tb, TBM_SETRANGE, TRUE, MAKELONG(d.mn, d.mx));
            int pos = (int)lroundf(*d.field * d.scale);
            SendMessageA(tb, TBM_SETPOS, TRUE, pos);
            Mk(hwnd, "STATIC", "", 0, 370, y + 3, 80, 18, IDC_SLABEL0 + i);
            SetSliderLabel(hwnd, i, pos);
        }
        HWND c1 = Mk(hwnd, "BUTTON", "Start with Windows (starts minimized, processing on)", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 410, 420, 22, IDC_CHK_STARTUP);
        HWND c2 = Mk(hwnd, "BUTTON", "Minimize to system tray", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 435, 420, 22, IDC_CHK_TRAY);
        SendMessageA(c1, BM_SETCHECK, g_Settings.startWithWindows ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageA(c2, BM_SETCHECK, g_Settings.minimizeToTray ? BST_CHECKED : BST_UNCHECKED, 0);
        Mk(hwnd, "BUTTON", "Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 350, 470, 90, 28, IDC_BTN_CLOSE);
        return 0;
    }
    case WM_HSCROLL: {
        HWND tb = (HWND)lParam;
        int idx = GetDlgCtrlID(tb) - IDC_SLIDER0;
        if (idx >= 0 && idx < N_SLIDERS) {
            int pos = (int)SendMessageA(tb, TBM_GETPOS, 0, 0);
            *g_Sliders[idx].field = pos / g_Sliders[idx].scale;   // live update
            SetSliderLabel(hwnd, idx, pos);
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        if (id == IDC_COMBO_IN && code == CBN_SELCHANGE) {
            g_InDev = (int)SendMessageA((HWND)lParam, CB_GETCURSEL, 0, 0) - 1; RestartIfRunning();
        } else if (id == IDC_COMBO_OUT && code == CBN_SELCHANGE) {
            g_OutDev = (int)SendMessageA((HWND)lParam, CB_GETCURSEL, 0, 0) - 1; RestartIfRunning();
        } else if (id == IDC_CHK_STARTUP) {
            g_Settings.startWithWindows = SendMessageA((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SetStartupWithWindows(g_Settings.startWithWindows);
        } else if (id == IDC_CHK_TRAY) {
            g_Settings.minimizeToTray = SendMessageA((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
        } else if (id == IDC_BTN_CLOSE) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: hSettingsWnd = NULL; SaveSettings(); return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void OpenSettings() {
    if (hSettingsWnd) { SetForegroundWindow(hSettingsWnd); return; }
    RECT r; GetWindowRect(hMainWnd, &r);
    hSettingsWnd = CreateWindowExA(0, "ApeSettingsClass", APP_NAME " - Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, r.left + 40, r.top + 40, 480, 570,
        hMainWnd, NULL, GetModuleHandle(NULL), NULL);
    ShowWindow(hSettingsWnd, SW_SHOW);
}

// ============================================================================
//  Main window
// ============================================================================
static void SetClassic(HWND h) { SetWindowTheme(h, L"", L""); }   // lets us colour it dark

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        AddSystemTrayIcon(hwnd);
        hStartBtn = Mk(hwnd, "BUTTON", "Start Processing", BS_PUSHBUTTON | WS_TABSTOP, 20, 105, 150, 32, IDC_BTN_START);
        Mk(hwnd, "BUTTON", "Settings...", BS_PUSHBUTTON | WS_TABSTOP, 180, 105, 110, 32, IDC_BTN_SETTINGS);
        hStatus = Mk(hwnd, "STATIC", "Stopped", 0, 305, 113, 200, 20, IDC_STATUS);

        struct { const char* t; int id; bool on; } chk[] = {
            { "Parametric EQ (3-band, GPU FIR)",              IDC_CHK_EQ,   g_Settings.enableEQ },
            { "Noise Gate / Voice Isolation",                 IDC_CHK_GATE, g_Settings.enableNoiseGate },
            { "Soft Saturation (tanh wave-shaper)",           IDC_CHK_SAT,  g_Settings.enableSaturation },
            { "Reverb (GPU convolution)",                     IDC_CHK_REV,  g_Settings.enableReverb },
        };
        for (int i = 0; i < 4; ++i) {
            HWND c = Mk(hwnd, "BUTTON", chk[i].t, BS_AUTOCHECKBOX | WS_TABSTOP, 30, 168 + i * 26, 440, 22, chk[i].id);
            SetClassic(c);
            SendMessageA(c, BM_SETCHECK, chk[i].on ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        hMeter = Mk(hwnd, PROGRESS_CLASSA, "", 0, 20, 300, 460, 12, IDC_METER);
        SetClassic(hMeter);
        SendMessageA(hMeter, PBM_SETBARCOLOR, 0, (LPARAM)NV_GREEN);
        SendMessageA(hMeter, PBM_SETBKCOLOR, 0, (LPARAM)RGB(45, 45, 45));
        SendMessageA(hMeter, PBM_SETRANGE32, 0, 100);
        Mk(hwnd, "STATIC", "https://" APP_GITHUB, SS_NOTIFY, 30, 372, 300, 18, IDC_GITHUB);
        SetTimer(hwnd, TIMER_METER, 50, NULL);
        break;
    }

    case WM_TIMER: {
        float pk = g_Settings.livePeakLevel;
        g_MeterDisp = (pk > g_MeterDisp) ? pk : g_MeterDisp * 0.85f;
        float db = 20.0f * log10f(g_MeterDisp + 1e-6f);
        int pos = (int)((db + 60.0f) / 60.0f * 100.0f);
        if (!g_Engine.Running() || pos < 0) pos = 0;
        if (pos > 100) pos = 100;
        SendMessageA(hMeter, PBM_SETPOS, pos, 0);
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wParam; int id = GetDlgCtrlID((HWND)lParam);
        if (msg == WM_CTLCOLORBTN && id != IDC_CHK_EQ && id != IDC_CHK_GATE && id != IDC_CHK_SAT && id != IDC_CHK_REV)
            return DefWindowProcA(hwnd, msg, wParam, lParam);
        SetBkColor(dc, BG_COL);
        COLORREF col = RGB(220, 220, 220);
        if (id == IDC_GITHUB) col = NV_GREEN;
        else if (id == IDC_STATUS) col = g_Engine.Running() ? NV_GREEN : RGB(140, 140, 140);
        SetTextColor(dc, col);
        return (LRESULT)g_BgBrush;
    }

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); g_IsMinimizedToTray = false;
        } else if (lParam == WM_RBUTTONUP) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuA(hMenu, MF_STRING, ID_TRAY_RESTORE, "Open Control Panel");
            AppendMenuA(hMenu, MF_STRING, ID_TRAY_EXIT, "Exit");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case ID_TRAY_RESTORE:
            ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); g_IsMinimizedToTray = false; break;
        case ID_TRAY_EXIT:
            DestroyWindow(hwnd); break;
        case IDC_BTN_START:
            if (g_Engine.Running()) { g_Engine.Stop(); g_Settings.livePeakLevel = 0; UpdateStartUI(); }
            else StartEngine();
            break;
        case IDC_BTN_SETTINGS: OpenSettings(); break;
        case IDC_GITHUB: ShellExecuteA(hwnd, "open", "https://" APP_GITHUB, NULL, NULL, SW_SHOWNORMAL); break;
        case IDC_CHK_EQ: case IDC_CHK_GATE: case IDC_CHK_SAT: case IDC_CHK_REV: {
            bool on = SendMessageA((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (id == IDC_CHK_EQ)   g_Settings.enableEQ = on;
            if (id == IDC_CHK_GATE) g_Settings.enableNoiseGate = on;
            if (id == IDC_CHK_SAT)  g_Settings.enableSaturation = on;
            if (id == IDC_CHK_REV)  g_Settings.enableReverb = on;
            break;
        }
        }
        break;
    }

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED && g_Settings.minimizeToTray) {
            ShowWindow(hwnd, SW_HIDE); g_IsMinimizedToTray = true;
        }
        break;

    case WM_ERASEBKGND: return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        FillRect(hdc, &ps.rcPaint, g_BgBrush);
        SetBkMode(hdc, TRANSPARENT);

        SelectObject(hdc, g_FontTitle); SetTextColor(hdc, NV_GREEN);
        Txt(hdc, 20, 15, "NVIDIA APE");
        SelectObject(hdc, g_FontSub);   SetTextColor(hdc, RGB(150, 150, 150));
        Txt(hdc, 20, 43, "(NVIDIA Audio Processing Engine)");
        SelectObject(hdc, g_FontUI);    SetTextColor(hdc, RGB(200, 200, 200));
        Txt(hdc, 20, 72, std::string("GPU Target: ") + g_GpuName);

        SetTextColor(hdc, RGB(220, 220, 220));
        Txt(hdc, 20, 145, "ACTIVE MODULES:");
        Txt(hdc, 20, 278, "OUTPUT LEVEL:");
        Txt(hdc, 20, 335, "ABOUT");
        SetTextColor(hdc, RGB(200, 200, 200));
        Txt(hdc, 30, 355, std::string("Author: ") + APP_AUTHOR);
        SetTextColor(hdc, RGB(160, 160, 160));
        Txt(hdc, 30, 395, std::string("Version: ") + APP_VERSION);
        Txt(hdc, 30, 415, APP_COPYRIGHT);
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_METER);
        g_Engine.Stop();
        gpuEngineShutdown();
        SaveSettings();
        RemoveSystemTrayIcon();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (!verifyNvidiaGPU(g_GpuName, sizeof(g_GpuName))) {
        MessageBoxA(NULL,
            "CRITICAL HARDWARE ERROR:\nThis application requires a dedicated NVIDIA GPU to run.\n"
            "No compatible NVIDIA CUDA compute device was detected on this system.",
            "NVIDIA APE - Hardware Error", MB_ICONERROR | MB_OK);
        return -1;
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    LoadSettings();

    NONCLIENTMETRICSA ncm = {}; ncm.cbSize = sizeof(ncm);
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_FontUI    = CreateFontIndirectA(&ncm.lfMessageFont);
    g_FontTitle = CreateFontA(26, 0, 0, 0, FW_BOLD,     FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    g_FontSub   = CreateFontA(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    g_BgBrush   = CreateSolidBrush(BG_COL);

    HICON icon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TRAY_ICON));
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, WndProc, 0, 0, hInstance, icon,
                       LoadCursor(NULL, IDC_ARROW), NULL, NULL, "NvidiaAudioEngineClass", icon };
    RegisterClassExA(&wc);
    WNDCLASSEXA ws = { sizeof(WNDCLASSEXA), 0, SettingsProc, 0, 0, hInstance, icon,
                       LoadCursor(NULL, IDC_ARROW), (HBRUSH)(COLOR_BTNFACE + 1), NULL, "ApeSettingsClass", icon };
    RegisterClassExA(&ws);

    hMainWnd = CreateWindowExA(0, "NvidiaAudioEngineClass", APP_NAME " - " APP_NAME_FULL,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        100, 100, 520, 490, NULL, NULL, hInstance, NULL);

    bool startHidden = lpCmdLine && strstr(lpCmdLine, "--tray") != NULL;
    if (startHidden) { g_IsMinimizedToTray = true; ShowWindow(hMainWnd, SW_HIDE); StartEngine(); }
    else { ShowWindow(hMainWnd, nCmdShow); UpdateWindow(hMainWnd); }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    UnregisterClassA("NvidiaAudioEngineClass", hInstance);
    return 0;
}
