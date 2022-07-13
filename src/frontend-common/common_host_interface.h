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
  ALWAYS_INLINE GameList* GetGameList() const { return m_game_list.get(); }

  /// Returns true if running in batch mode, i.e. exit after emulation.
  ALWAYS_INLINE bool InBatchMode() const { return m_flags.batch_mode; }

  /// Parses command line parameters for all frontends.
  bool ParseCommandLineParameters(int argc, char* argv[], std::unique_ptr<SystemBootParameters>* out_boot_params);



  /// Displays a loading screen with the logo, rendered with ImGui. Use when executing possibly-time-consuming tasks
  /// such as compiling shaders when starting up.
  void DisplayLoadingScreen(const char* message, int progress_min = -1, int progress_max = -1,
                            int progress_value = -1) override;

  /// Retrieves information about specified game from game list.
  void GetGameInfo(const char* path, CDImage* image, std::string* code, std::string* title) override;

  /// Parses a fullscreen mode into its components (width * height @ refresh hz)
  static bool ParseFullscreenMode(const std::string_view& mode, u32* width, u32* height, float* refresh_rate);

  /// Converts a fullscreen mode to a string.
  static std::string GetFullscreenModeString(u32 width, u32 height, float refresh_rate);



  /// Requests the specified size for the render window. Not guaranteed to succeed (e.g. if in fullscreen).
  virtual bool RequestRenderWindowSize(s32 new_window_width, s32 new_window_height);

  /// Requests a resize to a multiple of the render window size.
  bool RequestRenderWindowScale(float scale);

  /// Returns a pointer to the top-level window, needed by some controller interfaces.
  virtual void* GetTopLevelWindowHandle() const;

  /// Called when achievements data is loaded.
  virtual void OnAchievementsRefreshed() override;

  /// Opens a file in the DuckStation "package".
  /// This is the APK for Android builds, or the program directory for standalone builds.
  virtual std::unique_ptr<ByteStream> OpenPackageFile(const char* path, u32 flags) override;

protected:
  enum : u32
  {
    SETTINGS_VERSION = 3
  };

  struct OSDMessage
  {
    std::string key;
    std::string text;
    Common::Timer time;
    float duration;
  };

  CommonHostInterface();
  ~CommonHostInterface();

  /// Returns the path of the settings file.
  std::string GetSettingsFileName() const;

  /// Sets the base path for the user directory. Can be overridden by platform/frontend/command line.
  virtual void SetUserDirectory();

  /// Increases timer resolution when supported by the host OS.
  void SetTimerResolutionIncreased(bool enabled);

  void OnHostDisplayResized() override;

  void ApplyGameSettings(bool display_osd_messages);
  void ApplyRendererFromGameSettings(const std::string& boot_filename);
  void ApplyControllerCompatibilitySettings(u64 controller_mask, bool display_osd_messages);

  bool CreateHostDisplayResources();
  void ReleaseHostDisplayResources();

  std::unique_ptr<GameList> m_game_list;

  std::unique_ptr<HostDisplayTexture> m_logo_texture;

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
}
