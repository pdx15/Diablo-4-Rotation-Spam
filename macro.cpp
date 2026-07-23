#include <windows.h>

#include "resource.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct SpamKey {
  int vKey = '1';
  std::string keyName = "1";
  int delayMs = 50;
  bool withShift = false;
  bool withCtrl = false;
  bool withAlt = false;
  std::chrono::steady_clock::time_point lastPressed;
};

std::atomic<bool> isScriptActive(false);
std::atomic<bool> isHealthy(true);
bool showSettingsWindow = false;
bool isCapturing = false;
bool isCapturingCoordinates = false;

std::vector<SpamKey> spamKeys;
int combatMouseTrigger = 1;
bool globalHealthCheckEnable = true;
int healthVKey = 'Q';
std::string healthKeyName = "Q";
int healthDelayMs = 50;
std::chrono::steady_clock::time_point lastHealthPressed;

int healthX = 960;
int healthY = 1010;
int toggleHotkey = VK_XBUTTON2;
std::string toggleKeyName = "Mouse5";
int settingsHotkey = VK_F5;
std::string settingsKeyName = "F5";
int keyToCaptureType = -1;

std::string GetKeyNameFromVK(int vk) {
  if (vk == VK_XBUTTON1) return "Mouse4";
  if (vk == VK_XBUTTON2) return "Mouse5";
  if (vk == VK_LBUTTON) return "LButton";
  if (vk == VK_RBUTTON) return "RButton";
  char name[64] = {0};
  UINT scanCode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
  LONG lParam = scanCode << 16;
  if (GetKeyNameTextA(lParam, name, 64) > 0) return std::string(name);
  return "Unknown";
}

