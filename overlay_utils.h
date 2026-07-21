#pragma once
#include <d3d11.h>
#include <shellapi.h>
#include <windows.h>

#include "imgui.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001
#define ID_TRAY_SETTINGS 1002

extern ID3D11Device* g_pd3dDevice;
extern ID3D11DeviceContext* g_pd3dDeviceContext;
extern IDXGISwapChain* g_pSwapChain;
extern ID3D11RenderTargetView* g_mainRenderTargetView;
extern bool showSettingsWindow;

inline NOTIFYICONDATAW g_NotifyIconData = {0};

inline void CreateTrayIcon(HWND hWnd) {
  g_NotifyIconData.cbSize = sizeof(NOTIFYICONDATAW);
  g_NotifyIconData.hWnd = hWnd;
  g_NotifyIconData.uID = 1;
  g_NotifyIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_NotifyIconData.uCallbackMessage = WM_TRAYICON;
  g_NotifyIconData.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_SHIELD);

  wcscpy_s(g_NotifyIconData.szTip, L"musml Diablo 4 Tool");
  Shell_NotifyIconW(NIM_ADD, &g_NotifyIconData);
}

inline void DeleteTrayIcon() {
  Shell_NotifyIconW(NIM_DELETE, &g_NotifyIconData);
}

inline void CreateRenderTarget() {
  ID3D11Texture2D* pBackBuffer;
  g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
  g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr,
                                       &g_mainRenderTargetView);
  pBackBuffer->Release();
}

inline void CleanupRenderTarget() {
  if (g_mainRenderTargetView) {
    g_mainRenderTargetView->Release();
    g_mainRenderTargetView = nullptr;
  }
}

inline bool CreateDeviceD3D(HWND hWnd) {
  DXGI_SWAP_CHAIN_DESC sd;
  ZeroMemory(&sd, sizeof(sd));
  sd.BufferCount = 1;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hWnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  D3D_FEATURE_LEVEL featureLevel;
  const D3D_FEATURE_LEVEL featureLevelArray = {D3D_FEATURE_LEVEL_11_0,
                                               D3D_FEATURE_LEVEL_10_0};
  HRESULT res = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevelArray, 2,
      D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel,
      &g_pd3dDeviceContext);
  if (res == DXGI_ERROR_UNSUPPORTED)
    res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel,
        &g_pd3dDeviceContext);
  if (res != S_OK) return false;
  CreateRenderTarget();
  return true;
}

inline void CleanupDeviceD3D() {
  CleanupRenderTarget();
  if (g_pSwapChain) {
    g_pSwapChain->Release();
    g_pSwapChain = nullptr;
  }
  if (g_pd3dDeviceContext) {
    g_pd3dDeviceContext->Release();
    g_pd3dDeviceContext = nullptr;
  }
  if (g_pd3dDevice) {
    g_pd3dDevice->Release();
    g_pd3dDevice = nullptr;
  }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

inline LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam,
                              LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
  switch (msg) {
    case WM_SIZE:
      if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
        CleanupRenderTarget();
        g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam),
                                    (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN,
                                    0);
        CreateRenderTarget();
      }
      return 0;

    case WM_TRAYICON:
      if (lParam == WM_RBUTTONUP) {
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"Open Settings");
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

        POINT p;
        GetCursorPos(&p);
        SetForegroundWindow(hWnd);
        TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, p.x, p.y, 0,
                       hWnd, nullptr);
        DestroyMenu(hMenu);
      } else if (lParam == WM_LBUTTONDBLCLK) {
        showSettingsWindow = !showSettingsWindow;
      }
      return 0;

    case WM_COMMAND:
      if (LOWORD(wParam) == ID_TRAY_EXIT) {
        DeleteTrayIcon();
        PostQuitMessage(0);
        ExitProcess(0);
      } else if (LOWORD(wParam) == ID_TRAY_SETTINGS) {
        showSettingsWindow = true;
      }
      return 0;

    case WM_SYSCOMMAND:
      if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
      break;
    case WM_DESTROY:
      DeleteTrayIcon();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hWnd, msg, wParam, lParam);
}
