#include "host_interface.h"
#include "bios.h"
#include "cdrom.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/image.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "controller.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "dma.h"
#include "fmt/chrono.h"
#include "gpu.h"
#include "gte.h"
#include "host.h"
#include "host_display.h"
#include "host_settings.h"
#include "pgxp.h"
#include "save_state_version.h"
#include "spu.h"
#include "system.h"
#include "texture_replacements.h"
#include "util/audio_stream.h"
#include <cmath>
#include <cstring>
#include <cwchar>
#include <stdlib.h>
Log_SetChannel(HostInterface);

HostInterface* g_host_interface;

HostInterface::HostInterface()
{
  Assert(!g_host_interface);
  g_host_interface = this;

  // we can get the program directory at construction time
  m_program_directory = Path::GetDirectory(FileSystem::GetProgramPath());
}

HostInterface::~HostInterface()
{
  // system should be shut down prior to the destructor
  Assert(g_host_interface == this);
  g_host_interface = nullptr;
}

bool HostInterface::Initialize()
{
  return true;
}

void HostInterface::Shutdown()
{
}

std::string HostInterface::GetBIOSDirectory()
{
  std::string dir = Host::GetStringSettingValue("BIOS", "SearchDirectory", "");
  if (!dir.empty())
    return dir;

  return GetUserDirectoryRelativePath("bios");
}


std::string HostInterface::GetShaderCacheBasePath() const
{
  return GetUserDirectoryRelativePath("cache/");
}

void HostInterface::SetUserDirectoryToProgramDirectory()
{
  std::string program_path(FileSystem::GetProgramPath());
  if (program_path.empty())
    Panic("Failed to get program path.");

  std::string program_directory(Path::GetDirectory(program_path));
  if (program_directory.empty())
    Panic("Program path is not valid");

  m_user_directory = std::move(program_directory);
}

std::string HostInterface::GetUserDirectoryRelativePath(const char* format, ...) const
{
  std::va_list ap;
  va_start(ap, format);
  std::string formatted_path = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  if (m_user_directory.empty())
  {
    return formatted_path;
  }
  else
  {
    return StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", m_user_directory.c_str(),
                                           formatted_path.c_str());
  }
}

std::string HostInterface::GetProgramDirectoryRelativePath(const char* format, ...) const
{
  std::va_list ap;
  va_start(ap, format);
  std::string formatted_path = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  if (m_program_directory.empty())
  {
    return formatted_path;
  }
  else
  {
    return StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", m_program_directory.c_str(),
                                           formatted_path.c_str());
  }
}

std::string HostInterface::GetMemoryCardDirectory() const
{
  if (g_settings.memory_card_directory.empty())
    return GetUserDirectoryRelativePath("memcards");
  else
    return g_settings.memory_card_directory;
}

std::string HostInterface::GetSharedMemoryCardPath(u32 slot) const
{
  if (g_settings.memory_card_directory.empty())
  {
    return GetUserDirectoryRelativePath("memcards" FS_OSPATH_SEPARATOR_STR "shared_card_%u.mcd", slot + 1);
  }
  else
  {
    return StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "shared_card_%u.mcd",
                                           g_settings.memory_card_directory.c_str(), slot + 1);
  }
}

std::string HostInterface::GetGameMemoryCardPath(const char* game_code, u32 slot) const
{
  if (g_settings.memory_card_directory.empty())
  {
    return GetUserDirectoryRelativePath("memcards" FS_OSPATH_SEPARATOR_STR "%s_%u.mcd", game_code, slot + 1);
  }
  else
  {
    return StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s_%u.mcd",
                                           g_settings.memory_card_directory.c_str(), game_code, slot + 1);
  }
}
