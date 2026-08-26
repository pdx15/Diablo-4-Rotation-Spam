#pragma once

#include <string>

enum class UpdatePhase {
  Idle,
  Checking,
  UpToDate,
  Available,
  Downloading,
  Downloaded,
  Installing,
  Failed
};

struct UpdateStatus {
  UpdatePhase phase = UpdatePhase::Idle;
  std::string message;
  std::string latestVersion;
  bool hasDownload = false;
  bool canInstall = false;
};

void StartUpdateCheck(bool autoDownload);
void StartUpdateDownload();
void InstallDownloadedUpdate();
void OpenLatestReleasePage();
UpdateStatus GetUpdateStatus();
bool IsUpdateBusy();
