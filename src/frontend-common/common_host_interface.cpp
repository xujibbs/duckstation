#include "common_host_interface.h"
#include "IconsFontAwesome5.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/crash_handler.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "core/cdrom.h"
#include "core/cheats.h"
#include "core/cpu_code_cache.h"
#include "core/dma.h"
#include "core/gpu.h"
#include "core/gte.h"
#include "core/host.h"
#include "core/host_display.h"
#include "core/host_settings.h"
#include "core/mdec.h"
#include "core/pgxp.h"
#include "core/save_state_version.h"
#include "core/settings.h"
#include "core/spu.h"
#include "core/system.h"
#include "core/texture_replacements.h"
#include "core/timers.h"
#include "fullscreen_ui.h"
#include "game_list.h"
#include "icon.h"
#include "imgui.h"
#include "imgui_fullscreen.h"
#include "imgui_manager.h"
#include "inhibit_screensaver.h"
#include "input_manager.h"
#include "input_overlay_ui.h"
#include "save_state_selector_ui.h"
#include "scmversion/scmversion.h"
#include "util/audio_stream.h"
#include "util/ini_settings_interface.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifndef _UWP
#include "cubeb_audio_stream.h"
#endif

#ifdef WITH_SDL2
#include "sdl_audio_stream.h"
#endif

#ifdef WITH_DISCORD_PRESENCE
#include "discord_rpc.h"
#endif

#ifdef WITH_CHEEVOS
#include "cheevos.h"
#endif

#ifdef _WIN32
#include "common/windows_headers.h"
#include "xaudio2_audio_stream.h"
#include <KnownFolders.h>
#include <ShlObj.h>
#include <mmsystem.h>
#endif

namespace FrontendCommon {

#ifdef _WIN32
std::unique_ptr<AudioStream> CreateXAudio2AudioStream();
#endif

} // namespace FrontendCommon

Log_SetChannel(CommonHostInterface);

namespace CommonHost {
static void UpdateLogSettings(LOGLEVEL level, const char* filter, bool log_to_console, bool log_to_debug,
                              bool log_to_window, bool log_to_file);
#ifdef WITH_DISCORD_PRESENCE
static void SetDiscordPresenceEnabled(bool enabled);
static void InitializeDiscordPresence();
static void ShutdownDiscordPresence();
static void UpdateDiscordPresence(bool rich_presence_only);
static void PollDiscordPresence();
#endif
#ifdef WITH_CHEEVOS
static void UpdateCheevosActive(SettingsInterface& si);
#endif
} // namespace CommonHost

static std::string s_settings_filename;
static std::unique_ptr<FrontendCommon::InputOverlayUI> s_input_overlay_ui;

#ifdef WITH_DISCORD_PRESENCE
// discord rich presence
bool m_discord_presence_enabled = false;
bool m_discord_presence_active = false;
#ifdef WITH_CHEEVOS
std::string m_discord_presence_cheevos_string;
#endif
#endif

CommonHostInterface::CommonHostInterface() = default;

CommonHostInterface::~CommonHostInterface() = default;

bool CommonHostInterface::Initialize()
{
  if (!HostInterface::Initialize())
    return false;

  InitializeUserDirectory();

  // Change to the user directory so that all default/relative paths in the config are after this.
  if (!FileSystem::SetWorkingDirectory(m_user_directory.c_str()))
    Log_ErrorPrintf("Failed to set working directory to '%s'", m_user_directory.c_str());

  // Set crash handler to dump to user directory, because of permissions.
  CrashHandler::SetWriteDirectory(m_user_directory);

  System::LoadSettings(false);
  CommonHost::UpdateLogSettings(
    g_settings.log_level, g_settings.log_filter.empty() ? nullptr : g_settings.log_filter.c_str(),
    g_settings.log_to_console, g_settings.log_to_debug, g_settings.log_to_window, g_settings.log_to_file);

  m_game_list = std::make_unique<GameList>();
  m_game_list->SetCacheFilename(GetUserDirectoryRelativePath("cache/gamelist.cache"));
  m_game_list->SetUserCompatibilityListFilename(GetUserDirectoryRelativePath("compatibility.xml"));
  m_game_list->SetUserGameSettingsFilename(GetUserDirectoryRelativePath("gamesettings.ini"));

#ifdef WITH_CHEEVOS
#ifdef WITH_RAINTEGRATION
  if (Host::GetBaseBoolSettingValue("Cheevos", "UseRAIntegration", false))
    Cheevos::SwitchToRAIntegration();
#endif

  CommonHost::UpdateCheevosActive(*Host::GetSettingsInterface());
#endif

  {
    auto lock = Host::GetSettingsLock();
    InputManager::ReloadSources(*Host::GetSettingsInterface(), lock);
  }

  return true;
}

void CommonHostInterface::Shutdown()
{
  s_input_overlay_ui.reset();

  HostInterface::Shutdown();

#ifdef WITH_DISCORD_PRESENCE
  CommonHost::ShutdownDiscordPresence();
#endif

#ifdef WITH_CHEEVOS
  Cheevos::Shutdown();
#endif

  InputManager::CloseSources();
}

