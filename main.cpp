#include <windows.h>
#include <d3d11.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "overlay_utils.h"

struct SpamKey { int vKey; std::string keyName; int delayMs; std::chrono::steady_clock::time_point lastPressed; };
struct LocStrings {
    std::string gameStatus, scriptStatus, healthStatus, options, healthy, lowHp;
    std::string captureCoordsTitle, captureCoordsDesc, captureKeyTitle, captureKeyDesc;
    std::string settingsTitle, btnToggle, btnSettings, combatCondition, radioLmb, radioRmb;
    std::string chkGlobalHealth, lblHealthKey, lblHealTimer, btnPickCoords, lblSpamList, btnDelete, btnAddKey;
};

extern LocStrings lang;
extern std::atomic<bool> isScriptActive; extern std::atomic<bool> isHealthy;
extern bool showSettingsWindow; extern bool isCapturing; extern bool isCapturingCoordinates;
extern std::vector<SpamKey> spamKeys; extern int combatMouseTrigger;
extern bool globalHealthCheckEnable; extern int healthVKey; extern std::string healthKeyName;
extern int healthDelayMs; extern int healthX; extern int healthY;
extern std::string toggleKeyName; extern std::string settingsKeyName; extern int keyToCaptureType;

extern void LoadConfig(); extern void SaveConfig(); extern void CoreMacroLoop();
extern void GlobalHotkeyMonitor(); extern bool IsDiabloActive(); extern void LoadLanguage();

ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    LoadLanguage(); // Теперь язык определяется автоматически на основе вашей Windows
    LoadConfig();

    std::thread(CoreMacroLoop).detach(); std::thread(GlobalHotkeyMonitor).detach();

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"OverlayClass", nullptr };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"OverlayClass", L"musml Overlay", WS_POPUP, 50, 50, 750, 400, nullptr, nullptr, hInstance, nullptr);
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    if (!CreateDeviceD3D(hwnd)) { CleanupDeviceD3D(); UnregisterClassW(L"OverlayClass", hInstance); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd);

    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle(); style.WindowBorderSize = 1.0f; style.WindowRounding = 0.0f;

    // --- ПОДКЛЮЧЕНИЕ РУССКОГО ШРИФТА ДЛЯ IMGUI ---
    ImGuiIO& io = ImGui::GetIO();
    char winFolder[MAX_PATH];
    GetWindowsDirectoryA(winFolder, MAX_PATH);
    std::string fontPath = std::string(winFolder) + "\\Fonts\\Arial.ttf"; // Берем стандартный шрифт Windows

    // Загружаем шрифт и принудительно указываем диапазон кириллицы (GetGlyphRangesCyrillic)
    io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());

    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
        if (showSettingsWindow || isCapturing || isCapturingCoordinates) {
            if (exStyle & WS_EX_TRANSPARENT) SetWindowLong(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
        }
        else {
            if (!(exStyle & WS_EX_TRANSPARENT)) SetWindowLong(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
        }

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always); ImGui::SetNextWindowSize(ImVec2(240, 100), ImGuiCond_Always);
        ImGui::Begin("StatusPanel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        ImGui::Text(lang.gameStatus.c_str()); ImGui::SameLine();
        if (IsDiabloActive()) ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "ON");
        else ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "OFF");

        ImGui::Text(lang.scriptStatus.c_str()); ImGui::SameLine();
        if (isScriptActive) ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), ("ON (" + toggleKeyName + ")").c_str());
        else ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), ("OFF (" + toggleKeyName + ")").c_str());

        ImGui::Text(lang.healthStatus.c_str()); ImGui::SameLine();
        if (!IsDiabloActive()) ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "-");
        else if (isHealthy) ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), lang.healthy.c_str());
        else ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), lang.lowHp.c_str());

        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), (lang.options + "[" + settingsKeyName + "]").c_str());
        ImGui::End();

        if (isCapturingCoordinates) {
            ImGui::SetNextWindowPos(ImVec2(245, 0), ImGuiCond_Always); ImGui::SetNextWindowSize(ImVec2(460, 100), ImGuiCond_Always);
            ImGui::Begin("Capture Coords", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), lang.captureCoordsTitle.c_str());
            ImGui::Text(lang.captureCoordsDesc.c_str()); ImGui::End();
        }
        else if (isCapturing) {
            ImGui::SetNextWindowPos(ImVec2(245, 0), ImGuiCond_Always); ImGui::SetNextWindowSize(ImVec2(460, 100), ImGuiCond_Always);
            ImGui::Begin("Capture Key", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), lang.captureKeyTitle.c_str());
            ImGui::Text(lang.captureKeyDesc.c_str()); ImGui::End();
        }
        else if (showSettingsWindow) {
            ImGui::SetNextWindowPos(ImVec2(245, 0), ImGuiCond_Always); ImGui::SetNextWindowSize(ImVec2(460, 360), ImGuiCond_Always);
            ImGui::Begin("Settings Panel", &showSettingsWindow, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

            ImGui::Text(lang.settingsTitle.c_str());
            if (ImGui::Button((lang.btnToggle + "[" + toggleKeyName + "]").c_str())) { isCapturing = true; keyToCaptureType = 0; } ImGui::SameLine();
            if (ImGui::Button((lang.btnSettings + "[" + settingsKeyName + "]").c_str())) { isCapturing = true; keyToCaptureType = 1; }

            ImGui::Separator(); ImGui::Text(lang.combatCondition.c_str());
            ImGui::RadioButton(lang.radioLmb.c_str(), &combatMouseTrigger, 0); ImGui::SameLine();
            ImGui::RadioButton(lang.radioRmb.c_str(), &combatMouseTrigger, 1);
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveConfig();

            ImGui::Separator(); ImGui::Checkbox(lang.chkGlobalHealth.c_str(), &globalHealthCheckEnable);
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveConfig();

            if (globalHealthCheckEnable) {
                if (ImGui::Button((lang.lblHealthKey + "[" + healthKeyName + "]").c_str())) { isCapturing = true; keyToCaptureType = 2; } ImGui::SameLine();
                ImGui::PushItemWidth(100);
                if (ImGui::InputInt(lang.lblHealTimer.c_str(), &healthDelayMs, 0, 0)) { if (healthDelayMs < 1) healthDelayMs = 1; SaveConfig(); }
                ImGui::PopItemWidth();

                std::string coordsLabel = lang.btnPickCoords + " (X:" + std::to_string(healthX) + " Y:" + std::to_string(healthY) + ")";
                if (ImGui::Button(coordsLabel.c_str())) isCapturingCoordinates = true;
            }

            ImGui::Separator(); ImGui::Text(lang.lblSpamList.c_str());
            for (size_t i = 0; i < spamKeys.size(); i++) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Button(("[" + spamKeys[i].keyName + "]##btn").c_str(), ImVec2(80, 0))) { isCapturing = true; keyToCaptureType = static_cast<int>(3 + i); } ImGui::SameLine();
                ImGui::PushItemWidth(80); if (ImGui::InputInt("мс##del", &spamKeys[i].delayMs, 0, 0)) { if (spamKeys[i].delayMs < 1) spamKeys[i].delayMs = 1; SaveConfig(); } ImGui::PopItemWidth(); ImGui::SameLine();
                if (ImGui::Button(lang.btnDelete.c_str())) { spamKeys.erase(spamKeys.begin() + i); SaveConfig(); ImGui::PopID(); break; }
                ImGui::PopID();
            }
            if (ImGui::Button(lang.btnAddKey.c_str())) { spamKeys.push_back({ '1', "1", 100 }); SaveConfig(); }
            ImGui::End();
        }

        ImGui::Render();
        const float clear_color_with_alpha[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    CleanupDeviceD3D(); DestroyWindow(hwnd); UnregisterClassW(L"OverlayClass", hInstance);
    return 0;
}
