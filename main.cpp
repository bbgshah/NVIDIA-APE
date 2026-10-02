#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <string>
#include "resource.h"
#include "audio_engine.h"

NOTIFYICONDATA nid = {};
HWND hMainWnd = NULL;
EngineSettings g_Settings;
bool g_IsMinimizedToTray = false;
char g_GpuName[256] = "Checking...";

void SetStartupWithWindows(bool enable) {
    HKEY hKey;
    const char* czSubKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExA(HKEY_CURRENT_USER, czSubKey, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            char szPath[MAX_PATH];
            GetModuleFileNameA(NULL, szPath, MAX_PATH);
            RegSetValueExA(hKey, APP_NAME, 0, REG_SZ, (BYTE*)szPath, (DWORD)(strlen(szPath) + 1));
        } else {
            RegDeleteValueA(hKey, APP_NAME);
        }
        RegCloseKey(hKey);
    }
}

void AddSystemTrayIcon(HWND hwnd) {
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = IDI_TRAY_ICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_TRAY_ICON));
    lstrcpyA(nid.szTip, "NVIDIA APE (Audio Processing Engine)");
    Shell_NotifyIconA(NIM_ADD, &nid);
}

void RemoveSystemTrayIcon() {
    Shell_NotifyIconA(NIM_DELETE, &nid);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        AddSystemTrayIcon(hwnd);
        break;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            g_IsMinimizedToTray = false;
        } else if (lParam == WM_RBUTTONUP) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuA(hMenu, MF_STRING, ID_TRAY_RESTORE, "Open Control Panel");
            AppendMenuA(hMenu, MF_STRING, ID_TRAY_EXIT, "Exit");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_RESTORE) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            g_IsMinimizedToTray = false;
        } else if (LOWORD(wParam) == ID_TRAY_EXIT) {
            RemoveSystemTrayIcon();
            PostQuitMessage(0);
        }
        break;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED && g_Settings.minimizeToTray) {
            ShowWindow(hwnd, SW_HIDE);
            g_IsMinimizedToTray = true;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        
        HBRUSH bgBrush = CreateSolidBrush(RGB(17, 17, 17));
        FillRect(hdc, &ps.rcPaint, bgBrush);
        DeleteObject(bgBrush);

        SetBkMode(hdc, TRANSPARENT);

        HFONT hFontTitle = CreateFontA(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
        SelectObject(hdc, hFontTitle);
        SetTextColor(hdc, RGB(118, 185, 0));
        TextOutA(hdc, 20, 20, "NVIDIA APE", 10);
        DeleteObject(hFontTitle);

        HFONT hFontSubHeader = CreateFontA(13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
        SelectObject(hdc, hFontSubHeader);
        SetTextColor(hdc, RGB(150, 150, 150));
        TextOutA(hdc, 20, 46, "(NVIDIA Audio Processing Engine)", 32);
        DeleteObject(hFontSubHeader);

        HFONT hFontSub = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
        SelectObject(hdc, hFontSub);
        SetTextColor(hdc, RGB(200, 200, 200));
        
        std::string gpuText = "GPU Target: " + std::string(g_GpuName);
        TextOutA(hdc, 20, 75, gpuText.c_str(), (int)gpuText.length());

        TextOutA(hdc, 20, 110, "ACTIVE MODULE TOGGLES:", 22);
        SetTextColor(hdc, g_Settings.enableVoiceIsolation ? RGB(118, 185, 0) : RGB(100, 100, 100));
        TextOutA(hdc, 30, 135, "[ON] Tensor-Core AI Voice Isolation", 35);

        SetTextColor(hdc, g_Settings.enableParametricEQ ? RGB(118, 185, 0) : RGB(100, 100, 100));
        TextOutA(hdc, 30, 155, "[ON] Parallel Parametric EQ Matrix", 34);

        SetTextColor(hdc, g_Settings.enableSoftSaturation ? RGB(118, 185, 0) : RGB(100, 100, 100));
        TextOutA(hdc, 30, 175, "[ON] Hyperbolic Soft Saturation Wave-shaper", 42);

        SetTextColor(hdc, g_Settings.enableConvolutionReverb ? RGB(118, 185, 0) : RGB(100, 100, 100));
        TextOutA(hdc, 30, 195, "[OFF] 3D Spatial Audio & Convolution Reverb", 42);

        SetTextColor(hdc, RGB(220, 220, 220));
        TextOutA(hdc, 20, 240, "SETTINGS & ABOUT", 16);

        std::string authorStr = "Author: " + std::string(APP_AUTHOR);
        TextOutA(hdc, 30, 265, authorStr.c_str(), (int)authorStr.length());

        SetTextColor(hdc, RGB(128, 128, 128));
        std::string githubStr = "https://" + std::string(APP_GITHUB);
        TextOutA(hdc, 30, 285, githubStr.c_str(), (int)githubStr.length());

        SetTextColor(hdc, RGB(160, 160, 160));
        std::string verStr = "Version: " + std::string(APP_VERSION);
        TextOutA(hdc, 30, 310, verStr.c_str(), (int)verStr.length());
        
        TextOutA(hdc, 30, 330, APP_COPYRIGHT, (int)strlen(APP_COPYRIGHT));

        DeleteObject(hFontSub);
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DESTROY:
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
            "CRITICAL HARDWARE ERROR:\nThis application requires a dedicated NVIDIA GPU to run.\nNo compatible NVIDIA CUDA compute device was detected on this system.", 
            "NVIDIA APE - Hardware Error", 
            MB_ICONERROR | MB_OK);
        return -1;
    }

    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, WndProc, 0L, 0L, hInstance, 
        LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TRAY_ICON)), 
        LoadCursor(NULL, IDC_ARROW), 
        NULL, NULL, "NvidiaAudioEngineClass", 
        LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TRAY_ICON)) };
    RegisterClassExA(&wc);

    hMainWnd = CreateWindowExA(0, "NvidiaAudioEngineClass", APP_NAME " - " APP_NAME_FULL, 
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 
        100, 100, 520, 420, NULL, NULL, hInstance, NULL);

    ShowWindow(hMainWnd, nCmdShow);
    UpdateWindow(hMainWnd);

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));

    while (msg.message != WM_QUIT) {
        if (PeekMessageA(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        } else {
            if (g_IsMinimizedToTray) {
                Sleep(20);
            } else {
                Sleep(10);
            }
        }
    }

    UnregisterClassA("NvidiaAudioEngineClass", hInstance);
    return 0;
}