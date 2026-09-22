#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_state.h"
#include "event_schedule.h"
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_impl_dx11.h"
#include "third_party/imgui/imgui_impl_win32.h"
#include "overlay_utils.h"
#include "updater.h"
#include "version.h"

extern LocStrings lang;
extern std::atomic<bool> isScriptActive;
extern std::atomic<bool> isHealthy;
extern bool showSettingsWindow;
extern bool showEventsWindow;
extern bool hotkeyCaptureConflict;
extern bool isCapturing;
extern bool isCapturingCoordinates;
extern std::recursive_mutex settingsMutex;
extern std::vector<SpamKey> spamKeys;
extern int combatMouseTrigger;
extern bool globalHealthCheckEnable;
extern bool globalHealthIndependent;
extern int healthVKey;
extern std::string healthKeyName;
extern int healthDelayMs;
extern int healthX;
extern int healthY;
extern int fastLootHoldVKey;
extern std::string fastLootHoldKeyName;
extern int fastLootClickVKey;
extern std::string fastLootClickKeyName;
extern int fastLootDelayMs;
extern std::string toggleKeyName;
extern std::string settingsKeyName;
extern std::string eventsKeyName;
extern void BeginKeyCapture(int type);
extern std::vector<ProfileConfig> profiles;
extern int activeProfileIndex;
extern bool autoUpdateEnabled;

namespace {
	// HUD panels are free-floating now: their default size below only applies
	// on first use, afterwards the user's drag/resize layout is remembered.
	constexpr float kStatusWindowDefaultWidth = 340.0f;
	constexpr float kStatusWindowDefaultHeight = 114.0f;
	constexpr float kEventsWindowDefaultHeight = 105.0f;
	constexpr float kHudWindowMinWidth = 160.0f;
	constexpr float kHudWindowMinHeight = 50.0f;
	constexpr float kSettingsWindowInitialX = kStatusWindowDefaultWidth + 15.0f;
	constexpr float kSettingsWindowInitialY = 0.0f;
	constexpr float kSettingsWindowDefaultWidth = 560.0f;
	constexpr float kSettingsWindowDefaultHeight = 520.0f;
	constexpr float kSettingsWindowMaxWidth = 900.0f;
	constexpr float kSettingsWindowMaxHeight = 720.0f;

	char profileNameBuffer[64] = "";
	int lastProfileIndex = -1;
	bool updatePopupOpen = false;
	UpdatePhase lastUpdatePhase = UpdatePhase::Idle;
}  // namespace

extern std::string GetConfigPath();
extern void LoadConfig();
extern void SaveConfig();
extern void SelectProfile(int profileIndex);
extern void AddProfile();
extern void DeleteActiveProfile();
extern void CoreMacroLoop();
extern void GlobalHotkeyMonitor();
extern bool IsDiabloActive();
extern void LoadLanguage();

ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

namespace {
	// The HUD stays click-through while gaming; panels only become interactive
	// while the options window or a capture mode is active (inputActive).
	ImGuiWindowFlags HudWindowFlags(bool inputActive) {
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoFocusOnAppearing;
		if (!inputActive) flags |= ImGuiWindowFlags_NoInputs;
		return flags;
	}