void CommonHostInterface::InitializeUserDirectory()
{
  std::fprintf(stdout, "User directory: \"%s\"\n", m_user_directory.c_str());

  if (m_user_directory.empty())
    Panic("Cannot continue without user directory set.");

  if (!FileSystem::DirectoryExists(m_user_directory.c_str()))
  {
    std::fprintf(stderr, "User directory \"%s\" does not exist, creating.\n", m_user_directory.c_str());
    if (!FileSystem::CreateDirectory(m_user_directory.c_str(), true))
      std::fprintf(stderr, "Failed to create user directory \"%s\".\n", m_user_directory.c_str());
  }

  bool result = true;

  result &= FileSystem::EnsureDirectoryExists(GetUserDirectoryRelativePath("bios").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(GetUserDirectoryRelativePath("cache").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(
    GetUserDirectoryRelativePath("cache" FS_OSPATH_SEPARATOR_STR "achievement_badge").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(
    GetUserDirectoryRelativePath("cache" FS_OSPATH_SEPARATOR_STR "achievement_gameicon").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(GetUserDirectoryRelativePath("cheats").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(GetUserDirectoryRelativePath("covers").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(GetUserDirectoryRelativePath("dump").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(
    GetUserDirectoryRelativePath("dump" FS_OSPATH_SEPARATOR_STR "audio").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(
    GetUserDirectoryRelativePath("dump" FS_OSPATH_SEPARATOR_STR "textures").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(GetUserDirectoryRelativePath("inputprofiles").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(GetUserDirectoryRelativePath("memcards").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(GetUserDirectoryRelativePath("savestates").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(GetUserDirectoryRelativePath("screenshots").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(GetUserDirectoryRelativePath("shaders").c_str(), false);
  result &= FileSystem::EnsureDirectoryExists(GetUserDirectoryRelativePath("textures").c_str(), false);

  // Games directory for UWP because it's a pain to create them manually.
#ifdef _UWP
  result &= FileSystem::EnsureDirectoryExists(GetUserDirectoryRelativePath("games").c_str(), false);
#endif

  if (!result)
    Host::ReportErrorAsync("Error", "Failed to create one or more user directories. This may cause issues at runtime.");
}

static void PrintCommandLineVersion(const char* frontend_name)
{
  const bool was_console_enabled = Log::IsConsoleOutputEnabled();
  if (!was_console_enabled)
    Log::SetConsoleOutputParams(true);

  std::fprintf(stderr, "%s Version %s (%s)\n", frontend_name, g_scm_tag_str, g_scm_branch_str);
  std::fprintf(stderr, "https://github.com/stenzek/duckstation\n");
  std::fprintf(stderr, "\n");

  if (!was_console_enabled)
    Log::SetConsoleOutputParams(false);
}

static void PrintCommandLineHelp(const char* progname, const char* frontend_name)
{
  const bool was_console_enabled = Log::IsConsoleOutputEnabled();
  if (!was_console_enabled)
    Log::SetConsoleOutputParams(true);

  PrintCommandLineVersion(frontend_name);
  std::fprintf(stderr, "Usage: %s [parameters] [--] [boot filename]\n", progname);
  std::fprintf(stderr, "\n");
  std::fprintf(stderr, "  -help: Displays this information and exits.\n");
  std::fprintf(stderr, "  -version: Displays version information and exits.\n");
  std::fprintf(stderr, "  -batch: Enables batch mode (exits after powering off).\n");
  std::fprintf(stderr, "  -fastboot: Force fast boot for provided filename.\n");
  std::fprintf(stderr, "  -slowboot: Force slow boot for provided filename.\n");
  std::fprintf(stderr, "  -resume: Load resume save state. If a boot filename is provided,\n"
                       "    that game's resume state will be loaded, otherwise the most\n"
                       "    recent resume save state will be loaded.\n");
  std::fprintf(stderr, "  -state <index>: Loads specified save state by index. If a boot\n"
                       "    filename is provided, a per-game state will be loaded, otherwise\n"
                       "    a global state will be loaded.\n");
  std::fprintf(stderr, "  -statefile <filename>: Loads state from the specified filename.\n"
                       "    No boot filename is required with this option.\n");
  std::fprintf(stderr, "  -fullscreen: Enters fullscreen mode immediately after starting.\n");
  std::fprintf(stderr, "  -nofullscreen: Prevents fullscreen mode from triggering if enabled.\n");
  std::fprintf(stderr, "  -portable: Forces \"portable mode\", data in same directory.\n");
  std::fprintf(stderr, "  -nocontroller: Prevents the emulator from polling for controllers.\n"
                       "                 Try this option if you're having difficulties starting\n"
                       "                 the emulator.\n");
  std::fprintf(stderr, "  -settings <filename>: Loads a custom settings configuration from the\n"
                       "    specified filename. Default settings applied if file not found.\n");
  std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
                       "    parameters make up the filename. Use when the filename contains\n"
                       "    spaces or starts with a dash.\n");
  std::fprintf(stderr, "\n");

  if (!was_console_enabled)
    Log::SetConsoleOutputParams(false);
}

bool CommonHostInterface::ParseCommandLineParameters(int argc, char* argv[],
                                                     std::unique_ptr<SystemBootParameters>* out_boot_params)
{
  std::optional<bool> force_fast_boot;
  std::optional<bool> force_fullscreen;
  std::optional<s32> state_index;
  std::string state_filename;
  std::string boot_filename;
  bool no_more_args = false;

  for (int i = 1; i < argc; i++)
  {
    if (!no_more_args)
    {
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

      if (CHECK_ARG("-help"))
      {
        PrintCommandLineHelp(argv[0], GetFrontendName());
        return false;
      }
      else if (CHECK_ARG("-version"))
      {
        PrintCommandLineVersion(GetFrontendName());
        return false;
      }
      else if (CHECK_ARG("-batch"))
      {
        Log_InfoPrintf("Enabling batch mode.");
        m_flags.batch_mode = true;
        continue;
      }
      else if (CHECK_ARG("-fastboot"))
      {
        Log_InfoPrintf("Forcing fast boot.");
        force_fast_boot = true;
        continue;
      }
      else if (CHECK_ARG("-slowboot"))
      {
        Log_InfoPrintf("Forcing slow boot.");
        force_fast_boot = false;
        continue;
      }
      else if (CHECK_ARG("-nocontroller"))
      {
        Log_InfoPrintf("Disabling controller support.");
        m_flags.disable_controller_interface = true;
        continue;
      }
      else if (CHECK_ARG("-resume"))
      {
        state_index = -1;
        continue;
      }
      else if (CHECK_ARG_PARAM("-state"))
      {
        state_index = std::atoi(argv[++i]);
        continue;
      }
      else if (CHECK_ARG_PARAM("-statefile"))
      {
        state_filename = argv[++i];
        continue;
      }
      else if (CHECK_ARG("-fullscreen"))
      {
        Log_InfoPrintf("Going fullscreen after booting.");
        m_flags.start_fullscreen = true;
        force_fullscreen = true;
        continue;
      }
      else if (CHECK_ARG("-nofullscreen"))
      {
        Log_InfoPrintf("Preventing fullscreen after booting.");
        force_fullscreen = false;
        continue;
      }
      else if (CHECK_ARG("-portable"))
      {
        Log_InfoPrintf("Using portable mode.");
        SetUserDirectoryToProgramDirectory();
        continue;
      }
      else if (CHECK_ARG_PARAM("-resume"))
      {
        state_index = -1;
        continue;
      }
      else if (CHECK_ARG_PARAM("-settings"))
      {
        s_settings_filename = argv[++i];
        continue;
      }
      else if (CHECK_ARG("--"))
      {
        no_more_args = true;
        continue;
      }
      else if (argv[i][0] == '-')
      {
        Log_ErrorPrintf("Unknown parameter: '%s'", argv[i]);
        return false;
      }

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
    }

    if (!boot_filename.empty())
      boot_filename += ' ';
    boot_filename += argv[i];
  }

  if (state_index.has_value() || !boot_filename.empty() || !state_filename.empty())
  {
    // init user directory early since we need it for save states
    SetUserDirectory();

    if (state_index.has_value() && state_filename.empty())
    {
      // if a save state is provided, whether a boot filename was provided determines per-game/local
      if (boot_filename.empty())
      {
        // loading a global state. if this is -1, we're loading the most recent resume state
        if (*state_index < 0)
          state_filename = System::GetMostRecentResumeSaveStatePath();
        else
          state_filename = System::GetGlobalSaveStateFileName(*state_index);

        if (state_filename.empty() || !FileSystem::FileExists(state_filename.c_str()))
        {
          Log_ErrorPrintf("Could not find file for global save state %d", *state_index);
          return false;
        }
      }
      else
      {
        // find the game id, and get its save state path
        std::string game_code = System::GetGameCodeForPath(boot_filename.c_str(), true);
        if (game_code.empty())
        {
          Log_WarningPrintf("Could not identify game code for '%s', cannot load save state %d.", boot_filename.c_str(),
                            *state_index);
        }
        else
        {
          state_filename = System::GetGameSaveStateFileName(game_code.c_str(), *state_index);
          if (state_filename.empty() || !FileSystem::FileExists(state_filename.c_str()))
          {
            if (state_index >= 0) // Do not exit if -resume is specified, but resume save state does not exist
            {
              Log_ErrorPrintf("Could not find file for game '%s' save state %d", game_code.c_str(), *state_index);
              return false;
            }
            else
            {
              state_filename.clear();
            }
          }
        }
      }
    }

    std::unique_ptr<SystemBootParameters> boot_params = std::make_unique<SystemBootParameters>();
    boot_params->filename = std::move(boot_filename);
    boot_params->override_fast_boot = std::move(force_fast_boot);
    boot_params->override_fullscreen = std::move(force_fullscreen);

    if (!state_filename.empty())
    {
      std::unique_ptr<ByteStream> state_stream =
        ByteStream::OpenFile(state_filename.c_str(), BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
      if (!state_stream)
      {
        Log_ErrorPrintf("Failed to open save state file '%s'", state_filename.c_str());
        return false;
      }

      boot_params->state_stream = std::move(state_stream);
    }

    *out_boot_params = std::move(boot_params);
  }

  return true;
}

void CommonHostInterface::OnAchievementsRefreshed()
{
  // noop
}

void CommonHost::PumpMessagesOnCPUThread()
{
  InputManager::PollSources();

#ifdef WITH_DISCORD_PRESENCE
  PollDiscordPresence();
#endif

#ifdef WITH_CHEEVOS
  if (Cheevos::IsActive())
    Cheevos::Update();
#endif
}

bool CommonHostInterface::IsFullscreen() const
{
  return false;
}

bool CommonHostInterface::SetFullscreen(bool enabled)
{
  return false;
}

bool CommonHostInterface::CreateHostDisplayResources()
{
  m_logo_texture = FullscreenUI::LoadTextureResource("logo.png", false);
  if (!m_logo_texture)
    m_logo_texture = FullscreenUI::LoadTextureResource("duck.png", true);

  return true;
}

void CommonHostInterface::ReleaseHostDisplayResources()
{
  m_logo_texture.reset();
}

void CommonHostInterface::OnHostDisplayResized()
{
  HostInterface::OnHostDisplayResized();
  ImGuiManager::WindowResized();

  if (!System::IsShutdown())
    g_gpu->UpdateResolutionScale();
}

std::unique_ptr<AudioStream> Host::CreateAudioStream(AudioBackend backend)
{
  switch (backend)
  {
    case AudioBackend::Null:
      return AudioStream::CreateNullAudioStream();

#ifndef _UWP
    case AudioBackend::Cubeb:
      return CubebAudioStream::Create();
#endif

#ifdef _WIN32
    case AudioBackend::XAudio2:
      return FrontendCommon::CreateXAudio2AudioStream();
#endif

#ifdef WITH_SDL2
    case AudioBackend::SDL:
      return SDLAudioStream::Create();
#endif

    default:
      return nullptr;
  }
}

void CommonHost::UpdateLogSettings(LOGLEVEL level, const char* filter, bool log_to_console, bool log_to_debug,
                                   bool log_to_window, bool log_to_file)
{
  Log::SetFilterLevel(level);
  Log::SetConsoleOutputParams(g_settings.log_to_console, filter, level);
  Log::SetDebugOutputParams(g_settings.log_to_debug, filter, level);

  if (log_to_file)
  {
    Log::SetFileOutputParams(g_settings.log_to_file,
                             g_host_interface->GetUserDirectoryRelativePath("duckstation.log").c_str(), true, filter,
                             level);
  }
  else
  {
    Log::SetFileOutputParams(false, nullptr);
  }
}

void CommonHostInterface::SetUserDirectory()
{
  if (!m_user_directory.empty())
    return;

  std::fprintf(stdout, "Program directory \"%s\"\n", m_program_directory.c_str());

  if (FileSystem::FileExists(
        StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", m_program_directory.c_str(), "portable.txt")
          .c_str()) ||
      FileSystem::FileExists(
        StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", m_program_directory.c_str(), "settings.ini")
          .c_str()))
  {
    std::fprintf(stdout, "portable.txt or old settings.ini found, using program directory as user directory.\n");
    m_user_directory = m_program_directory;
  }
  else
  {
#if defined(_WIN32) && !defined(_UWP)
    // On Windows, use My Documents\DuckStation.
    PWSTR documents_directory;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &documents_directory)))
    {
      const std::string documents_directory_str(StringUtil::WideStringToUTF8String(documents_directory));
      if (!documents_directory_str.empty())
      {
        m_user_directory = StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s",
                                                           documents_directory_str.c_str(), "DuckStation");
      }
      CoTaskMemFree(documents_directory);
    }
#elif defined(__linux__) || defined(__FreeBSD__)
    // On Linux, use .local/share/duckstation as a user directory by default.
    const char* xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home && xdg_data_home[0] == '/')
    {
      m_user_directory = StringUtil::StdStringFromFormat("%s/duckstation", xdg_data_home);
    }
    else
    {
      const char* home_path = getenv("HOME");
      if (home_path)
        m_user_directory = StringUtil::StdStringFromFormat("%s/.local/share/duckstation", home_path);
    }
#elif defined(__APPLE__)
    // On macOS, default to ~/Library/Application Support/DuckStation.
    const char* home_path = getenv("HOME");
    if (home_path)
      m_user_directory = StringUtil::StdStringFromFormat("%s/Library/Application Support/DuckStation", home_path);
#endif

    if (m_user_directory.empty())
    {
      std::fprintf(stderr, "User directory path could not be determined, falling back to program directory.");
      m_user_directory = m_program_directory;
    }
  }
}

void CommonHost::OnSystemStarting()
{
  //
}

void CommonHost::OnSystemStarted()
{
  if (FullscreenUI::IsInitialized())
    FullscreenUI::SystemCreated();

  if (g_settings.inhibit_screensaver)
    FrontendCommon::SuspendScreensaver(Host::GetHostDisplay()->GetWindowInfo());
}

void CommonHost::OnSystemPaused()
{
#if 0
  if (IsFullscreen() && !FullscreenUI::IsInitialized())
    SetFullscreen(false);
#endif

  InputManager::PauseVibration();

  if (g_settings.inhibit_screensaver)
    FrontendCommon::ResumeScreensaver();
}

void CommonHost::OnSystemResumed()
{
  if (g_settings.inhibit_screensaver)
    FrontendCommon::SuspendScreensaver(Host::GetHostDisplay()->GetWindowInfo());
}

void CommonHost::OnSystemDestroyed()
{
  Host::ClearOSDMessages();

  if (FullscreenUI::IsInitialized())
    FullscreenUI::SystemDestroyed();

  InputManager::PauseVibration();

  if (g_settings.inhibit_screensaver)
    FrontendCommon::ResumeScreensaver();
}

void CommonHost::OnGameChanged(const std::string& disc_path, const std::string& game_serial,
                               const std::string& game_name)
{
#ifdef WITH_DISCORD_PRESENCE
  UpdateDiscordPresence(false);
#endif

#ifdef WITH_CHEEVOS
  // if (Cheevos::IsLoggedIn())
  // Cheevos::GameChanged(path, image);
#endif
}

std::string CommonHostInterface::GetSettingsFileName() const
{
  std::string filename;
  if (!s_settings_filename.empty())
    filename = s_settings_filename;
  else
    filename = GetUserDirectoryRelativePath("settings.ini");

  return filename;
}

void CommonHost::SetDefaultSettings(SettingsInterface& si)
{
  InputManager::SetDefaultConfig(si);

#ifdef WITH_DISCORD_PRESENCE
  si.SetBoolValue("Main", "EnableDiscordPresence", false);
#endif

#ifdef WITH_CHEEVOS
  si.SetBoolValue("Cheevos", "Enabled", false);
  si.SetBoolValue("Cheevos", "TestMode", false);
  si.SetBoolValue("Cheevos", "UnofficialTestMode", false);
  si.SetBoolValue("Cheevos", "UseFirstDiscFromPlaylist", true);
  si.DeleteValue("Cheevos", "Username");
  si.DeleteValue("Cheevos", "Token");

#ifdef WITH_RAINTEGRATION
  si.SetBoolValue("Cheevos", "UseRAIntegration", false);
#endif
#endif
}

void CommonHost::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
  InputManager::ReloadSources(si, lock);
  InputManager::ReloadBindings(si, *Host::GetSettingsInterfaceForBindings());

#ifdef WITH_DISCORD_PRESENCE
  SetDiscordPresenceEnabled(si.GetBoolValue("Main", "EnableDiscordPresence", false));
#endif

#ifdef WITH_CHEEVOS
  UpdateCheevosActive(si);
#endif

  if (FullscreenUI::IsInitialized())
    FullscreenUI::UpdateSettings();

  const bool input_display_enabled = si.GetBoolValue("Display", "ShowInputs", false);
  if (input_display_enabled && !s_input_overlay_ui)
    s_input_overlay_ui = std::make_unique<FrontendCommon::InputOverlayUI>();
  else if (!input_display_enabled && s_input_overlay_ui)
    s_input_overlay_ui.reset();
}

void CommonHost::CheckForSettingsChanges(const Settings& old_settings)
{
  if (System::IsValid())
  {
    if (g_settings.inhibit_screensaver != old_settings.inhibit_screensaver)
    {
      if (g_settings.inhibit_screensaver)
        FrontendCommon::SuspendScreensaver(Host::GetHostDisplay()->GetWindowInfo());
      else
        FrontendCommon::ResumeScreensaver();
    }
  }

  if (g_settings.log_level != old_settings.log_level || g_settings.log_filter != old_settings.log_filter ||
      g_settings.log_to_console != old_settings.log_to_console ||
      g_settings.log_to_debug != old_settings.log_to_debug || g_settings.log_to_window != old_settings.log_to_window ||
      g_settings.log_to_file != old_settings.log_to_file)
  {
    UpdateLogSettings(g_settings.log_level, g_settings.log_filter.empty() ? nullptr : g_settings.log_filter.c_str(),
                      g_settings.log_to_console, g_settings.log_to_debug, g_settings.log_to_window,
                      g_settings.log_to_file);
  }
}

void CommonHostInterface::SetTimerResolutionIncreased(bool enabled)
{
#if defined(_WIN32) && !defined(_UWP)
  static bool current_state = false;
  if (current_state == enabled)
    return;

  current_state = enabled;

  if (enabled)
    timeBeginPeriod(1);
  else
    timeEndPeriod(1);
#endif
}

void CommonHostInterface::DisplayLoadingScreen(const char* message, int progress_min /*= -1*/,
                                               int progress_max /*= -1*/, int progress_value /*= -1*/)
{
  const auto& io = ImGui::GetIO();
  const float scale = io.DisplayFramebufferScale.x;
  const float width = (400.0f * scale);
  const bool has_progress = (progress_min < progress_max);

  // eat the last imgui frame, it might've been partially rendered by the caller.
  ImGui::NewFrame();

  const float logo_width = 260.0f * scale;
  const float logo_height = 260.0f * scale;

  ImGui::SetNextWindowSize(ImVec2(logo_width, logo_height), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, (io.DisplaySize.y * 0.5f) - (50.0f * scale)),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::Begin("LoadingScreenLogo", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBackground))
  {
    if (m_logo_texture)
      ImGui::Image(m_logo_texture->GetHandle(), ImVec2(logo_width, logo_height));
  }
  ImGui::End();

  ImGui::SetNextWindowSize(ImVec2(width, (has_progress ? 50.0f : 30.0f) * scale), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, (io.DisplaySize.y * 0.5f) + (100.0f * scale)),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  if (ImGui::Begin("LoadingScreen", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing))
  {
    if (has_progress)
    {
      ImGui::Text("%s: %d/%d", message, progress_value, progress_max);
      ImGui::ProgressBar(static_cast<float>(progress_value) / static_cast<float>(progress_max - progress_min),
                         ImVec2(-1.0f, 0.0f), "");
      Log_InfoPrintf("%s: %d/%d", message, progress_value, progress_max);
    }
    else
    {
      const ImVec2 text_size(ImGui::CalcTextSize(message));
      ImGui::SetCursorPosX((width - text_size.x) / 2.0f);
      ImGui::TextUnformatted(message);
      Log_InfoPrintf("%s", message);
    }
  }
  ImGui::End();

  ImGui::EndFrame();
  Host::GetHostDisplay()->Render();
}

void CommonHostInterface::GetGameInfo(const char* path, CDImage* image, std::string* code, std::string* title)
{
  const GameListEntry* list_entry = m_game_list->GetEntryForPath(path);
  if (list_entry && list_entry->type != GameListEntryType::Playlist)
  {
    *code = list_entry->code;
    *title = list_entry->title;
    return;
  }

  if (image)
  {
    GameDatabaseEntry database_entry;
    if (m_game_list->GetDatabaseEntryForDisc(image, &database_entry))
    {
      *code = std::move(database_entry.serial);
      *title = std::move(database_entry.title);
      return;
    }

    *code = System::GetGameCodeForImage(image, true);
  }

  const std::string display_name(FileSystem::GetDisplayNameFromPath(path));
  *title = Path::GetFileTitle(display_name);
}

void CommonHostInterface::ApplyGameSettings(bool display_osd_messages)
{
  g_settings.controller_disable_analog_mode_forcing = false;

  // this gets called while booting, so can't use valid
  if (System::IsShutdown() || System::GetRunningCode().empty() || !g_settings.apply_game_settings)
    return;

  const GameListEntry* ge = m_game_list->GetEntryForPath(System::GetRunningPath().c_str());
  if (ge)
  {
    ApplyControllerCompatibilitySettings(ge->supported_controllers, display_osd_messages);
    ge->settings.ApplySettings(display_osd_messages);
  }
  else
  {
    GameDatabaseEntry db_entry;
    if (m_game_list->GetDatabaseEntryForCode(System::GetRunningCode(), &db_entry))
      ApplyControllerCompatibilitySettings(db_entry.supported_controllers_mask, display_osd_messages);

    const GameSettings::Entry* gs = m_game_list->GetGameSettingsForCode(System::GetRunningCode());
    if (gs)
      gs->ApplySettings(display_osd_messages);
  }
}

void CommonHostInterface::ApplyRendererFromGameSettings(const std::string& boot_filename)
{
  if (boot_filename.empty())
    return;

  // we can't use the code here, since it's not loaded yet. but we can cheekily access the game list
  const GameListEntry* ge = m_game_list->GetEntryForPath(boot_filename.c_str());
  if (ge && ge->settings.gpu_renderer.has_value() && ge->settings.gpu_renderer.value() != g_settings.gpu_renderer)
  {
    Log_InfoPrintf("Changing renderer from '%s' to '%s' due to game settings.",
                   Settings::GetRendererName(g_settings.gpu_renderer),
                   Settings::GetRendererName(ge->settings.gpu_renderer.value()));
    g_settings.gpu_renderer = ge->settings.gpu_renderer.value();
  }
}

void CommonHostInterface::ApplyControllerCompatibilitySettings(u64 controller_mask, bool display_osd_messages)
{
#define BIT_FOR(ctype) (static_cast<u64>(1) << static_cast<u32>(ctype))

  if (controller_mask == 0 || controller_mask == static_cast<u64>(-1))
    return;

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    const ControllerType ctype = g_settings.controller_types[i];
    if (ctype == ControllerType::None)
      continue;

    if (controller_mask & BIT_FOR(ctype))
      continue;

    // Special case: Dualshock is permitted when not supported as long as it's in digital mode.
    if (ctype == ControllerType::AnalogController &&
        (controller_mask & BIT_FOR(ControllerType::DigitalController)) != 0)
    {
      g_settings.controller_disable_analog_mode_forcing = true;
      continue;
    }

    if (display_osd_messages)
    {
      SmallString supported_controller_string;
      for (u32 j = 0; j < static_cast<u32>(ControllerType::Count); j++)
      {
        const ControllerType supported_ctype = static_cast<ControllerType>(j);
        if ((controller_mask & BIT_FOR(supported_ctype)) == 0)
          continue;

        if (!supported_controller_string.IsEmpty())
          supported_controller_string.AppendString(", ");

        supported_controller_string.AppendString(
          Host::TranslateString("ControllerType", Settings::GetControllerTypeDisplayName(supported_ctype)));
      }

      Host::AddFormattedOSDMessage(
        30.0f,
        Host::TranslateString("OSDMessage",
                              "Controller in port %u (%s) is not supported for %s.\nSupported controllers: "
                              "%s\nPlease configure a supported controller from the list above."),
        i + 1u, Host::TranslateString("ControllerType", Settings::GetControllerTypeDisplayName(ctype)).GetCharArray(),
        System::GetRunningTitle().c_str(), supported_controller_string.GetCharArray());
    }
  }

#undef BIT_FOR
}

#if 0
bool CommonHostInterface::UpdateControllerInputMapFromGameSettings()
{
  // this gets called while booting, so can't use valid
  if (System::IsShutdown() || System::GetRunningCode().empty() || !g_settings.apply_game_settings)
    return false;

  const GameSettings::Entry* gs = m_game_list->GetGameSettings(System::GetRunningPath(), System::GetRunningCode());
  if (!gs || gs->input_profile_name.empty())
    return false;

  std::string path = GetInputProfilePath(gs->input_profile_name.c_str());
  if (path.empty())
  {
    AddFormattedOSDMessage(10.0f, TranslateString("OSDMessage", "Input profile '%s' cannot be found."),
                           gs->input_profile_name.c_str());
    return false;
  }

  if (System::GetState() == System::State::Starting)
  {
    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Using input profile '%s'."),
                           gs->input_profile_name.c_str());
  }

  INISettingsInterface si(std::move(path));
  if (!si.Load())
  {
    AddFormattedOSDMessage(10.0f, TranslateString("OSDMessage", "Input profile '%s' cannot be loaded."),
                           gs->input_profile_name.c_str());
    return false;
  }

  UpdateControllerInputMap(si);
  return true;
}
#endif

bool CommonHostInterface::ParseFullscreenMode(const std::string_view& mode, u32* width, u32* height,
                                              float* refresh_rate)
{
  if (!mode.empty())
  {
    std::string_view::size_type sep1 = mode.find('x');
    if (sep1 != std::string_view::npos)
    {
      std::optional<u32> owidth = StringUtil::FromChars<u32>(mode.substr(0, sep1));
      sep1++;

      while (sep1 < mode.length() && std::isspace(mode[sep1]))
        sep1++;

      if (owidth.has_value() && sep1 < mode.length())
      {
        std::string_view::size_type sep2 = mode.find('@', sep1);
        if (sep2 != std::string_view::npos)
        {
          std::optional<u32> oheight = StringUtil::FromChars<u32>(mode.substr(sep1, sep2 - sep1));
          sep2++;

          while (sep2 < mode.length() && std::isspace(mode[sep2]))
            sep2++;

          if (oheight.has_value() && sep2 < mode.length())
          {
            std::optional<float> orefresh_rate = StringUtil::FromChars<float>(mode.substr(sep2));
            if (orefresh_rate.has_value())
            {
              *width = owidth.value();
              *height = oheight.value();
              *refresh_rate = orefresh_rate.value();
              return true;
            }
          }
        }
      }
    }
  }

  *width = 0;
  *height = 0;
  *refresh_rate = 0;
  return false;
}

std::string CommonHostInterface::GetFullscreenModeString(u32 width, u32 height, float refresh_rate)
{
  return StringUtil::StdStringFromFormat("%u x %u @ %f hz", width, height, refresh_rate);
}

bool CommonHostInterface::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
  return false;
}

bool CommonHostInterface::RequestRenderWindowScale(float scale)
{
  if (!System::IsValid() || scale == 0)
    return false;

  HostDisplay* display = Host::GetHostDisplay();
  const float y_scale =
    (static_cast<float>(display->GetDisplayWidth()) / static_cast<float>(display->GetDisplayHeight())) /
    display->GetDisplayAspectRatio();

  const u32 requested_width =
    std::max<u32>(static_cast<u32>(std::ceil(static_cast<float>(display->GetDisplayWidth()) * scale)), 1);
  const u32 requested_height =
    std::max<u32>(static_cast<u32>(std::ceil(static_cast<float>(display->GetDisplayHeight()) * y_scale * scale)), 1);

  return RequestRenderWindowSize(static_cast<s32>(requested_width), static_cast<s32>(requested_height));
}

void* CommonHostInterface::GetTopLevelWindowHandle() const
{
  return nullptr;
}

std::unique_ptr<ByteStream> CommonHostInterface::OpenPackageFile(const char* path, u32 flags)
{
  const u32 allowed_flags = (BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_SEEKABLE | BYTESTREAM_OPEN_STREAMED);
  const std::string full_path(
    StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", m_program_directory.c_str(), path));
  const u32 real_flags = (flags & allowed_flags) | BYTESTREAM_OPEN_READ;
  Log_DevPrintf("Requesting package file '%s'", path);
  return ByteStream::OpenFile(full_path.c_str(), real_flags);
}

#ifdef WITH_DISCORD_PRESENCE

void CommonHost::SetDiscordPresenceEnabled(bool enabled)
{
  if (m_discord_presence_enabled == enabled)
    return;

  m_discord_presence_enabled = enabled;
  if (enabled)
    InitializeDiscordPresence();
  else
    ShutdownDiscordPresence();
}

void CommonHost::InitializeDiscordPresence()
{
  if (m_discord_presence_active)
    return;

  DiscordEventHandlers handlers = {};
  Discord_Initialize("705325712680288296", &handlers, 0, nullptr);
  m_discord_presence_active = true;

  UpdateDiscordPresence(false);
}

void CommonHost::ShutdownDiscordPresence()
{
  if (!m_discord_presence_active)
    return;

  Discord_ClearPresence();
  Discord_Shutdown();
  m_discord_presence_active = false;
#ifdef WITH_CHEEVOS
  m_discord_presence_cheevos_string.clear();
#endif
}

void CommonHost::UpdateDiscordPresence(bool rich_presence_only)
{
  if (!m_discord_presence_active)
    return;

#ifdef WITH_CHEEVOS
  // Update only if RetroAchievements rich presence has changed
  const std::string& new_rich_presence = Cheevos::GetRichPresenceString();
  if (new_rich_presence == m_discord_presence_cheevos_string && rich_presence_only)
  {
    return;
  }
  m_discord_presence_cheevos_string = new_rich_presence;
#else
  if (rich_presence_only)
  {
    return;
  }
#endif

  // https://discord.com/developers/docs/rich-presence/how-to#updating-presence-update-presence-payload-fields
  DiscordRichPresence rp = {};
  rp.largeImageKey = "duckstation_logo";
  rp.largeImageText = "DuckStation PS1/PSX Emulator";
  rp.startTimestamp = std::time(nullptr);

  SmallString details_string;
  if (!System::IsShutdown())
  {
    details_string.AppendFormattedString("%s (%s)", System::GetRunningTitle().c_str(),
                                         System::GetRunningCode().c_str());
  }
  else
  {
    details_string.AppendString("No Game Running");
  }

#ifdef WITH_CHEEVOS
  SmallString state_string;
  // Trim to 128 bytes as per Discord-RPC requirements
  if (m_discord_presence_cheevos_string.length() >= 128)
  {
    // 124 characters + 3 dots + null terminator
    state_string = m_discord_presence_cheevos_string.substr(0, 124);
    state_string.AppendString("...");
  }
  else
  {
    state_string = m_discord_presence_cheevos_string;
  }

  rp.state = state_string;
#endif
  rp.details = details_string;

  Discord_UpdatePresence(&rp);
}

void CommonHost::PollDiscordPresence()
{
  if (!m_discord_presence_active)
    return;

  UpdateDiscordPresence(true);

  Discord_RunCallbacks();
}

#endif

#ifdef WITH_CHEEVOS

void CommonHost::UpdateCheevosActive(SettingsInterface& si)
{
  const bool cheevos_enabled = si.GetBoolValue("Cheevos", "Enabled", false);
  const bool cheevos_test_mode = si.GetBoolValue("Cheevos", "TestMode", false);
  const bool cheevos_unofficial_test_mode = si.GetBoolValue("Cheevos", "UnofficialTestMode", false);
  const bool cheevos_use_first_disc_from_playlist = si.GetBoolValue("Cheevos", "UseFirstDiscFromPlaylist", true);
  const bool cheevos_rich_presence = si.GetBoolValue("Cheevos", "RichPresence", true);
  const bool cheevos_hardcore = si.GetBoolValue("Cheevos", "ChallengeMode", false);

#ifdef WITH_RAINTEGRATION
  if (Cheevos::IsUsingRAIntegration())
    return;
#endif

  if (cheevos_enabled != Cheevos::IsActive() || cheevos_test_mode != Cheevos::IsTestModeActive() ||
      cheevos_unofficial_test_mode != Cheevos::IsUnofficialTestModeActive() ||
      cheevos_use_first_disc_from_playlist != Cheevos::IsUsingFirstDiscFromPlaylist() ||
      cheevos_rich_presence != Cheevos::IsRichPresenceEnabled() ||
      cheevos_hardcore != Cheevos::IsChallengeModeEnabled())
  {
    Cheevos::Shutdown();
    if (cheevos_enabled)
    {
      if (!Cheevos::Initialize(cheevos_test_mode, cheevos_use_first_disc_from_playlist, cheevos_rich_presence,
                               cheevos_hardcore, cheevos_unofficial_test_mode))
      {
        Host::ReportErrorAsync("Error", "Failed to initialize cheevos after settings change.");
      }
    }
  }
}

#endif

BEGIN_HOTKEY_LIST(g_common_hotkeys)
END_HOTKEY_LIST()

#if 0

void CommonHostInterface::RegisterGeneralHotkeys()
{
#ifndef __ANDROID__
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("OpenQuickMenu"),
    TRANSLATABLE("Hotkeys", "Open Quick Menu"), [this](bool pressed) {
    if (pressed && m_fullscreen_ui_enabled)
      FullscreenUI::OpenQuickMenu();
  });
#endif

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("FastForward"),
    TRANSLATABLE("Hotkeys", "Fast Forward"), [this](bool pressed) { SetFastForwardEnabled(pressed); });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("ToggleFastForward"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle Fast Forward")), [this](bool pressed) {
    if (pressed)
      SetFastForwardEnabled(!m_fast_forward_enabled);
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("Turbo"),
    TRANSLATABLE("Hotkeys", "Turbo"), [this](bool pressed) { SetTurboEnabled(pressed); });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("ToggleTurbo"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle Turbo")), [this](bool pressed) {
    if (pressed)
      SetTurboEnabled(!m_turbo_enabled);
  });
#ifndef __ANDROID__
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("ToggleFullscreen"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle Fullscreen")), [this](bool pressed) {
    if (pressed)
      SetFullscreen(!IsFullscreen());
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("TogglePause"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle Pause")), [this](bool pressed) {
    if (pressed && System::IsValid())
      PauseSystem(!System::IsPaused());
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("PowerOff"),
    StaticString(TRANSLATABLE("Hotkeys", "Power Off System")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      if (g_settings.confim_power_off && !InBatchMode())
      {
        SmallString confirmation_message(
          TranslateString("CommonHostInterface", "Are you sure you want to stop emulation?"));
        if (ShouldSaveResumeState())
        {
          confirmation_message.AppendString("\n\n");
          confirmation_message.AppendString(
            TranslateString("CommonHostInterface", "The current state will be saved."));
        }

        if (!ConfirmMessage(confirmation_message))
        {
          System::ResetPerformanceCounters();
          System::ResetThrottler();
          return;
        }
      }

      PowerOffSystem(ShouldSaveResumeState());
    }
  });

#endif

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("Screenshot"),
    StaticString(TRANSLATABLE("Hotkeys", "Save Screenshot")), [this](bool pressed) {
    if (pressed && System::IsValid())
      SaveScreenshot();
  });

#if !defined(__ANDROID__) && defined(WITH_CHEEVOS)
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("OpenAchievements"),
    StaticString(TRANSLATABLE("Hotkeys", "Open Achievement List")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      if (!m_fullscreen_ui_enabled || !FullscreenUI::OpenAchievementsWindow())
      {
        AddOSDMessage(
          TranslateStdString("OSDMessage", "Achievements are disabled or unavailable for this game."),
          10.0f);
      }
    }
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("OpenLeaderboards"),
    StaticString(TRANSLATABLE("Hotkeys", "Open Leaderboard List")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      if (!m_fullscreen_ui_enabled || !FullscreenUI::OpenLeaderboardsWindow())
      {
        AddOSDMessage(
          TranslateStdString("OSDMessage", "Leaderboards are disabled or unavailable for this game."),
          10.0f);
      }
    }
  });