void PressKeyWithModifiers(WORD wVk, bool shift, bool ctrl, bool alt) {
  std::vector<INPUT> inputs;
  INPUT in = {0};
  in.type = INPUT_KEYBOARD;

  if (shift) {
    in.ki.wVk = VK_SHIFT;
    in.ki.dwFlags = 0;
    inputs.push_back(in);
  }
  if (ctrl) {
    in.ki.wVk = VK_CONTROL;
    in.ki.dwFlags = 0;
    inputs.push_back(in);
  }
  if (alt) {
    in.ki.wVk = VK_MENU;
    in.ki.dwFlags = 0;
    inputs.push_back(in);
  }

  in.ki.wVk = wVk;
  in.ki.dwFlags = 0;
  inputs.push_back(in);
  in.ki.wVk = wVk;
  in.ki.dwFlags = KEYEVENTF_KEYUP;
  inputs.push_back(in);

  if (alt) {
    in.ki.wVk = VK_MENU;
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(in);
  }
  if (ctrl) {
    in.ki.wVk = VK_CONTROL;
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(in);
  }
  if (shift) {
    in.ki.wVk = VK_SHIFT;
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(in);
  }

  SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

void PressKey(WORD wVk) { PressKeyWithModifiers(wVk, false, false, false); }

bool IsDiabloActive() {
  HWND hwnd = GetForegroundWindow();
  if (!hwnd) return false;
  wchar_t className[256];
  GetClassNameW(hwnd, className, 256);
  return (wcscmp(className, L"Diablo IV Main Window Class") == 0);
}
double GetHealthyPixelsRatio(int centerX, int centerY, COLORREF targetColor,
                             int tolerance) {
  HDC hdcScreen = GetDC(NULL);
  if (!hdcScreen) return 0.0;

  int healthyCount = 0;
  int totalCount = 0;
  int radius = 3;

  int r2 = GetRValue(targetColor), g2 = GetGValue(targetColor),
      b2 = GetBValue(targetColor);

  for (int y = centerY - radius; y <= centerY + radius; ++y) {
    for (int x = centerX - radius; x <= centerX + radius; ++x) {
      COLORREF color = GetPixel(hdcScreen, x, y);
      if (color == CLR_INVALID) continue;

      int r1 = GetRValue(color), g1 = GetGValue(color), b1 = GetBValue(color);
      if (abs(r1 - r2) <= tolerance && abs(g1 - g2) <= tolerance &&
          abs(b1 - b2) <= tolerance) {
        healthyCount++;
      }
      totalCount++;
    }
  }
  ReleaseDC(NULL, hdcScreen);

  if (totalCount == 0) return 0.0;
  return static_cast<double>(healthyCount) / totalCount;
}

void SaveConfig() {
  std::ofstream out("config.txt");
  if (!out.is_open()) return;
  out << toggleHotkey << " " << toggleKeyName << "\n"
      << settingsHotkey << " " << settingsKeyName << "\n";
  out << combatMouseTrigger << "\n"
      << globalHealthCheckEnable << " " << healthVKey << " " << healthKeyName
      << " " << healthDelayMs << "\n";
  out << healthX << " " << healthY << "\n" << spamKeys.size() << "\n";
  for (const auto& sk : spamKeys) {
    out << sk.vKey << " " << sk.keyName << " " << sk.delayMs << " "
        << sk.withShift << " " << sk.withCtrl << " " << sk.withAlt << "\n";
  }
  out.close();
}

void LoadConfig() {
  std::ifstream in("config.txt");
  if (!in.is_open()) {
    spamKeys.push_back({'1', "1", 50, false, false, false});
    spamKeys.push_back({'2', "2", 50, false, false, false});
    spamKeys.push_back({'3', "3", 50, false, false, false});
    spamKeys.push_back({'4', "4", 2000, false, false, false});
    return;
  }
  in >> toggleHotkey >> toggleKeyName >> settingsHotkey >> settingsKeyName >>
      combatMouseTrigger;
  in >> globalHealthCheckEnable >> healthVKey >> healthKeyName >>
      healthDelayMs >> healthX >> healthY;
  size_t size = 0;
  if (in >> size) {
    spamKeys.clear();
    for (size_t i = 0; i < size; ++i) {
      SpamKey sk;
      in >> sk.vKey >> sk.keyName >> sk.delayMs >> sk.withShift >>
          sk.withCtrl >> sk.withAlt;
      spamKeys.push_back(sk);
    }
  }
  in.close();
}

void CoreMacroLoop() {
  while (true) {
    if (IsDiabloActive()) {
      double healthyRatio =
          GetHealthyPixelsRatio(healthX, healthY, RGB(0x9E, 0x30, 0x38), 50);

      if (healthyRatio > 0.0) {
        isHealthy = true;
      } else {
        isHealthy = false;
      }

      auto now = std::chrono::steady_clock::now();
      int currentMouseKey = (combatMouseTrigger == 0) ? VK_LBUTTON : VK_RBUTTON;
      bool isMouseTriggerPressed =
          (GetAsyncKeyState(currentMouseKey) & 0x8000) != 0;

      if (globalHealthCheckEnable && !isHealthy && isScriptActive &&
          isMouseTriggerPressed) {
        auto elapsedHealth =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastHealthPressed)
                .count();
        if (elapsedHealth >= healthDelayMs) {
          PressKey(healthVKey);
          lastHealthPressed = now;
        }
      }
      if (isScriptActive && isMouseTriggerPressed) {
        for (auto& sk : spamKeys) {
          auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - sk.lastPressed)
                             .count();
          if (elapsed >= sk.delayMs) {
            PressKeyWithModifiers(sk.vKey, sk.withShift, sk.withCtrl,
                                  sk.withAlt);
            sk.lastPressed = now;
          }
        }
      }
    } else {
      isHealthy = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void GlobalHotkeyMonitor() {
  while (true) {
    if (isCapturingCoordinates) {
      if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        POINT p;
        if (GetCursorPos(&p)) {
          HWND diabloHwnd =
              FindWindowW(L"Diablo IV Main Window Class", nullptr);
          if (diabloHwnd) ScreenToClient(diabloHwnd, &p);
          healthX = p.x;
          healthY = p.y;
        }
        isCapturingCoordinates = false;
        SaveConfig();
        Beep(800, 150);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
      }
    } else if (isCapturing) {
      for (int vk = 1; vk < 256; vk++) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON) continue;
        if (GetAsyncKeyState(vk) & 0x8000) {
          std::string name = GetKeyNameFromVK(vk);
          if (keyToCaptureType == 0) {
            toggleHotkey = vk;
            toggleKeyName = name;
          } else if (keyToCaptureType == 1) {
            settingsHotkey = vk;
            settingsKeyName = name;
          } else if (keyToCaptureType == 2) {
            healthVKey = vk;
            healthKeyName = name;
          } else if (keyToCaptureType >= 3) {
            size_t idx = keyToCaptureType - 3;
            if (idx < spamKeys.size()) {
              spamKeys[idx].vKey = vk;
              spamKeys[idx].keyName = name;
            }
          }
          isCapturing = false;
          keyToCaptureType = -1;
          SaveConfig();
          Beep(600, 100);
          std::this_thread::sleep_for(std::chrono::milliseconds(400));
          break;
        }
      }
    } else {
      if (GetAsyncKeyState(toggleHotkey) & 0x8000) {
        isScriptActive = !isScriptActive;
        Beep(isScriptActive ? 440 : 220, 150);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      }
      if (GetAsyncKeyState(settingsHotkey) & 0x8000) {
        showSettingsWindow = !showSettingsWindow;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      }
      if (GetAsyncKeyState(VK_F9) & 0x8000) {
        Beep(220, 300);
        PostQuitMessage(0);
        ExitProcess(0);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

struct LocStrings {
  std::string gameStatus = "Game Status: ";
  std::string scriptStatus = "Script Status: ";
  std::string healthStatus = "Health Status: ";
  std::string options = "Options: ";
  std::string healthy = "Healthy";
  std::string lowHp = "Low HP";
  std::string captureCoordsTitle = "HEALTH PIXEL SELECTION MODE:";
  std::string captureCoordsDesc =
      "Switch to Diablo 4 and LEFT CLICK on your health sphere...";
  std::string captureKeyTitle = "KEY CAPTURE MODE:";
  std::string captureKeyDesc =
      "Press ANY key on your keyboard or side mouse buttons...";
  std::string settingsTitle = "Overlay Configuration:";
  std::string btnToggle = "Toggle Script: ";
  std::string btnSettings = "Open Options: ";
  std::string combatCondition = "Combat Spam Activation Condition:";
  std::string radioLmb = "Hold LMB";
  std::string radioRmb = "Hold RMB";
  std::string chkGlobalHealth = "Global Auto-Heal by HP pixel";
  std::string lblHealthKey = "Heal Key: ";
  std::string lblHealTimer = "Heal Timer (ms)";
  std::string btnPickCoords = "Pick HP Point with Click";
  std::string lblSpamList = "Combat Spam Keys List:";
  std::string btnDelete = "Delete";
  std::string btnAddKey = "Add Combat Key";
  std::string lblShift = "Shift";
  std::string lblCtrl = "Ctrl";
  std::string lblAlt = "Alt";
};
LocStrings lang;

std::string LoadTextResource(int resourceId) {
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resourceInfo =
      FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
  if (!resourceInfo) return {};

  HGLOBAL resourceData = LoadResource(module, resourceInfo);
  if (!resourceData) return {};

  DWORD resourceSize = SizeofResource(module, resourceInfo);
  const char* data = static_cast<const char*>(LockResource(resourceData));
  if (!data || resourceSize == 0) return {};

  return std::string(data, static_cast<size_t>(resourceSize));
}

void LoadLanguage() {
  int resourceId = IDR_LANG_EN;
  LANGID langId = GetUserDefaultUILanguage();
  if (PRIMARYLANGID(langId) == LANG_RUSSIAN) {
    resourceId = IDR_LANG_RU;
  }

  std::string languageText = LoadTextResource(resourceId);
  if (languageText.empty() && resourceId != IDR_LANG_EN) {
    languageText = LoadTextResource(IDR_LANG_EN);
  }
  if (languageText.empty()) return;

  std::istringstream in(languageText);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
      line.erase(0, 3);
    }
    if (line.empty() || line[0] == '#') continue;
    std::size_t sep = line.find('=');
    if (sep == std::string::npos) continue;
    std::string key = line.substr(0, sep);
    std::string val = line.substr(sep + 1);
    if (key == "gameStatus")
      lang.gameStatus = val;
    else if (key == "scriptStatus")
      lang.scriptStatus = val;
    else if (key == "healthStatus")
      lang.healthStatus = val;
    else if (key == "options")
      lang.options = val;
    else if (key == "healthy")
      lang.healthy = val;
    else if (key == "lowHp")
      lang.lowHp = val;
    else if (key == "captureCoordsTitle")
      lang.captureCoordsTitle = val;
    else if (key == "captureCoordsDesc")
      lang.captureCoordsDesc = val;
    else if (key == "captureKeyTitle")
      lang.captureKeyTitle = val;
    else if (key == "captureKeyDesc")
      lang.captureKeyDesc = val;
    else if (key == "settingsTitle")
      lang.settingsTitle = val;
    else if (key == "btnToggle")
      lang.btnToggle = val;
    else if (key == "btnSettings")
      lang.btnSettings = val;
    else if (key == "combatCondition")
      lang.combatCondition = val;
    else if (key == "radioLmb")
      lang.radioLmb = val;
    else if (key == "radioRmb")
      lang.radioRmb = val;
    else if (key == "chkGlobalHealth")
      lang.chkGlobalHealth = val;
    else if (key == "lblHealthKey")
      lang.lblHealthKey = val;
    else if (key == "lblHealTimer")
      lang.lblHealTimer = val;
    else if (key == "btnPickCoords")
      lang.btnPickCoords = val;
    else if (key == "lblSpamList")
      lang.lblSpamList = val;
    else if (key == "btnDelete")
      lang.btnDelete = val;
    else if (key == "btnAddKey")
      lang.btnAddKey = val;
    else if (key == "lblShift")
      lang.lblShift = val;
    else if (key == "lblCtrl")
      lang.lblCtrl = val;
    else if (key == "lblAlt")
      lang.lblAlt = val;
  }
}