	void DrawEventsWindow(const events::Timers& timers, bool inputActive,
		const ImVec2& statusPos, const ImVec2& statusSize,
		const ImVec2& overlayMax) {
		ImGui::SetNextWindowPos(
			ImVec2(statusPos.x, statusPos.y + statusSize.y + 6.0f),
			ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(
			ImVec2(statusSize.x, kEventsWindowDefaultHeight),
			ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(
			ImVec2(kHudWindowMinWidth, kHudWindowMinHeight), overlayMax);
		if (ImGui::Begin("EventsPanel", nullptr, HudWindowFlags(inputActive))) {
			ImGui::TextUnformatted(lang.eventsWindowTitle.c_str());
			ImGui::Separator();
			if (ImGui::BeginTable("##eventTimers", 2, ImGuiTableFlags_SizingFixedFit)) {
			auto row = [](const std::string& label, const events::Countdown& timer) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(label.c_str());
				ImGui::TableSetColumnIndex(1);
				std::string value;
				ImVec4 color(0.6f, 0.6f, 0.6f, 1.0f);
				if (timer.phase == events::Phase::Break) value = lang.eventsBreak;
				else {
					bool ending = timer.phase == events::Phase::EndsIn;
					value = (ending ? lang.eventsEndsIn : lang.eventsStartsIn) + " " +
						events::FormatDuration(timer.seconds, lang.eventsHours, lang.eventsMinutes);
					color = ending ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
						: ImVec4(0.0f, 0.8f, 1.0f, 1.0f);
				}
				ImGui::TextColored(color, "%s", value.c_str());
			};
			row(lang.eventWorldBoss, timers.worldBoss);
			row(lang.eventLegion, timers.legion);
			row(lang.eventHelltide, timers.helltide);
			ImGui::EndTable();
		}
	}
		ImGui::End();
	}

	// The updater relaunches us as: <exe> --updated-from <pid of old process>.
	// We must wait for that process to die before touching its leftovers,
	// otherwise the .bak file stays locked and the cleanup silently fails.
	unsigned long ParseUpdatedFromPid(LPSTR commandLine) {
		if (!commandLine) return 0;
		const char* marker = "--updated-from";
		const char* found = strstr(commandLine, marker);
		if (!found) return 0;
		found += strlen(marker);
		while (*found == ' ' || *found == '=') ++found;
		return strtoul(found, nullptr, 10);
	}
}  // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
	LPSTR lpCmdLine, int nCmdShow) {
	unsigned long previousPid = ParseUpdatedFromPid(lpCmdLine);
	if (previousPid != 0) WaitForPreviousInstance(previousPid);

	LoadLanguage();
	LoadConfig();
	CleanupUpdateArtifacts();
	if (autoUpdateEnabled) StartUpdateCheck();
	std::thread(CoreMacroLoop).detach();
	std::thread(GlobalHotkeyMonitor).detach();

	WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc,         0L,
					    0L,         hInstance,  nullptr,         nullptr,
					    nullptr,    nullptr,    L"OverlayClass", nullptr };
	RegisterClassExW(&wc);

	// Cover the whole virtual screen so the HUD panels can be moved and
	// stretched anywhere on any monitor; the color key keeps everything but
	// the panels invisible and click-through.
	RECT overlayRect{};
	overlayRect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
	overlayRect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
	overlayRect.right = overlayRect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
	overlayRect.bottom = overlayRect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
	if (overlayRect.right <= overlayRect.left ||
		overlayRect.bottom <= overlayRect.top) {
		overlayRect.left = 0;
		overlayRect.top = 0;
		overlayRect.right = GetSystemMetrics(SM_CXSCREEN);
		overlayRect.bottom = GetSystemMetrics(SM_CYSCREEN);
	}

	HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
		L"OverlayClass", L"Overlay", WS_POPUP, overlayRect.left, overlayRect.top,
		overlayRect.right - overlayRect.left,
		overlayRect.bottom - overlayRect.top, nullptr, nullptr,
		hInstance, nullptr);
	SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

	if (!CreateDeviceD3D(hwnd)) {
		CleanupDeviceD3D();
		UnregisterClassW(L"OverlayClass", hInstance);
		return 1;
	}
	ShowWindow(hwnd, SW_SHOWDEFAULT);
	UpdateWindow(hwnd);
	CreateTrayIcon(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowBorderSize = 1.0f;
	style.WindowRounding = 0.0f;

	ImGuiIO& io = ImGui::GetIO();

	// Remember the panel layout next to config.txt so it survives restarts
	// and does not depend on the directory the exe was launched from.
	std::string iniPath =
		std::filesystem::path(GetConfigPath()).parent_path().string() +
		"\\imgui.ini";
	{
		std::error_code ec;
		std::filesystem::path iniFile(iniPath);
		if (!std::filesystem::exists(iniFile, ec)) {
			// One-time migration of the legacy ./imgui.ini next to the exe.
			std::error_code legacyEc;
			std::filesystem::path legacy =
				std::filesystem::current_path(legacyEc) / "imgui.ini";
			if (!legacyEc && std::filesystem::exists(legacy, legacyEc) && !legacyEc)
				std::filesystem::copy_file(legacy, iniFile, legacyEc);
		}
	}
	io.IniFilename = iniPath.c_str();

	char winFolder[MAX_PATH];
	GetWindowsDirectoryA(winFolder, MAX_PATH);
	std::string fontPath = std::string(winFolder) + "\\Fonts\\Arial.ttf";
	io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, nullptr,
		io.Fonts->GetGlyphRangesCyrillic());

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
	bool settingsLayoutAdjusted = false;
	ImVec2 statusWindowPos(0.0f, 0.0f);
	ImVec2 statusWindowSize(kStatusWindowDefaultWidth, kStatusWindowDefaultHeight);
	bool done = false;
	while (!done) {
		MSG msg;
		while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			if (msg.message == WM_QUIT) done = true;
		}
		if (done) break;

		bool overlayNeedsInput = false;
		{
			std::lock_guard<std::recursive_mutex> lock(settingsMutex);
			overlayNeedsInput =
				showSettingsWindow || isCapturing || isCapturingCoordinates;
		}

		LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
		if (overlayNeedsInput) {
			if (exStyle & WS_EX_TRANSPARENT)
				SetWindowLong(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
		}
		else {
			if (!(exStyle & WS_EX_TRANSPARENT))
				SetWindowLong(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		ImVec2 overlayMax = ImGui::GetIO().DisplaySize;
		if (overlayMax.x < kHudWindowMinWidth) overlayMax.x = kHudWindowMinWidth;
		if (overlayMax.y < kHudWindowMinHeight) overlayMax.y = kHudWindowMinHeight;

		{
			std::lock_guard<std::recursive_mutex> lock(settingsMutex);

			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize(
				ImVec2(kStatusWindowDefaultWidth, kStatusWindowDefaultHeight),
				ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints(
				ImVec2(kHudWindowMinWidth, kHudWindowMinHeight), overlayMax);
			ImGui::Begin("StatusPanel", nullptr,
				HudWindowFlags(overlayNeedsInput));
			statusWindowPos = ImGui::GetWindowPos();
			statusWindowSize = ImGui::GetWindowSize();

			ImGui::Text(lang.gameStatus.c_str());
			ImGui::SameLine();
			if (IsDiabloActive())
				ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "ON");
			else
				ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "OFF");

			ImGui::Text(lang.scriptStatus.c_str());
			ImGui::SameLine();
			if (isScriptActive)
				ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
					("ON (" + toggleKeyName + ")").c_str());
			else
				ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
					("OFF (" + toggleKeyName + ")").c_str());

			ImGui::Text(lang.healthStatus.c_str());
			ImGui::SameLine();
			if (!IsDiabloActive())
				ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "-");
			else if (isHealthy)
				ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
					lang.healthy.c_str());
			else
				ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), lang.lowHp.c_str());

			if (!profiles.empty() && activeProfileIndex >= 0 &&
				activeProfileIndex < static_cast<int>(profiles.size())) {
				ImGui::Text(lang.profile.c_str());
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "%s",
					profiles[activeProfileIndex].name.c_str());
			}

			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s[%s]",
				lang.options.c_str(), settingsKeyName.c_str());
			ImGui::SameLine();
			ImGui::TextColored(showEventsWindow ? ImVec4(0.0f, 0.8f, 1.0f, 1.0f)
				: ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s [%s]",
				lang.events.c_str(), eventsKeyName.c_str());
			ImGui::End();

		if (showEventsWindow) {
			DrawEventsWindow(events::CalculateTimers(events::Now()), overlayNeedsInput,
				statusWindowPos, statusWindowSize, overlayMax);
		}

			if (isCapturingCoordinates) {
				ImGui::SetNextWindowPos(ImVec2(kSettingsWindowInitialX, 0), ImGuiCond_Always);
				ImGui::SetNextWindowSize(ImVec2(500, 100), ImGuiCond_Always);
				ImGui::Begin("Capture Coords", nullptr,
					ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
					ImGuiWindowFlags_NoMove);
				ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
					lang.captureCoordsTitle.c_str());
				ImGui::Text(lang.captureCoordsDesc.c_str());
				ImGui::End();
			}
			else if (isCapturing) {
				ImGui::SetNextWindowPos(ImVec2(kSettingsWindowInitialX, 0), ImGuiCond_Always);
				ImGui::SetNextWindowSize(ImVec2(500, 100), ImGuiCond_Always);
				ImGui::Begin("Capture Key", nullptr,
					ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
					ImGuiWindowFlags_NoMove);
				ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
					lang.captureKeyTitle.c_str());
				ImGui::TextUnformatted(lang.captureKeyDesc.c_str());
				if (hotkeyCaptureConflict) {
					ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s",
						lang.captureHotkeyConflict.c_str());
				}
				ImGui::End();
			}
			else if (showSettingsWindow) {
				ImGui::SetNextWindowPos(
					ImVec2(kSettingsWindowInitialX, kSettingsWindowInitialY),
					ImGuiCond_FirstUseEver);
				ImGui::SetNextWindowSize(
					ImVec2(kSettingsWindowDefaultWidth, kSettingsWindowDefaultHeight),
					ImGuiCond_FirstUseEver);
				ImGui::SetNextWindowSizeConstraints(
					ImVec2(460, 320),
					ImVec2(kSettingsWindowMaxWidth, kSettingsWindowMaxHeight));
				std::string settingsWindowTitle =
					lang.settingsWindowTitle + "###SettingsPanel";
				ImGui::Begin(settingsWindowTitle.c_str(), &showSettingsWindow,
					ImGuiWindowFlags_NoCollapse);
				if (!settingsLayoutAdjusted) {
					// Migrate the old default position so it cannot cover the wider HUD.
					const auto position = ImGui::GetWindowPos();
					if (position.x == 245.0f && position.y == 0.0f)
						ImGui::SetWindowPos(ImVec2(kSettingsWindowInitialX, 0));
					settingsLayoutAdjusted = true;
				}

				ImGui::Text(lang.settingsTitle.c_str());
				if (!profiles.empty()) {
					if (activeProfileIndex < 0 ||
						activeProfileIndex >= static_cast<int>(profiles.size())) {
						activeProfileIndex = 0;
					}

					ImGui::Text(lang.profile.c_str());
					ImGui::SameLine();

					ImGui::PushItemWidth(160);

					const char* currentProfile =
						profiles[activeProfileIndex].name.c_str();

					if (ImGui::BeginCombo("##profile", currentProfile)) {
						for (size_t i = 0; i < profiles.size(); ++i) {
							bool selected = activeProfileIndex == static_cast<int>(i);

							if (ImGui::Selectable(profiles[i].name.c_str(), selected))
								SelectProfile(static_cast<int>(i));

							if (selected) ImGui::SetItemDefaultFocus();
						}

						ImGui::EndCombo();
					}

					ImGui::PopItemWidth();

					if (lastProfileIndex != activeProfileIndex) {
						strcpy_s(profileNameBuffer, sizeof(profileNameBuffer),
							profiles[activeProfileIndex].name.c_str());

						lastProfileIndex = activeProfileIndex;
					}

					ImGui::PushItemWidth(160);

					if (ImGui::InputText("##ProfileName", profileNameBuffer,
						sizeof(profileNameBuffer))) {
						profiles[activeProfileIndex].name = profileNameBuffer;
						SaveConfig();
					}

					ImGui::PopItemWidth();

					ImGui::SameLine();

					if (ImGui::Button(lang.btnAddProfile.c_str())) AddProfile();
					ImGui::SameLine();
					if (ImGui::Button(lang.btnDeleteProfile.c_str()))
						DeleteActiveProfile();
				}

				ImGui::Separator();
				if (ImGui::Checkbox(lang.chkAutoUpdate.c_str(), &autoUpdateEnabled)) {
					SaveConfig();
					if (autoUpdateEnabled) StartUpdateCheck();
				}
				ImGui::SameLine();
				bool updateBusy = IsUpdateBusy();
				if (updateBusy) ImGui::BeginDisabled();
				if (ImGui::Button(lang.btnCheckUpdate.c_str())) StartUpdateCheck();
				if (updateBusy) ImGui::EndDisabled();

				UpdateStatus updateStatus = GetUpdateStatus();

				const std::string updatePopupTitle =
					lang.updatePopupTitle + "###UpdateAvailable";
				if (updateStatus.phase == UpdatePhase::Available &&
					lastUpdatePhase != UpdatePhase::Available &&
					lastUpdatePhase != UpdatePhase::Downloading &&
					lastUpdatePhase != UpdatePhase::Installing &&
					lastUpdatePhase != UpdatePhase::Restarting) {
					updatePopupOpen = true;
					ImGui::OpenPopup(updatePopupTitle.c_str());
				}
				lastUpdatePhase = updateStatus.phase;

				ImGui::SetNextWindowSize(ImVec2(400, 140), ImGuiCond_Always);
				if (ImGui::BeginPopupModal(updatePopupTitle.c_str(), &updatePopupOpen,
					ImGuiWindowFlags_NoResize)) {
					char promptBuf[256];
					snprintf(promptBuf, sizeof(promptBuf), lang.updatePrompt.c_str(),
						updateStatus.latestVersion.c_str());
					ImGui::TextWrapped("%s", promptBuf);
					ImGui::Separator();
					if (ImGui::Button(lang.btnUpdate.c_str(), ImVec2(100, 0))) {
						StartUpdateProcess();
						ImGui::CloseCurrentPopup();
						updatePopupOpen = false;
					}
					ImGui::SameLine();
					if (ImGui::Button(lang.btnLater.c_str(), ImVec2(100, 0))) {
						ImGui::CloseCurrentPopup();
						updatePopupOpen = false;
					}
					ImGui::SameLine();
					if (ImGui::Button(lang.btnOpenRelease.c_str(), ImVec2(160, 0))) {
						OpenLatestReleasePage();
					}
					ImGui::EndPopup();
				}

				ImGui::Text("%s %s", lang.updateCurrentVersion.c_str(),
					APP_VERSION_STR);
				ImGui::TextWrapped("%s %s", lang.updateStatus.c_str(),
					updateStatus.message.c_str());

				if (updateStatus.phase == UpdatePhase::Downloading &&
					updateStatus.downloadProgress >= 0.0f) {
					if (updateStatus.totalBytes > 0) {
						char overlay[64];
						snprintf(overlay, sizeof(overlay), "%.1f %%",
							updateStatus.downloadProgress * 100.0f);
						ImGui::ProgressBar(updateStatus.downloadProgress, ImVec2(-1, 0),
							overlay);
					}
					else {
						ImGui::ProgressBar(-1.0f, ImVec2(-1, 0),
							lang.updateDownloading.c_str());
					}
				}
				else if (updateStatus.phase == UpdatePhase::Installing ||
					updateStatus.phase == UpdatePhase::Restarting) {
					ImGui::ProgressBar(-1.0f, ImVec2(-1, 0),
						lang.updateInstalling.c_str());
				}

				// Retrying after a failure must stay possible: a failed install
				// rolls back, so the app is still the old (updatable) version.
				bool canUpdate = (updateStatus.phase == UpdatePhase::Available ||
					updateStatus.phase == UpdatePhase::Failed) &&
					!updateBusy;
				if (!canUpdate) ImGui::BeginDisabled();
				if (ImGui::Button(lang.btnUpdate.c_str())) StartUpdateProcess();
				if (!canUpdate) ImGui::EndDisabled();

				ImGui::SameLine();
				if (ImGui::Button(lang.btnOpenRelease.c_str())) OpenLatestReleasePage();

				ImGui::Separator();
				if (ImGui::Button(
					(lang.btnToggle + "[" + toggleKeyName + "]").c_str())) {
					BeginKeyCapture(CaptureToggle);
				}
				if (ImGui::Button(
					(lang.btnSettings + "[" + settingsKeyName + "]").c_str())) {
					BeginKeyCapture(CaptureSettings);
				}
				ImGui::SameLine();
				if (ImGui::Button(
					(lang.btnEvents + " [" + eventsKeyName + "]").c_str())) {
					BeginKeyCapture(CaptureEvents);
				}

				ImGui::Separator();
				ImGui::Text(lang.combatCondition.c_str());

				ImGui::RadioButton(lang.radioAlways.c_str(), &combatMouseTrigger, -1);
				ImGui::SameLine();
				ImGui::RadioButton(lang.radioLmb.c_str(), &combatMouseTrigger, 0);
				ImGui::SameLine();
				ImGui::RadioButton(lang.radioRmb.c_str(), &combatMouseTrigger, 1);

				if (ImGui::IsItemDeactivatedAfterEdit()) SaveConfig();

				ImGui::Separator();
				ImGui::Checkbox(lang.chkGlobalHealth.c_str(), &globalHealthCheckEnable);
				if (ImGui::IsItemDeactivatedAfterEdit()) SaveConfig();

				if (globalHealthCheckEnable) {
					if (ImGui::Checkbox(lang.chkGlobalHealthIndependent.c_str(),
						&globalHealthIndependent)) {
						SaveConfig();
					}
					if (ImGui::Button(
						(lang.lblHealthKey + "[" + healthKeyName + "]").c_str())) {
						BeginKeyCapture(CaptureHealth);
					}
					ImGui::SameLine();
					ImGui::PushItemWidth(100);
					if (ImGui::InputInt(lang.lblHealTimer.c_str(), &healthDelayMs, 0,
						0)) {
						if (healthDelayMs < 1) healthDelayMs = 1;
						SaveConfig();
					}
					ImGui::PopItemWidth();

					std::string coordsLabel = lang.btnPickCoords +
						" (X:" + std::to_string(healthX) +
						" Y:" + std::to_string(healthY) + ")";
					if (ImGui::Button(coordsLabel.c_str())) isCapturingCoordinates = true;
				}

				ImGui::Separator();
				if (ImGui::Button(
					(lang.lblFastLootHoldKey + "[" + fastLootHoldKeyName + "]")
					.c_str())) {
					BeginKeyCapture(CaptureLootHold);
				}
				ImGui::SameLine();
				if (ImGui::Button(
					(lang.lblFastLootClickKey + "[" + fastLootClickKeyName + "]")
					.c_str())) {
					BeginKeyCapture(CaptureLootClick);
				}
				ImGui::SameLine();
				ImGui::PushItemWidth(100);
				if (ImGui::InputInt(lang.lblFastLootTimer.c_str(), &fastLootDelayMs, 0,
					0)) {
					if (fastLootDelayMs < 1) fastLootDelayMs = 1;
					SaveConfig();
				}
				ImGui::PopItemWidth();

				ImGui::Separator();
				ImGui::Text(lang.lblSpamList.c_str());
				for (size_t i = 0; i < spamKeys.size(); i++) {
					ImGui::PushID(static_cast<int>(i));
					if (ImGui::Button(("[" + spamKeys[i].keyName + "]##btn").c_str(),
						ImVec2(65, 0))) {
						BeginKeyCapture(CaptureSpamBase + static_cast<int>(i));
					}
					ImGui::SameLine();
					ImGui::PushItemWidth(65);
					if (ImGui::InputInt((lang.lblDelayMs + "##del").c_str(),
						&spamKeys[i].delayMs, 0, 0)) {
						if (spamKeys[i].delayMs < 1) spamKeys[i].delayMs = 1;
						SaveConfig();
					}
					ImGui::PopItemWidth();
					ImGui::SameLine();

					if (ImGui::Checkbox(lang.lblShift.c_str(), &spamKeys[i].withShift))
						SaveConfig();
					ImGui::SameLine();
					if (ImGui::Checkbox(lang.lblCtrl.c_str(), &spamKeys[i].withCtrl))
						SaveConfig();
					ImGui::SameLine();
					if (ImGui::Checkbox(lang.lblAlt.c_str(), &spamKeys[i].withAlt))
						SaveConfig();
					ImGui::SameLine();

					if (ImGui::Button("X")) {
						spamKeys.erase(spamKeys.begin() + i);
						SaveConfig();
						ImGui::PopID();
						break;
					}
					ImGui::PopID();
				}
				if (ImGui::Button(lang.btnAddKey.c_str())) {
					spamKeys.push_back({ '1', "1", 100, false, false, false });
					SaveConfig();
				}
				ImGui::End();
			}
		}

		ImGui::Render();
		const float clear_color_with_alpha[] = { 0.0f, 0.0f, 0.0f, 0.0f };
		g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView,
			nullptr);
		g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,
			clear_color_with_alpha);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_pSwapChain->Present(1, 0);
	}

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	CleanupDeviceD3D();
	DestroyWindow(hwnd);
	UnregisterClassW(L"OverlayClass", hInstance);
	return 0;
}
