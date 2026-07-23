#include <windows.h>

#include "app_state.h"
#include "resource.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

std::atomic<bool> isScriptActive(false);
std::atomic<bool> isHealthy(true);
bool showSettingsWindow = false;
bool isCapturing = false;
bool isCapturingCoordinates = false;
std::recursive_mutex settingsMutex;

std::vector<SpamKey> spamKeys;
int combatMouseTrigger = 1;
bool globalHealthCheckEnable = true;
int healthVKey = 'Q';
std::string healthKeyName = "Q";
int healthDelayMs = 50;
std::chrono::steady_clock::time_point lastHealthPressed;
std::vector<ProfileConfig> profiles;
int activeProfileIndex = 0;

int healthX = 960;
int healthY = 1010;
int toggleHotkey = VK_XBUTTON2;
std::string toggleKeyName = "Mouse5";
int settingsHotkey = VK_F5;
std::string settingsKeyName = "F5";
int keyToCaptureType = -1;

struct MacroSettingsSnapshot {
  int combatMouseTrigger = 1;
  bool globalHealthCheckEnable = true;
  int healthVKey = 'Q';
  int healthDelayMs = 50;
  int healthX = 960;
  int healthY = 1010;
  std::vector<SpamKey> spamKeys;
};

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

MacroSettingsSnapshot GetMacroSettingsSnapshot() {
  std::lock_guard<std::recursive_mutex> lock(settingsMutex);
  return {combatMouseTrigger,
          globalHealthCheckEnable,
          healthVKey,
          healthDelayMs,
          healthX,
          healthY,
          spamKeys};
}

std::vector<SpamKey> MakeDefaultSpamKeys() {
  return {{'1', "1", 50, false, false, false},
          {'2', "2", 50, false, false, false},
          {'3', "3", 50, false, false, false},
          {'4', "4", 2000, false, false, false}};
}

std::string Trim(const std::string& text) {
  size_t first = 0;
  while (first < text.size() &&
         std::isspace(static_cast<unsigned char>(text[first]))) {
    first++;
  }

  size_t last = text.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(text[last - 1]))) {
    last--;
  }

  return text.substr(first, last - first);
}

bool TryReadInt(const std::unordered_map<std::string, std::string>& values,
                const std::string& key, int& out) {
  auto it = values.find(key);
  if (it == values.end()) return false;

  try {
    size_t parsed = 0;
    int value = std::stoi(Trim(it->second), &parsed);
    if (parsed != Trim(it->second).size()) return false;
    out = value;
    return true;
  } catch (...) {
    return false;
  }
}

int ReadInt(const std::unordered_map<std::string, std::string>& values,
            const std::string& key, int fallback, int minValue,
            int maxValue) {
  int value = fallback;
  TryReadInt(values, key, value);
  return std::clamp(value, minValue, maxValue);
}

bool ReadBool(const std::unordered_map<std::string, std::string>& values,
              const std::string& key, bool fallback) {
  auto it = values.find(key);
  if (it == values.end()) return fallback;

  std::string value = Trim(it->second);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (value == "1" || value == "true" || value == "yes" || value == "on")
    return true;
  if (value == "0" || value == "false" || value == "no" || value == "off")
    return false;
  return fallback;
}

std::string ReadString(const std::unordered_map<std::string, std::string>& values,
                       const std::string& key,
                       const std::string& fallback) {
  auto it = values.find(key);
  if (it == values.end()) return fallback;

  std::string value = Trim(it->second);
  return value.empty() ? fallback : value;
}

bool LoadKeyValueConfig(std::unordered_map<std::string, std::string>& values) {
  std::ifstream in("config.txt");
  if (!in.is_open()) return false;

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
      line.erase(0, 3);
    }

    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;

    size_t sep = line.find('=');
    if (sep == std::string::npos) continue;

    std::string key = Trim(line.substr(0, sep));
    std::string value = Trim(line.substr(sep + 1));
    if (!key.empty()) values[key] = value;
  }

  return !values.empty();
}

