#pragma once
#include "common/bitfield.h"
#include "common/string.h"
#include "core/controller.h"
#include "core/host_interface.h"
#include <atomic>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

class HostDisplayTexture;

class GameList;
struct GameDatabaseEntry;

namespace FrontendCommon {
class SaveStateSelectorUI;
} // namespace FrontendCommon

class CommonHostInterface : public HostInterface
{
public:

  /// Returns the name of the frontend.
  virtual const char* GetFrontendName() const = 0;

  /// Request the frontend to exit.
  virtual void RequestExit() = 0;

  /// Runs an event next frame as part of the event loop.
  virtual void RunLater(std::function<void()> func) = 0;

  virtual bool IsFullscreen() const;
  virtual bool SetFullscreen(bool enabled);

  virtual bool Initialize() override;

  virtual void Shutdown() override;

  /// Returns the game list.
  GameList* GetGameList() const;

  /// Returns true if running in batch mode, i.e. exit after emulation.
  ALWAYS_INLINE bool InBatchMode() const { return m_flags.batch_mode; }

  /// Parses command line parameters for all frontends.
  bool ParseCommandLineParameters(int argc, char* argv[], std::unique_ptr<SystemBootParameters>* out_boot_params);

  /// Opens a file in the DuckStation "package".
  /// This is the APK for Android builds, or the program directory for standalone builds.
  virtual std::unique_ptr<ByteStream> OpenPackageFile(const char* path, u32 flags) override;

protected:
  enum : u32
  {
    SETTINGS_VERSION = 3
  };

  CommonHostInterface();
  ~CommonHostInterface();

  /// Returns the path of the settings file.
  std::string GetSettingsFileName() const;

  /// Sets the base path for the user directory. Can be overridden by platform/frontend/command line.
  virtual void SetUserDirectory();

  /// Increases timer resolution when supported by the host OS.
  void SetTimerResolutionIncreased(bool enabled);

  void ApplyGameSettings(bool display_osd_messages);
  void ApplyControllerCompatibilitySettings(u64 controller_mask, bool display_osd_messages);

  union
  {
    u8 bits;

    // running in batch mode? i.e. exit after stopping emulation
    BitField<u8, bool, 0, 1> batch_mode;

    // disable controller interface (buggy devices with SDL)
    BitField<u8, bool, 1, 1> disable_controller_interface;

    // starting fullscreen (outside of boot options)
    BitField<u8, bool, 2, 1> start_fullscreen;

    // force fullscreen UI enabled (nogui)
    BitField<u8, bool, 3, 1> force_fullscreen_ui;
  } m_flags = {};

private:
  void InitializeUserDirectory();
};

namespace CommonHost
{
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
}
