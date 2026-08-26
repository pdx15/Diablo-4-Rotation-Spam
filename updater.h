#pragma once

#include <cstdint>
#include <string>

enum class UpdatePhase {
  Idle,
  Checking,
  UpToDate,
  Available,
  Downloading,
  Installing,
  Failed,
  Restarting
};

struct UpdateStatus {
  UpdatePhase phase = UpdatePhase::Idle;
  std::string message;
  std::string latestVersion;
  float downloadProgress = 0.0f;
  std::uint64_t downloadedBytes = 0;
  std::uint64_t totalBytes = 0;
};

void CleanupUpdateArtifacts();
// Blocks until the process that launched us (the pre-update build) is gone, so
// its executable can be replaced/cleaned up safely.
void WaitForPreviousInstance(unsigned long processId);
void StartUpdateCheck();
void StartUpdateProcess();
void OpenLatestReleasePage();
UpdateStatus GetUpdateStatus();
bool IsUpdateBusy();
