#pragma once

#include <chrono>
#include <string>
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
  std::string profile = "Profile:";
  std::string btnAddProfile = "Add Profile";
  std::string btnDeleteProfile = "Delete Profile";
  std::string btnToggle = "Toggle Script: ";
  std::string btnSettings = "Open Options: ";
  std::string combatCondition = "Combat Spam Activation Condition:";
  std::string radioLmb = "Hold LMB";
  std::string radioRmb = "Hold RMB";
  std::string radioAlways = "Always";
  std::string chkGlobalHealth = "Global Auto-Heal by HP pixel";
  std::string lblHealthKey = "Heal Key: ";
  std::string lblHealTimer = "Heal Timer (ms)";
  std::string btnPickCoords = "Pick HP Point with Click";
  std::string lblSpamList = "Combat Spam Keys List:";
  std::string btnDelete = "Delete";
  std::string btnAddKey = "Add Combat Key";
  std::string lblDelayMs = "ms";
  std::string lblShift = "Shift";
  std::string lblCtrl = "Ctrl";
  std::string lblAlt = "Alt";
  std::string lblFastLootHoldKey = "Fast Loot Hold Key: ";
  std::string lblFastLootClickKey = "Fast Loot Click Key: ";
  std::string lblFastLootTimer = "Fast Loot Timer (ms)";
  std::string chkAutoUpdate = "Auto update";
  std::string updateCurrentVersion = "Current version:";
  std::string updateStatus = "Update status:";
  std::string btnCheckUpdate = "Check";
  std::string btnDownloadUpdate = "Download";
  std::string btnInstallUpdate = "Install";
  std::string btnOpenRelease = "Release page";
};

struct ProfileConfig {
  std::string name = "Default";
  int combatMouseTrigger = 1;
  bool globalHealthCheckEnable = true;
  int healthVKey = 'Q';
  std::string healthKeyName = "Q";
  int healthDelayMs = 50;
  int healthX = 960;
  int healthY = 1010;
  int fastLootHoldVKey = 1;  // VK_LBUTTON by default (key to hold down)
  std::string fastLootHoldKeyName = "LButton";
  int fastLootClickVKey = 1;  // VK_LBUTTON by default (in-game loot key)
  std::string fastLootClickKeyName = "LButton";
  int fastLootDelayMs = 40;
  std::vector<SpamKey> spamKeys;
};