ProfileConfig MakeProfileFromGlobals(const std::string& name) {
  ProfileConfig profile;
  profile.name = name;
  profile.combatMouseTrigger = combatMouseTrigger;
  profile.globalHealthCheckEnable = globalHealthCheckEnable;
  profile.healthVKey = healthVKey;
  profile.healthKeyName = healthKeyName;
  profile.healthDelayMs = healthDelayMs;
  profile.healthX = healthX;
  profile.healthY = healthY;
  profile.spamKeys = spamKeys;
  return profile;
}

void ApplyProfileToGlobals(const ProfileConfig& profile) {
  combatMouseTrigger = profile.combatMouseTrigger;
  globalHealthCheckEnable = profile.globalHealthCheckEnable;
  healthVKey = profile.healthVKey;
  healthKeyName = profile.healthKeyName;
  healthDelayMs = profile.healthDelayMs;
  healthX = profile.healthX;
  healthY = profile.healthY;
  spamKeys = profile.spamKeys;
}

void StoreGlobalsInActiveProfile() {
  if (profiles.empty()) return;
  activeProfileIndex =
      std::clamp(activeProfileIndex, 0, static_cast<int>(profiles.size()) - 1);
  profiles[activeProfileIndex] =
      MakeProfileFromGlobals(profiles[activeProfileIndex].name);
}

void ResetToDefaultConfig() {
  toggleHotkey = VK_XBUTTON2;
  toggleKeyName = "Mouse5";
  settingsHotkey = VK_F5;
  settingsKeyName = "F5";
  combatMouseTrigger = 1;
  globalHealthCheckEnable = true;
  healthVKey = 'Q';
  healthKeyName = "Q";
  healthDelayMs = 50;
  healthX = 960;
  healthY = 1010;
  spamKeys = MakeDefaultSpamKeys();
  profiles.clear();
  profiles.push_back(MakeProfileFromGlobals("Default"));
  activeProfileIndex = 0;
}

void SaveConfig() {
  std::lock_guard<std::recursive_mutex> lock(settingsMutex);

  if (profiles.empty()) {
    profiles.push_back(MakeProfileFromGlobals("Default"));
    activeProfileIndex = 0;
  }
  StoreGlobalsInActiveProfile();

  std::ofstream out("config.txt", std::ios::trunc);
  if (!out.is_open()) return;

  out << "version=2\n";
  out << "activeProfile=" << activeProfileIndex << "\n";
  out << "toggleHotkey=" << toggleHotkey << "\n";
  out << "toggleKeyName=" << toggleKeyName << "\n";
  out << "settingsHotkey=" << settingsHotkey << "\n";
  out << "settingsKeyName=" << settingsKeyName << "\n";
  out << "profileCount=" << profiles.size() << "\n";

  for (size_t i = 0; i < profiles.size(); ++i) {
    const ProfileConfig& profile = profiles[i];
    std::string prefix = "profile." + std::to_string(i) + ".";
    out << prefix << "name=" << profile.name << "\n";
    out << prefix << "combatMouseTrigger=" << profile.combatMouseTrigger
        << "\n";
    out << prefix << "globalHealthCheckEnable="
        << profile.globalHealthCheckEnable << "\n";
    out << prefix << "healthVKey=" << profile.healthVKey << "\n";
    out << prefix << "healthKeyName=" << profile.healthKeyName << "\n";
    out << prefix << "healthDelayMs=" << profile.healthDelayMs << "\n";
    out << prefix << "healthX=" << profile.healthX << "\n";
    out << prefix << "healthY=" << profile.healthY << "\n";
    out << prefix << "spamCount=" << profile.spamKeys.size() << "\n";

    for (size_t j = 0; j < profile.spamKeys.size(); ++j) {
      const SpamKey& sk = profile.spamKeys[j];
      std::string spamPrefix =
          prefix + "spam." + std::to_string(j) + ".";
      out << spamPrefix << "vKey=" << sk.vKey << "\n";
      out << spamPrefix << "keyName=" << sk.keyName << "\n";
      out << spamPrefix << "delayMs=" << sk.delayMs << "\n";
      out << spamPrefix << "withShift=" << sk.withShift << "\n";
      out << spamPrefix << "withCtrl=" << sk.withCtrl << "\n";
      out << spamPrefix << "withAlt=" << sk.withAlt << "\n";
    }
  }
}

