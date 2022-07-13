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

std::optional<std::vector<u8>> HostInterface::GetBIOSImage(ConsoleRegion region)
{
  std::string bios_dir = GetBIOSDirectory();
  std::string bios_name;
  switch (region)
  {
    case ConsoleRegion::NTSC_J:
      bios_name = Host::GetStringSettingValue("BIOS", "PathNTSCJ", "");
      break;

    case ConsoleRegion::PAL:
      bios_name = Host::GetStringSettingValue("BIOS", "PathPAL", "");
      break;

    case ConsoleRegion::NTSC_U:
    default:
      bios_name = Host::GetStringSettingValue("BIOS", "PathNTSCU", "");
      break;
  }

  if (bios_name.empty())
  {
    // auto-detect
    return FindBIOSImageInDirectory(region, bios_dir.c_str());
  }

  // try the configured path
  std::optional<BIOS::Image> image = BIOS::LoadImageFromFile(
    StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", bios_dir.c_str(), bios_name.c_str()).c_str());
  if (!image.has_value())
  {
    Host::ReportFormattedErrorAsync(
      "Error", Host::TranslateString("HostInterface", "Failed to load configured BIOS file '%s'"), bios_name.c_str());
    return std::nullopt;
  }

  BIOS::Hash found_hash = BIOS::GetHash(*image);
  Log_DevPrintf("Hash for BIOS '%s': %s", bios_name.c_str(), found_hash.ToString().c_str());

  if (!BIOS::IsValidHashForRegion(region, found_hash))
    Log_WarningPrintf("Hash for BIOS '%s' does not match region. This may cause issues.", bios_name.c_str());

  return image;
}

std::optional<std::vector<u8>> HostInterface::FindBIOSImageInDirectory(ConsoleRegion region, const char* directory)
{
  Log_InfoPrintf("Searching for a %s BIOS in '%s'...", Settings::GetConsoleRegionDisplayName(region), directory);

  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(
    directory, "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &results);

  std::string fallback_path;
  std::optional<BIOS::Image> fallback_image;
  const BIOS::ImageInfo* fallback_info = nullptr;

  for (const FILESYSTEM_FIND_DATA& fd : results)
  {
    if (fd.Size != BIOS::BIOS_SIZE && fd.Size != BIOS::BIOS_SIZE_PS2 && fd.Size != BIOS::BIOS_SIZE_PS3)
    {
      Log_WarningPrintf("Skipping '%s': incorrect size", fd.FileName.c_str());
      continue;
    }

    std::string full_path(
      StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", directory, fd.FileName.c_str()));

    std::optional<BIOS::Image> found_image = BIOS::LoadImageFromFile(full_path.c_str());
    if (!found_image)
      continue;

    BIOS::Hash found_hash = BIOS::GetHash(*found_image);
    Log_DevPrintf("Hash for BIOS '%s': %s", fd.FileName.c_str(), found_hash.ToString().c_str());

    const BIOS::ImageInfo* ii = BIOS::GetImageInfoForHash(found_hash);

    if (BIOS::IsValidHashForRegion(region, found_hash))
    {
      Log_InfoPrintf("Using BIOS '%s': %s", fd.FileName.c_str(), ii ? ii->description : "");
      return found_image;
    }

    // don't let an unknown bios take precedence over a known one
    if (!fallback_path.empty() && (fallback_info || !ii))
      continue;

    fallback_path = std::move(full_path);
    fallback_image = std::move(found_image);
    fallback_info = ii;
  }

  if (!fallback_image.has_value())
  {
    Host::ReportFormattedErrorAsync("Error",
                                    Host::TranslateString("HostInterface", "No BIOS image found for %s region"),
                                    Settings::GetConsoleRegionDisplayName(region));
    return std::nullopt;
  }

  if (!fallback_info)
  {
    Log_WarningPrintf("Using unknown BIOS '%s'. This may crash.", fallback_path.c_str());
  }
  else
  {
    Log_WarningPrintf("Falling back to possibly-incompatible image '%s': %s", fallback_path.c_str(),
                      fallback_info->description);
  }

  return fallback_image;
}

std::vector<std::pair<std::string, const BIOS::ImageInfo*>>
HostInterface::FindBIOSImagesInDirectory(const char* directory)
{
  std::vector<std::pair<std::string, const BIOS::ImageInfo*>> results;

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(directory, "*",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &files);

  for (FILESYSTEM_FIND_DATA& fd : files)
  {
    if (fd.Size != BIOS::BIOS_SIZE && fd.Size != BIOS::BIOS_SIZE_PS2 && fd.Size != BIOS::BIOS_SIZE_PS3)
      continue;

    std::string full_path(
      StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", directory, fd.FileName.c_str()));

    std::optional<BIOS::Image> found_image = BIOS::LoadImageFromFile(full_path.c_str());
    if (!found_image)
      continue;

    BIOS::Hash found_hash = BIOS::GetHash(*found_image);
    const BIOS::ImageInfo* ii = BIOS::GetImageInfoForHash(found_hash);
    results.emplace_back(std::move(fd.FileName), ii);
  }

  return results;
}

bool HostInterface::HasAnyBIOSImages()
{
  const std::string dir = GetBIOSDirectory();
  return (FindBIOSImageInDirectory(ConsoleRegion::Auto, dir.c_str()).has_value());
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

void HostInterface::OnHostDisplayResized()
{
  if (System::IsValid())
  {
    if (g_settings.gpu_widescreen_hack && g_settings.display_aspect_ratio == DisplayAspectRatio::MatchWindow)
      GTE::UpdateAspectRatio();
  }
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

TinyString HostInterface::GetTimestampStringForFileName()
{
  return TinyString::FromFmt("{:%Y-%m-%d_%H-%M-%S}", fmt::localtime(std::time(nullptr)));
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

void HostInterface::ToggleSoftwareRendering()
{
  if (System::IsShutdown() || g_settings.gpu_renderer == GPURenderer::Software)
    return;

  const GPURenderer new_renderer = g_gpu->IsHardwareRenderer() ? GPURenderer::Software : g_settings.gpu_renderer;

  Host::AddKeyedFormattedOSDMessage("SoftwareRendering", 5.0f,
                                    Host::TranslateString("OSDMessage", "Switching to %s renderer..."),
                                    Settings::GetRendererDisplayName(new_renderer));
  System::RecreateGPU(new_renderer);
  Host::InvalidateDisplay();
}

void HostInterface::ModifyResolutionScale(s32 increment)
{
  const u32 new_resolution_scale = std::clamp<u32>(
    static_cast<u32>(static_cast<s32>(g_settings.gpu_resolution_scale) + increment), 1, GPU::MAX_RESOLUTION_SCALE);
  if (new_resolution_scale == g_settings.gpu_resolution_scale)
    return;

  g_settings.gpu_resolution_scale = new_resolution_scale;

  if (!System::IsShutdown())
  {
    g_gpu->RestoreGraphicsAPIState();
    g_gpu->UpdateSettings();
    g_gpu->ResetGraphicsAPIState();
    System::ClearMemorySaveStates();
    Host::InvalidateDisplay();
  }
}

void HostInterface::UpdateSoftwareCursor()
{
  if (System::IsShutdown())
  {
    SetMouseMode(false, false);
    Host::GetHostDisplay()->ClearSoftwareCursor();
    return;
  }

  const Common::RGBA8Image* image = nullptr;
  float image_scale = 1.0f;
  bool relative_mode = false;
  bool hide_cursor = false;

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = System::GetController(i);
    if (controller && controller->GetSoftwareCursor(&image, &image_scale, &relative_mode))
    {
      hide_cursor = true;
      break;
    }
  }

  SetMouseMode(relative_mode, hide_cursor);

  if (image && image->IsValid())
  {
    Host::GetHostDisplay()->SetSoftwareCursor(image->GetPixels(), image->GetWidth(), image->GetHeight(), image->GetByteStride(),
                                 image_scale);
  }
  else
  {
    Host::GetHostDisplay()->ClearSoftwareCursor();
  }
}