#endif // !defined(__ANDROID__) && defined(WITH_CHEEVOS)
}

void CommonHostInterface::RegisterSystemHotkeys()
{
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "System")), StaticString("Reset"),
    StaticString(TRANSLATABLE("Hotkeys", "Reset System")), [this](bool pressed) {
    if (pressed && System::IsValid())
      ResetSystem();
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "System")), StaticString("ChangeDisc"),
    StaticString(TRANSLATABLE("Hotkeys", "Change Disc")), [](bool pressed) {
    if (pressed && System::IsValid() && System::HasMediaSubImages())
    {
      const u32 current = System::GetMediaSubImageIndex();
      const u32 next = (current + 1) % System::GetMediaSubImageCount();
      if (current != next)
        System::SwitchMediaSubImage(next);
    }
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "System")), StaticString("SwapMemoryCards"),
    StaticString(TRANSLATABLE("Hotkeys", "Swap Memory Card Slots")), [this](bool pressed) {
    if (pressed && System::IsValid())
      SwapMemoryCards();
  });

#ifndef __ANDROID__
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "System")), StaticString("FrameStep"),
    StaticString(TRANSLATABLE("Hotkeys", "Frame Step")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      if (!IsCheevosChallengeModeActive())
        DoFrameStep();
      else
        DisplayHotkeyBlockedByChallengeModeMessage();
    }
  });