bool LoadLegacyConfig() {
  std::ifstream in("config.txt");
  if (!in.is_open()) return false;

  if (!(in >> toggleHotkey >> toggleKeyName >> settingsHotkey >>
        settingsKeyName >> combatMouseTrigger)) {
    return false;
  }
  if (!(in >> globalHealthCheckEnable >> healthVKey >> healthKeyName >>
        healthDelayMs >> healthX >> healthY)) {
    return false;
  }

  size_t size = 0;
  if (in >> size) {
    size = std::min<size_t>(size, 64);
    spamKeys.clear();
    for (size_t i = 0; i < size; ++i) {
      SpamKey sk;
      if (!(in >> sk.vKey >> sk.keyName >> sk.delayMs >> sk.withShift >>
            sk.withCtrl >> sk.withAlt)) {
        return false;
      }
      sk.delayMs = std::max(sk.delayMs, 1);
      spamKeys.push_back(sk);
    }
  }

  if (spamKeys.empty()) spamKeys = MakeDefaultSpamKeys();
  profiles.clear();
  profiles.push_back(MakeProfileFromGlobals("Default"));
  activeProfileIndex = 0;
  return true;
}

bool LoadModernConfig() {
  std::unordered_map<std::string, std::string> values;
  if (!LoadKeyValueConfig(values)) return false;
  if (values.find("version") == values.end() ||
      values.find("profileCount") == values.end()) {
    return false;
  }

  toggleHotkey = ReadInt(values, "toggleHotkey", VK_XBUTTON2, 1, 255);
  toggleKeyName = ReadString(values, "toggleKeyName", "Mouse5");
  settingsHotkey = ReadInt(values, "settingsHotkey", VK_F5, 1, 255);
  settingsKeyName = ReadString(values, "settingsKeyName", "F5");

  int profileCount = ReadInt(values, "profileCount", 1, 1, 16);
  profiles.clear();

  for (int i = 0; i < profileCount; ++i) {
    std::string prefix = "profile." + std::to_string(i) + ".";
    ProfileConfig profile;
    profile.name =
        ReadString(values, prefix + "name", "Profile " + std::to_string(i + 1));
    profile.combatMouseTrigger =
        ReadInt(values, prefix + "combatMouseTrigger", 1, 0, 1);
    profile.globalHealthCheckEnable =
        ReadBool(values, prefix + "globalHealthCheckEnable", true);
    profile.healthVKey = ReadInt(values, prefix + "healthVKey", 'Q', 1, 255);
    profile.healthKeyName = ReadString(values, prefix + "healthKeyName", "Q");
    profile.healthDelayMs =
        ReadInt(values, prefix + "healthDelayMs", 50, 1, 600000);
    profile.healthX = ReadInt(values, prefix + "healthX", 960, -32000, 32000);
    profile.healthY = ReadInt(values, prefix + "healthY", 1010, -32000, 32000);

    int spamCount = ReadInt(values, prefix + "spamCount", 0, 0, 64);
    for (int j = 0; j < spamCount; ++j) {
      std::string spamPrefix = prefix + "spam." + std::to_string(j) + ".";
      SpamKey sk;
      sk.vKey = ReadInt(values, spamPrefix + "vKey", '1', 1, 255);
      sk.keyName = ReadString(values, spamPrefix + "keyName", "1");
      sk.delayMs = ReadInt(values, spamPrefix + "delayMs", 50, 1, 600000);
      sk.withShift = ReadBool(values, spamPrefix + "withShift", false);
      sk.withCtrl = ReadBool(values, spamPrefix + "withCtrl", false);
      sk.withAlt = ReadBool(values, spamPrefix + "withAlt", false);
      profile.spamKeys.push_back(sk);
    }

    profiles.push_back(profile);
  }

  if (profiles.empty()) profiles.push_back(ProfileConfig{});
  activeProfileIndex =
      ReadInt(values, "activeProfile", 0, 0, static_cast<int>(profiles.size()) - 1);
  ApplyProfileToGlobals(profiles[activeProfileIndex]);
  return true;
}

