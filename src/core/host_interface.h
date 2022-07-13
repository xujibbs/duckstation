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

  /// Opens a file in the DuckStation "package".
  /// This is the APK for Android builds, or the program directory for standalone builds.
  virtual std::unique_ptr<ByteStream> OpenPackageFile(const char* path, u32 flags) = 0;

  /// Sets the user directory to the program directory, i.e. "portable mode".
  void SetUserDirectoryToProgramDirectory();

  std::string m_program_directory;
  std::string m_user_directory;
};

extern HostInterface* g_host_interface;