#endif

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "System")), StaticString("Rewind"),
    StaticString(TRANSLATABLE("Hotkeys", "Rewind")), [this](bool pressed) { SetRewindState(pressed); });

#ifndef __ANDROID__
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "System")), StaticString("ToggleCheats"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle Cheats")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      if (!IsCheevosChallengeModeActive())
        DoToggleCheats();
      else
        DisplayHotkeyBlockedByChallengeModeMessage();
    }
  });
#else
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "System")), StaticString("TogglePatchCodes"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle Patch Codes")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      if (!IsCheevosChallengeModeActive())
        DoToggleCheats();
      else
        DisplayHotkeyBlockedByChallengeModeMessage();
    }
  });
#endif

  RegisterHotkey(
    StaticString(TRANSLATABLE("Hotkeys", "System")), StaticString("ToggleOverclocking"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle Clock Speed Control (Overclocking)")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      g_settings.cpu_overclock_enable = !g_settings.cpu_overclock_enable;
      g_settings.UpdateOverclockActive();
      System::UpdateOverclock();

      if (g_settings.cpu_overclock_enable)
      {
        const u32 percent = g_settings.GetCPUOverclockPercent();
        const double clock_speed =
          ((static_cast<double>(System::MASTER_CLOCK) * static_cast<double>(percent)) / 100.0) / 1000000.0;
        AddKeyedFormattedOSDMessage(
          "ToggleOverclocking", 5.0f,
          TranslateString("OSDMessage", "CPU clock speed control enabled (%u%% / %.3f MHz)."), percent, clock_speed);
      }
      else
      {
        AddKeyedFormattedOSDMessage("ToggleOverclocking", 5.0f,
          TranslateString("OSDMessage", "CPU clock speed control disabled (%.3f MHz)."),
          static_cast<double>(System::MASTER_CLOCK) / 1000000.0);
      }
    }
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "System")), StaticString("IncreaseEmulationSpeed"),
    StaticString(TRANSLATABLE("Hotkeys", "Increase Emulation Speed")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      g_settings.emulation_speed += 0.1f;
      UpdateSpeedLimiterState();
      AddKeyedFormattedOSDMessage("EmulationSpeedChange", 5.0f,
        TranslateString("OSDMessage", "Emulation speed set to %u%%."),
        static_cast<u32>(std::lround(g_settings.emulation_speed * 100.0f)));
    }
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "System")), StaticString("DecreaseEmulationSpeed"),
    StaticString(TRANSLATABLE("Hotkeys", "Decrease Emulation Speed")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      g_settings.emulation_speed = std::max(g_settings.emulation_speed - 0.1f, 0.1f);
      UpdateSpeedLimiterState();
      AddKeyedFormattedOSDMessage("EmulationSpeedChange", 5.0f,
        TranslateString("OSDMessage", "Emulation speed set to %u%%."),
        static_cast<u32>(std::lround(g_settings.emulation_speed * 100.0f)));
    }
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "System")), StaticString("ResetEmulationSpeed"),
    StaticString(TRANSLATABLE("Hotkeys", "Reset Emulation Speed")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      g_settings.emulation_speed = GetFloatSettingValue("Main", "EmulationSpeed", 1.0f);
      UpdateSpeedLimiterState();
      AddKeyedFormattedOSDMessage("EmulationSpeedChange", 5.0f,
        TranslateString("OSDMessage", "Emulation speed set to %u%%."),
        static_cast<u32>(std::lround(g_settings.emulation_speed * 100.0f)));
    }
  });
}