void LoadConfig() {
  std::lock_guard<std::recursive_mutex> lock(settingsMutex);

  if (LoadModernConfig()) return;

  if (LoadLegacyConfig()) {
    SaveConfig();
    return;
  }

  ResetToDefaultConfig();
}

std::string MakeUniqueProfileName() {
  for (int i = 1; i <= 16; ++i) {
    std::string name = "Profile " + std::to_string(i);
    bool exists = false;
    for (const ProfileConfig& profile : profiles) {
      if (profile.name == name) {
        exists = true;
        break;
      }
    }
    if (!exists) return name;
  }

  return "Profile";
}

void SelectProfile(int profileIndex) {
  std::lock_guard<std::recursive_mutex> lock(settingsMutex);

  if (profiles.empty()) {
    ResetToDefaultConfig();
    SaveConfig();
    return;
  }

  profileIndex =
      std::clamp(profileIndex, 0, static_cast<int>(profiles.size()) - 1);
  if (profileIndex == activeProfileIndex) return;

  StoreGlobalsInActiveProfile();
  activeProfileIndex = profileIndex;
  ApplyProfileToGlobals(profiles[activeProfileIndex]);
  SaveConfig();
}

void AddProfile() {
  std::lock_guard<std::recursive_mutex> lock(settingsMutex);

  if (profiles.empty()) ResetToDefaultConfig();
  if (profiles.size() >= 16) return;

  StoreGlobalsInActiveProfile();
  ProfileConfig profile = profiles[activeProfileIndex];
  profile.name = MakeUniqueProfileName();
  profiles.push_back(profile);
  activeProfileIndex = static_cast<int>(profiles.size()) - 1;
  ApplyProfileToGlobals(profiles[activeProfileIndex]);
  SaveConfig();
}

void DeleteActiveProfile() {
  std::lock_guard<std::recursive_mutex> lock(settingsMutex);

  if (profiles.size() <= 1) return;

  activeProfileIndex =
      std::clamp(activeProfileIndex, 0, static_cast<int>(profiles.size()) - 1);
  profiles.erase(profiles.begin() + activeProfileIndex);
  activeProfileIndex =
      std::clamp(activeProfileIndex, 0, static_cast<int>(profiles.size()) - 1);
  ApplyProfileToGlobals(profiles[activeProfileIndex]);
  SaveConfig();
}

