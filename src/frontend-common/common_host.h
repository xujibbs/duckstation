#pragma once
#include "core/system.h"
#include <mutex>

class SettingsInterface;

namespace CommonHost {
/// Parses command line parameters for all frontends.
bool ParseCommandLineParameters(int argc, char* argv[], std::unique_ptr<SystemBootParameters>* out_boot_params);

/// Initializes configuration.
void UpdateLogSettings();

bool Initialize();
void Shutdown();

void SetDefaultSettings(SettingsInterface& si);
void LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock);
void CheckForSettingsChanges(const Settings& old_settings);
void OnSystemStarting();
void OnSystemStarted();
void OnSystemDestroyed();
void OnSystemPaused();
void OnSystemResumed();
void OnGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name);
void PumpMessagesOnCPUThread();
bool CreateHostDisplayResources();
void ReleaseHostDisplayResources();
} // namespace CommonHost