void CommonHostInterface::RegisterGraphicsHotkeys()
{
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("ToggleSoftwareRendering"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle Software Rendering")), [this](bool pressed) {
    if (pressed && System::IsValid())
      ToggleSoftwareRendering();
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("TogglePGXP"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle PGXP")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      g_settings.gpu_pgxp_enable = !g_settings.gpu_pgxp_enable;
      g_gpu->RestoreGraphicsAPIState();
      g_gpu->UpdateSettings();
      g_gpu->ResetGraphicsAPIState();
      System::ClearMemorySaveStates();
      AddKeyedOSDMessage("TogglePGXP",
        g_settings.gpu_pgxp_enable ?
        TranslateStdString("OSDMessage", "PGXP is now enabled.") :
        TranslateStdString("OSDMessage", "PGXP is now disabled."),
        5.0f);

      if (g_settings.gpu_pgxp_enable)
        PGXP::Initialize();
      else
        PGXP::Shutdown();

      // we need to recompile all blocks if pgxp is toggled on/off
      if (g_settings.IsUsingCodeCache())
        CPU::CodeCache::Flush();
    }
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("IncreaseResolutionScale"),
    StaticString(TRANSLATABLE("Hotkeys", "Increase Resolution Scale")), [this](bool pressed) {
    if (pressed && System::IsValid())
      ModifyResolutionScale(1);
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("DecreaseResolutionScale"),
    StaticString(TRANSLATABLE("Hotkeys", "Decrease Resolution Scale")), [this](bool pressed) {
    if (pressed && System::IsValid())
      ModifyResolutionScale(-1);
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("TogglePostProcessing"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle Post-Processing")), [this](bool pressed) {
    if (pressed && System::IsValid())
      TogglePostProcessing();
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("ReloadPostProcessingShaders"),
    StaticString(TRANSLATABLE("Hotkeys", "Reload Post Processing Shaders")), [this](bool pressed) {
    if (pressed && System::IsValid())
      ReloadPostProcessingShaders();
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("ReloadTextureReplacements"),
    StaticString(TRANSLATABLE("Hotkeys", "Reload Texture Replacements")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      AddKeyedOSDMessage("ReloadTextureReplacements",
        TranslateStdString("OSDMessage", "Texture replacements reloaded."), 10.0f);
      g_texture_replacements.Reload();
    }
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("ToggleWidescreen"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle Widescreen")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      ToggleWidescreen();
    }
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("TogglePGXPDepth"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle PGXP Depth Buffer")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      g_settings.gpu_pgxp_depth_buffer = !g_settings.gpu_pgxp_depth_buffer;
      if (!g_settings.gpu_pgxp_enable)
        return;

      g_gpu->RestoreGraphicsAPIState();
      g_gpu->UpdateSettings();
      g_gpu->ResetGraphicsAPIState();
      System::ClearMemorySaveStates();
      AddKeyedOSDMessage("TogglePGXPDepth",
        g_settings.gpu_pgxp_depth_buffer ?
        TranslateStdString("OSDMessage", "PGXP Depth Buffer is now enabled.") :
        TranslateStdString("OSDMessage", "PGXP Depth Buffer is now disabled."),
        5.0f);
    }
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("TogglePGXPCPU"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle PGXP CPU Mode")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      g_settings.gpu_pgxp_cpu = !g_settings.gpu_pgxp_cpu;
      if (!g_settings.gpu_pgxp_enable)
        return;

      g_gpu->RestoreGraphicsAPIState();
      g_gpu->UpdateSettings();
      g_gpu->ResetGraphicsAPIState();
      System::ClearMemorySaveStates();
      AddKeyedOSDMessage("TogglePGXPCPU",
        g_settings.gpu_pgxp_cpu ?
        TranslateStdString("OSDMessage", "PGXP CPU mode is now enabled.") :
        TranslateStdString("OSDMessage", "PGXP CPU mode is now disabled."),
        5.0f);

      PGXP::Shutdown();
      PGXP::Initialize();

      // we need to recompile all blocks if pgxp is toggled on/off
      if (g_settings.IsUsingCodeCache())
        CPU::CodeCache::Flush();
    }
  });
}

void CommonHostInterface::RegisterSaveStateHotkeys()
{
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")), StaticString("LoadSelectedSaveState"),
    StaticString(TRANSLATABLE("Hotkeys", "Load From Selected Slot")), [this](bool pressed) {
    if (pressed)
    {
      if (!IsCheevosChallengeModeActive())
        m_save_state_selector_ui->LoadCurrentSlot();
      else
        DisplayHotkeyBlockedByChallengeModeMessage();
    }
  });
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")), StaticString("SaveSelectedSaveState"),
    StaticString(TRANSLATABLE("Hotkeys", "Save To Selected Slot")), [this](bool pressed) {
    if (pressed)
      m_save_state_selector_ui->SaveCurrentSlot();
  });
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")), StaticString("SelectPreviousSaveStateSlot"),
    StaticString(TRANSLATABLE("Hotkeys", "Select Previous Save Slot")), [this](bool pressed) {
    if (pressed)
      m_save_state_selector_ui->SelectPreviousSlot();
  });
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")), StaticString("SelectNextSaveStateSlot"),
    StaticString(TRANSLATABLE("Hotkeys", "Select Next Save Slot")), [this](bool pressed) {
    if (pressed)
      m_save_state_selector_ui->SelectNextSlot();
  });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")), StaticString("UndoLoadState"),
    StaticString(TRANSLATABLE("Hotkeys", "Undo Load State")), [this](bool pressed) {
    if (pressed)
      UndoLoadState();
  });

  for (u32 slot = 1; slot <= PER_GAME_SAVE_STATE_SLOTS; slot++)
  {
    RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")),
      TinyString::FromFormat("LoadGameState%u", slot), TinyString::FromFormat("Load Game State %u", slot),
      [this, slot](bool pressed) {
      if (pressed)
      {
        if (!IsCheevosChallengeModeActive())
          LoadState(false, slot);
        else
          DisplayHotkeyBlockedByChallengeModeMessage();
      }
    });
    RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")),
      TinyString::FromFormat("SaveGameState%u", slot), TinyString::FromFormat("Save Game State %u", slot),
      [this, slot](bool pressed) {
      if (pressed)
        SaveState(false, slot);
    });
  }

  for (u32 slot = 1; slot <= GLOBAL_SAVE_STATE_SLOTS; slot++)
  {
    RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")),
      TinyString::FromFormat("LoadGlobalState%u", slot),
      TinyString::FromFormat("Load Global State %u", slot), [this, slot](bool pressed) {
      if (pressed)
      {
        if (!IsCheevosChallengeModeActive())
          LoadState(true, slot);
        else
          DisplayHotkeyBlockedByChallengeModeMessage();
      }
    });
    RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")),
      TinyString::FromFormat("SaveGlobalState%u", slot),
      TinyString::FromFormat("Save Global State %u", slot), [this, slot](bool pressed) {
      if (pressed)
        SaveState(true, slot);
    });
  }

  // Dummy strings for translation because we construct them in a loop.
  (void)TRANSLATABLE("Hotkeys", "Load Game State 1");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 2");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 3");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 4");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 5");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 6");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 7");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 8");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 9");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 10");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 1");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 2");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 3");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 4");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 5");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 6");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 7");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 8");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 9");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 10");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 1");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 2");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 3");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 4");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 5");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 6");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 7");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 8");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 9");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 10");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 1");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 2");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 3");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 4");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 5");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 6");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 7");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 8");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 9");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 10");
}