void CoreMacroLoop() {
  std::vector<std::chrono::steady_clock::time_point> spamLastPressed;
  auto lastHealthPressed = std::chrono::steady_clock::time_point{};

  while (true) {
    if (IsDiabloActive()) {
      MacroSettingsSnapshot settings = GetMacroSettingsSnapshot();
      double healthyRatio =
          GetHealthyPixelsRatio(settings.healthX, settings.healthY,
                                RGB(0x9E, 0x30, 0x38), 50);

      if (healthyRatio > 0.0) {
        isHealthy = true;
      } else {
        isHealthy = false;
      }

      auto now = std::chrono::steady_clock::now();
      int currentMouseKey =
          (settings.combatMouseTrigger == 0) ? VK_LBUTTON : VK_RBUTTON;
      bool isMouseTriggerPressed =
          (GetAsyncKeyState(currentMouseKey) & 0x8000) != 0;

      if (settings.globalHealthCheckEnable && !isHealthy && isScriptActive &&
          isMouseTriggerPressed) {
        auto elapsedHealth =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastHealthPressed)
                .count();
        if (elapsedHealth >= settings.healthDelayMs) {
          PressKey(settings.healthVKey);
          lastHealthPressed = now;
        }
      }

      if (spamLastPressed.size() != settings.spamKeys.size()) {
        spamLastPressed.resize(settings.spamKeys.size());
      }

      if (isScriptActive && isMouseTriggerPressed) {
        for (size_t i = 0; i < settings.spamKeys.size(); ++i) {
          const SpamKey& sk = settings.spamKeys[i];
          auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - spamLastPressed[i])
                             .count();
          if (elapsed >= sk.delayMs) {
            PressKeyWithModifiers(sk.vKey, sk.withShift, sk.withCtrl,
                                  sk.withAlt);
            spamLastPressed[i] = now;
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
    bool capturingCoordinates = false;
    bool capturingKey = false;
    int captureType = -1;
    int currentToggleHotkey = VK_XBUTTON2;
    int currentSettingsHotkey = VK_F5;

    {
      std::lock_guard<std::recursive_mutex> lock(settingsMutex);
      capturingCoordinates = isCapturingCoordinates;
      capturingKey = isCapturing;
      captureType = keyToCaptureType;
      currentToggleHotkey = toggleHotkey;
      currentSettingsHotkey = settingsHotkey;
    }

    if (capturingCoordinates) {
      if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        POINT p;
        if (GetCursorPos(&p)) {
          HWND diabloHwnd =
              FindWindowW(L"Diablo IV Main Window Class", nullptr);
          if (diabloHwnd) ScreenToClient(diabloHwnd, &p);
          {
            std::lock_guard<std::recursive_mutex> lock(settingsMutex);
            healthX = p.x;
            healthY = p.y;
            isCapturingCoordinates = false;
            SaveConfig();
          }
        }
        Beep(800, 150);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
      }
    } else if (capturingKey) {
      for (int vk = 1; vk < 256; vk++) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON) continue;
        if (GetAsyncKeyState(vk) & 0x8000) {
          std::string name = GetKeyNameFromVK(vk);
          {
            std::lock_guard<std::recursive_mutex> lock(settingsMutex);
            captureType = keyToCaptureType;
            if (captureType == 0) {
              toggleHotkey = vk;
              toggleKeyName = name;
            } else if (captureType == 1) {
              settingsHotkey = vk;
              settingsKeyName = name;
            } else if (captureType == 2) {
              healthVKey = vk;
              healthKeyName = name;
            } else if (captureType >= 3) {
              size_t idx = captureType - 3;
              if (idx < spamKeys.size()) {
                spamKeys[idx].vKey = vk;
                spamKeys[idx].keyName = name;
              }
            }
            isCapturing = false;
            keyToCaptureType = -1;
            SaveConfig();
          }
          Beep(600, 100);
          std::this_thread::sleep_for(std::chrono::milliseconds(400));
          break;
        }
      }
    } else {
      if (GetAsyncKeyState(currentToggleHotkey) & 0x8000) {
        isScriptActive = !isScriptActive;
        Beep(isScriptActive ? 440 : 220, 150);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      }
      if (GetAsyncKeyState(currentSettingsHotkey) & 0x8000) {
        {
          std::lock_guard<std::recursive_mutex> lock(settingsMutex);
          showSettingsWindow = !showSettingsWindow;
        }
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
    else if (key == "profile")
      lang.profile = val;
    else if (key == "btnAddProfile")
      lang.btnAddProfile = val;
    else if (key == "btnDeleteProfile")
      lang.btnDeleteProfile = val;
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
    else if (key == "lblDelayMs")
      lang.lblDelayMs = val;
    else if (key == "lblShift")
      lang.lblShift = val;
    else if (key == "lblCtrl")
      lang.lblCtrl = val;
    else if (key == "lblAlt")
      lang.lblAlt = val;
  }
}
