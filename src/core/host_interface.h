#pragma once
#include "common/string.h"
#include "common/timer.h"
#include "settings.h"
#include "types.h"
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

enum LOGLEVEL;

class ByteStream;
class CDImage;
class HostDisplay;
class GameList;

struct SystemBootParameters;

namespace BIOS {
struct ImageInfo;
}

class HostInterface
{
public:
  HostInterface();
  virtual ~HostInterface();

  /// Initializes the emulator frontend.
  virtual bool Initialize();

  /// Shuts down the emulator frontend.
  virtual void Shutdown();

  /// Returns the base user directory path.
  ALWAYS_INLINE const std::string& GetUserDirectory() const { return m_user_directory; }

  /// Returns a path relative to the user directory.
  std::string GetUserDirectoryRelativePath(const char* format, ...) const printflike(2, 3);

  /// Returns a path relative to the application directory (for system files).
  std::string GetProgramDirectoryRelativePath(const char* format, ...) const printflike(2, 3);

  /// Returns a string which can be used as part of a filename, based on the current date/time.
  static TinyString GetTimestampStringForFileName();

  /// Displays a loading screen with the logo, rendered with ImGui. Use when executing possibly-time-consuming tasks
  /// such as compiling shaders when starting up.
  virtual void DisplayLoadingScreen(const char* message, int progress_min = -1, int progress_max = -1,
                                    int progress_value = -1)  = 0;

  /// Retrieves information about specified game from game list.
  virtual void GetGameInfo(const char* path, CDImage* image, std::string* code, std::string* title) = 0;

  /// Returns the directory where per-game memory cards will be saved.
  virtual std::string GetMemoryCardDirectory() const;

  /// Returns the default path to a memory card.
  virtual std::string GetSharedMemoryCardPath(u32 slot) const;

  /// Returns the default path to a memory card for a specific game.
  virtual std::string GetGameMemoryCardPath(const char* game_code, u32 slot) const;

  /// Returns the path to the shader cache directory.
  virtual std::string GetShaderCacheBasePath() const;

  /// Returns the path to the directory to search for BIOS images.
  virtual std::string GetBIOSDirectory();

  /// Loads the BIOS image for the specified region.
  std::optional<std::vector<u8>> GetBIOSImage(ConsoleRegion region);

  /// Searches for a BIOS image for the specified region in the specified directory. If no match is found, the first
  /// BIOS image within 512KB and 4MB will be used.
  std::optional<std::vector<u8>> FindBIOSImageInDirectory(ConsoleRegion region, const char* directory);

  /// Returns a list of filenames and descriptions for BIOS images in a directory.
  std::vector<std::pair<std::string, const BIOS::ImageInfo*>> FindBIOSImagesInDirectory(const char* directory);

  /// Returns true if any BIOS images are found in the configured BIOS directory.
  bool HasAnyBIOSImages();

  /// Opens a file in the DuckStation "package".
  /// This is the APK for Android builds, or the program directory for standalone builds.
  virtual std::unique_ptr<ByteStream> OpenPackageFile(const char* path, u32 flags) = 0;

  /// Called when achievements data is loaded.
  virtual void OnAchievementsRefreshed() = 0;

  /// Enables "relative" mouse mode, locking the cursor position and returning relative coordinates.
  virtual void SetMouseMode(bool relative, bool hide_cursor) = 0;

  /// Call when host display size changes, use with "match display" aspect ratio setting.
  virtual void OnHostDisplayResized();

  /// Sets the user directory to the program directory, i.e. "portable mode".
  void SetUserDirectoryToProgramDirectory();

  /// Quick switch between software and hardware rendering.
  void ToggleSoftwareRendering();

  /// Adjusts the internal (render) resolution of the hardware backends.
  void ModifyResolutionScale(s32 increment);

  /// Updates software cursor state, based on controllers.
  void UpdateSoftwareCursor();

  std::string m_program_directory;
  std::string m_user_directory;
};

extern HostInterface* g_host_interface;