void CommonHostInterface::RegisterAudioHotkeys()
{
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Audio")), StaticString("AudioMute"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle Mute")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      g_settings.audio_output_muted = !g_settings.audio_output_muted;
      const s32 volume = GetAudioOutputVolume();
      m_audio_stream->SetOutputVolume(volume);
      if (g_settings.audio_output_muted)
      {
        AddKeyedOSDMessage("AudioControlHotkey", TranslateStdString("OSDMessage", "Volume: Muted"),
          2.0f);
      }
      else
      {
        AddKeyedFormattedOSDMessage("AudioControlHotkey", 2.0f,
          TranslateString("OSDMessage", "Volume: %d%%"), volume);
      }
    }
  });
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Audio")), StaticString("AudioCDAudioMute"),
    StaticString(TRANSLATABLE("Hotkeys", "Toggle CD Audio Mute")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      g_settings.cdrom_mute_cd_audio = !g_settings.cdrom_mute_cd_audio;
      AddKeyedOSDMessage("AudioControlHotkey",
        g_settings.cdrom_mute_cd_audio ?
        TranslateStdString("OSDMessage", "CD Audio Muted.") :
        TranslateStdString("OSDMessage", "CD Audio Unmuted."),
        2.0f);
    }
  });
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Audio")), StaticString("AudioVolumeUp"),
    StaticString(TRANSLATABLE("Hotkeys", "Volume Up")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      g_settings.audio_output_muted = false;

      const s32 volume = std::min<s32>(GetAudioOutputVolume() + 10, 100);
      g_settings.audio_output_volume = volume;
      g_settings.audio_fast_forward_volume = volume;
      m_audio_stream->SetOutputVolume(volume);
      AddKeyedFormattedOSDMessage("AudioControlHotkey", 2.0f,
        TranslateString("OSDMessage", "Volume: %d%%"), volume);
    }
  });
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Audio")), StaticString("AudioVolumeDown"),
    StaticString(TRANSLATABLE("Hotkeys", "Volume Down")), [this](bool pressed) {
    if (pressed && System::IsValid())
    {
      g_settings.audio_output_muted = false;

      const s32 volume = std::max<s32>(GetAudioOutputVolume() - 10, 0);
      g_settings.audio_output_volume = volume;
      g_settings.audio_fast_forward_volume = volume;
      m_audio_stream->SetOutputVolume(volume);
      AddKeyedFormattedOSDMessage("AudioControlHotkey", 2.0f,
        TranslateString("OSDMessage", "Volume: %d%%"), volume);
    }
  });
}

#endif