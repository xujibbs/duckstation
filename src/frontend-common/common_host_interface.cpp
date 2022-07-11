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
#include "core/host_settings.h"
#include "core/host_display.h"
#include "imgui_fullscreen.h"
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
#include "imgui_manager.h"
#include "inhibit_screensaver.h"
#include "ini_settings_interface.h"
#include "input_manager.h"
#include "input_overlay_ui.h"
#include "save_state_selector_ui.h"
#include "scmversion/scmversion.h"
#include "util/audio_stream.h"
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

static std::string s_settings_filename;
static std::unique_ptr<FrontendCommon::InputOverlayUI> s_input_overlay_ui;

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

  LoadSettings(*Host::GetSettingsInterface());
  FixIncompatibleSettings(false);
  UpdateLogSettings(g_settings.log_level, g_settings.log_filter.empty() ? nullptr : g_settings.log_filter.c_str(),
                    g_settings.log_to_console, g_settings.log_to_debug, g_settings.log_to_window,
                    g_settings.log_to_file);

  m_game_list = std::make_unique<GameList>();
  m_game_list->SetCacheFilename(GetUserDirectoryRelativePath("cache/gamelist.cache"));
  m_game_list->SetUserCompatibilityListFilename(GetUserDirectoryRelativePath("compatibility.xml"));
  m_game_list->SetUserGameSettingsFilename(GetUserDirectoryRelativePath("gamesettings.ini"));

  m_save_state_selector_ui = std::make_unique<FrontendCommon::SaveStateSelectorUI>(this);

#ifdef WITH_CHEEVOS
#ifdef WITH_RAINTEGRATION
  if (GetBoolSettingValue("Cheevos", "UseRAIntegration", false))
    Cheevos::SwitchToRAIntegration();
#endif

  UpdateCheevosActive(*Host::GetSettingsInterface());
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
  ShutdownDiscordPresence();
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

  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("bios").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("cache").c_str(), false);
  result &= FileSystem::CreateDirectory(
    GetUserDirectoryRelativePath("cache" FS_OSPATH_SEPARATOR_STR "achievement_badge").c_str(), false);
  result &= FileSystem::CreateDirectory(
    GetUserDirectoryRelativePath("cache" FS_OSPATH_SEPARATOR_STR "achievement_gameicon").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("cheats").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("covers").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("dump").c_str(), false);
  result &=
    FileSystem::CreateDirectory(GetUserDirectoryRelativePath("dump" FS_OSPATH_SEPARATOR_STR "audio").c_str(), false);
  result &=
    FileSystem::CreateDirectory(GetUserDirectoryRelativePath("dump" FS_OSPATH_SEPARATOR_STR "textures").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("inputprofiles").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("memcards").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("savestates").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("screenshots").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("shaders").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("textures").c_str(), false);

  // Games directory for UWP because it's a pain to create them manually.
#ifdef _UWP
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("games").c_str(), false);
#endif

  if (!result)
    ReportError("Failed to create one or more user directories. This may cause issues at runtime.");
}

bool CommonHostInterface::BootSystem(std::shared_ptr<SystemBootParameters> parameters)
{
  // If the fullscreen UI is enabled, make sure it's finished loading the game list so we don't race it.
  if (m_display && FullscreenUI::IsInitialized())
    FullscreenUI::EnsureGameListLoaded();

  // In Challenge mode, do not allow loading a save state under any circumstances
  // If it's present, drop it
  if (IsCheevosChallengeModeActive())
    parameters->state_stream.reset();

  ApplyRendererFromGameSettings(parameters->filename);

  if (!HostInterface::BootSystem(parameters))
  {
    // if in batch mode, exit immediately if booting failed
    if (InBatchMode())
      RequestExit();

    return false;
  }

  // enter fullscreen if requested in the parameters
  if (!g_settings.start_paused && ((parameters->override_fullscreen.has_value() && *parameters->override_fullscreen) ||
                                   (!parameters->override_fullscreen.has_value() && g_settings.start_fullscreen)))
  {
    SetFullscreen(true);
  }

  if (g_settings.audio_dump_on_boot)
    StartDumpingAudio();

  UpdateSpeedLimiterState();
  return true;
}

void CommonHostInterface::DestroySystem()
{
  m_undo_load_state.reset();
  SetTimerResolutionIncreased(false);
  m_save_state_selector_ui->Close();
  m_display->SetPostProcessingChain({});

  HostInterface::DestroySystem();
}

void CommonHostInterface::PowerOffSystem(bool save_resume_state)
{
  if (System::IsShutdown())
    return;

  if (save_resume_state)
    SaveResumeSaveState();

  DestroySystem();

  if (InBatchMode())
    RequestExit();
}

void CommonHostInterface::ResetSystem()
{
  HostInterface::ResetSystem();
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
          state_filename = GetMostRecentResumeSaveStatePath();
        else
          state_filename = GetGlobalSaveStateFileName(*state_index);

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
          state_filename = GetGameSaveStateFileName(game_code.c_str(), *state_index);
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

void CommonHostInterface::PollAndUpdate()
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

std::unique_ptr<AudioStream> CommonHostInterface::CreateAudioStream(AudioBackend backend)
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

s32 CommonHostInterface::GetAudioOutputVolume() const
{
  return g_settings.GetAudioOutputVolume(IsRunningAtNonStandardSpeed());
}

bool CommonHostInterface::UndoLoadState()
{
  if (!m_undo_load_state)
    return false;

  Assert(System::IsValid());

  m_undo_load_state->SeekAbsolute(0);
  if (!System::LoadState(m_undo_load_state.get()))
  {
    ReportError("Failed to load undo state, resetting system.");
    m_undo_load_state.reset();
    ResetSystem();
    return false;
  }

  System::ResetPerformanceCounters();
  System::ResetThrottler();

  Log_InfoPrintf("Loaded undo save state.");
  m_undo_load_state.reset();
  return true;
}

bool CommonHostInterface::SaveUndoLoadState()
{
  if (m_undo_load_state)
    m_undo_load_state.reset();

  m_undo_load_state = ByteStream::CreateGrowableMemoryStream(nullptr, System::MAX_SAVE_STATE_SIZE);
  if (!System::SaveState(m_undo_load_state.get()))
  {
    AddOSDMessage(TranslateStdString("OSDMessage", "Failed to save undo load state."), 15.0f);
    m_undo_load_state.reset();
    return false;
  }

  Log_InfoPrintf("Saved undo load state: %" PRIu64 " bytes", m_undo_load_state->GetSize());
  return true;
}

bool CommonHostInterface::LoadState(const char* filename)
{
  const bool system_was_valid = System::IsValid();
  if (system_was_valid)
    SaveUndoLoadState();

  const bool result = HostInterface::LoadState(filename);
  if (system_was_valid || !result)
  {
#ifdef WITH_CHEEVOS
    Cheevos::Reset();
#endif
  }

  if (!result && CanUndoLoadState())
    UndoLoadState();

  return result;
}

bool CommonHostInterface::LoadState(bool global, s32 slot)
{
  if (!global && (System::IsShutdown() || System::GetRunningCode().empty()))
  {
    ReportFormattedError("Can't save per-game state without a running game code.");
    return false;
  }

  std::string save_path =
    global ? GetGlobalSaveStateFileName(slot) : GetGameSaveStateFileName(System::GetRunningCode().c_str(), slot);
  return LoadState(save_path.c_str());
}

bool CommonHostInterface::SaveState(bool global, s32 slot)
{
  const std::string& code = System::GetRunningCode();
  if (!global && code.empty())
  {
    ReportFormattedError("Can't save per-game state without a running game code.");
    return false;
  }

  std::string save_path = global ? GetGlobalSaveStateFileName(slot) : GetGameSaveStateFileName(code.c_str(), slot);
  RenameCurrentSaveStateToBackup(save_path.c_str());
  return SaveState(save_path.c_str());
}

bool CommonHostInterface::CanResumeSystemFromFile(const char* filename)
{
  if (GetBoolSettingValue("Main", "SaveStateOnExit", true) && !IsCheevosChallengeModeActive())
  {
    const GameListEntry* entry = m_game_list->GetEntryForPath(filename);
    if (entry)
      return !entry->code.empty();
    else
      return !System::GetGameCodeForPath(filename, true).empty();
  }

  return false;
}

bool CommonHostInterface::ResumeSystemFromState(const char* filename, bool boot_on_failure)
{
  if (!BootSystem(std::make_shared<SystemBootParameters>(filename)))
    return false;

  const bool global = System::GetRunningCode().empty();
  if (global)
  {
    ReportFormattedError("Cannot resume system with undetectable game code from '%s'.", filename);
    if (!boot_on_failure)
    {
      DestroySystem();
      return true;
    }
  }
  else
  {
    const std::string path = GetGameSaveStateFileName(System::GetRunningCode().c_str(), -1);
    if (FileSystem::FileExists(path.c_str()))
    {
      if (!LoadState(path.c_str()) && !boot_on_failure)
      {
        DestroySystem();
        return false;
      }
    }
    else if (!boot_on_failure)
    {
      ReportFormattedError("Resume save state not found for '%s' ('%s').", System::GetRunningCode().c_str(),
                           System::GetRunningTitle().c_str());
      DestroySystem();
      return false;
    }
  }

  return true;
}

bool CommonHostInterface::ResumeSystemFromMostRecentState()
{
  const std::string path = GetMostRecentResumeSaveStatePath();
  if (path.empty())
  {
    ReportError("No resume save state found.");
    return false;
  }

  return LoadState(path.c_str());
}

bool CommonHostInterface::ShouldSaveResumeState() const
{
  return g_settings.save_state_on_exit;
}

bool CommonHostInterface::IsRunningAtNonStandardSpeed() const
{
  if (!System::IsValid())
    return false;

  const float target_speed = System::GetTargetSpeed();
  return (target_speed <= 0.95f || target_speed >= 1.05f);
}

void CommonHostInterface::UpdateSpeedLimiterState()
{
  float target_speed = m_turbo_enabled ?
                         g_settings.turbo_speed :
                         (m_fast_forward_enabled ? g_settings.fast_forward_speed : g_settings.emulation_speed);
  m_throttler_enabled = (target_speed != 0.0f);
  m_display_all_frames = !m_throttler_enabled || g_settings.display_all_frames;

  bool syncing_to_host = false;
  if (g_settings.sync_to_host_refresh_rate && g_settings.audio_resampling && target_speed == 1.0f && m_display &&
      System::IsRunning())
  {
    float host_refresh_rate;
    if (m_display->GetHostRefreshRate(&host_refresh_rate))
    {
      const float ratio = host_refresh_rate / System::GetThrottleFrequency();
      syncing_to_host = (ratio >= 0.95f && ratio <= 1.05f);
      Log_InfoPrintf("Refresh rate: Host=%fhz Guest=%fhz Ratio=%f - %s", host_refresh_rate,
                     System::GetThrottleFrequency(), ratio, syncing_to_host ? "can sync" : "can't sync");
      if (syncing_to_host)
        target_speed *= ratio;
    }
  }

  const bool is_non_standard_speed = (std::abs(target_speed - 1.0f) > 0.05f);
  const bool audio_sync_enabled =
    !System::IsRunning() || (m_throttler_enabled && g_settings.audio_sync_enabled && !is_non_standard_speed);
  const bool video_sync_enabled =
    !System::IsRunning() || (m_throttler_enabled && g_settings.video_sync_enabled && !is_non_standard_speed);
  const float max_display_fps = (!System::IsRunning() || m_throttler_enabled) ? 0.0f : g_settings.display_max_fps;
  Log_InfoPrintf("Target speed: %f%%", target_speed * 100.0f);
  Log_InfoPrintf("Syncing to %s%s", audio_sync_enabled ? "audio" : "",
                 (audio_sync_enabled && video_sync_enabled) ? " and video" : (video_sync_enabled ? "video" : ""));
  Log_InfoPrintf("Max display fps: %f (%s)", max_display_fps,
                 m_display_all_frames ? "displaying all frames" : "skipping displaying frames when needed");

  if (System::IsValid())
  {
    System::SetTargetSpeed(target_speed);
    System::ResetThrottler();
  }

  if (m_audio_stream)
  {
    const u32 input_sample_rate = (target_speed == 0.0f || !g_settings.audio_resampling) ?
                                    AUDIO_SAMPLE_RATE :
                                    static_cast<u32>(static_cast<float>(AUDIO_SAMPLE_RATE) * target_speed);
    Log_InfoPrintf("Audio input sample rate: %u hz", input_sample_rate);

    m_audio_stream->SetInputSampleRate(input_sample_rate);
    m_audio_stream->SetWaitForBufferFill(true);

    if (g_settings.audio_fast_forward_volume != g_settings.audio_output_volume)
      m_audio_stream->SetOutputVolume(GetAudioOutputVolume());

    m_audio_stream->SetSync(audio_sync_enabled);
    if (audio_sync_enabled)
      m_audio_stream->EmptyBuffers();
  }

  if (m_display)
  {
    m_display->SetDisplayMaxFPS(max_display_fps);
    m_display->SetVSync(video_sync_enabled);
  }

  if (g_settings.increase_timer_resolution)
    SetTimerResolutionIncreased(m_throttler_enabled);

  // When syncing to host and using vsync, we don't need to sleep.
  if (syncing_to_host && video_sync_enabled && m_display_all_frames)
  {
    Log_InfoPrintf("Using host vsync for throttling.");
    m_throttler_enabled = false;
  }
}

void CommonHostInterface::RecreateSystem()
{
  const bool was_paused = System::IsPaused();
  HostInterface::RecreateSystem();
  if (was_paused)
    PauseSystem(true);
}

void CommonHostInterface::UpdateLogSettings(LOGLEVEL level, const char* filter, bool log_to_console, bool log_to_debug,
                                            bool log_to_window, bool log_to_file)
{
  Log::SetFilterLevel(level);
  Log::SetConsoleOutputParams(g_settings.log_to_console, filter, level);
  Log::SetDebugOutputParams(g_settings.log_to_debug, filter, level);

  if (log_to_file)
  {
    Log::SetFileOutputParams(g_settings.log_to_file, GetUserDirectoryRelativePath("duckstation.log").c_str(), true,
                             filter, level);
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

void CommonHostInterface::OnSystemCreated()
{
  if (FullscreenUI::IsInitialized())
    FullscreenUI::SystemCreated();

  if (g_settings.display_post_processing && !m_display->SetPostProcessingChain(g_settings.display_post_process_chain))
    AddOSDMessage(TranslateStdString("OSDMessage", "Failed to load post processing shader chain."), 20.0f);

  if (g_settings.inhibit_screensaver)
    FrontendCommon::SuspendScreensaver(m_display->GetWindowInfo());
}

void CommonHostInterface::OnSystemPaused(bool paused)
{
  if (paused)
  {
    if (IsFullscreen() && !FullscreenUI::IsInitialized())
      SetFullscreen(false);

    InputManager::PauseVibration();
    FrontendCommon::ResumeScreensaver();
  }
  else
  {
    if (g_settings.inhibit_screensaver)
      FrontendCommon::SuspendScreensaver(m_display->GetWindowInfo());
  }

  UpdateSpeedLimiterState();
}

void CommonHostInterface::OnSystemDestroyed()
{
  // Restore present-all-frames behavior.
  if (m_display)
    m_display->SetDisplayMaxFPS(0.0f);

  if (FullscreenUI::IsInitialized())
    FullscreenUI::SystemDestroyed();

  InputManager::PauseVibration();
  FrontendCommon::ResumeScreensaver();
}

void CommonHostInterface::OnRunningGameChanged(const std::string& path, CDImage* image, const std::string& game_code,
                                               const std::string& game_title)
{
  if (g_settings.apply_game_settings)
    ApplySettings(true);

  if (!System::IsShutdown())
  {
    System::SetCheatList(nullptr);
    if (g_settings.auto_load_cheats)
    {
      DebugAssert(!IsCheevosChallengeModeActive());
      LoadCheatListFromGameTitle();
    }
  }

#ifdef WITH_DISCORD_PRESENCE
  UpdateDiscordPresence(false);
#endif

#ifdef WITH_CHEEVOS
  if (Cheevos::IsLoggedIn())
    Cheevos::GameChanged(path, image);
#endif
}

void CommonHostInterface::OnControllerTypeChanged(u32 slot)
{
  {
    auto lock = Host::GetSettingsLock();
    InputManager::ReloadBindings(*Host::GetSettingsInterface(), *Host::GetSettingsInterface());
  }
}

bool CommonHostInterface::IsCheevosChallengeModeActive() const
{
#ifdef WITH_CHEEVOS
  return Cheevos::IsChallengeModeActive();
#else
  return false;
#endif
}

void CommonHostInterface::DoFrameStep()
{
  if (System::IsShutdown())
    return;

  m_frame_step_request = true;
  PauseSystem(false);
}

void CommonHostInterface::DoToggleCheats()
{
  if (System::IsShutdown())
    return;

  CheatList* cl = System::GetCheatList();
  if (!cl)
  {
    AddKeyedOSDMessage("ToggleCheats", TranslateStdString("OSDMessage", "No cheats are loaded."), 10.0f);
    return;
  }

  cl->SetMasterEnable(!cl->GetMasterEnable());
  AddKeyedOSDMessage("ToggleCheats",
                     cl->GetMasterEnable() ?
                       TranslateStdString("OSDMessage", "%n cheats are now active.", "", cl->GetEnabledCodeCount()) :
                       TranslateStdString("OSDMessage", "%n cheats are now inactive.", "", cl->GetEnabledCodeCount()),
                     10.0f);
}

static void DisplayHotkeyBlockedByChallengeModeMessage()
{
  g_host_interface->AddOSDMessage(g_host_interface->TranslateStdString(
    "OSDMessage", "Hotkey unavailable because achievements hardcore mode is active."));
}

void CommonHostInterface::SetFastForwardEnabled(bool enabled)
{
  if (!System::IsValid())
    return;

  m_fast_forward_enabled = enabled;
  UpdateSpeedLimiterState();
}

void CommonHostInterface::SetTurboEnabled(bool enabled)
{
  if (!System::IsValid())
    return;

  m_turbo_enabled = enabled;
  UpdateSpeedLimiterState();
}

void CommonHostInterface::SetRewindState(bool enabled)
{
  if (!System::IsValid())
    return;

  if (!IsCheevosChallengeModeActive())
  {
    if (!g_settings.rewind_enable)
    {
      if (enabled)
        AddKeyedOSDMessage("SetRewindState", TranslateStdString("OSDMessage", "Rewinding is not enabled."), 5.0f);

      return;
    }

    if (!FullscreenUI::IsInitialized())
    {
      AddKeyedOSDMessage("SetRewindState",
                         enabled ? TranslateStdString("OSDMessage", "Rewinding...") :
                                   TranslateStdString("OSDMessage", "Stopped rewinding."),
                         5.0f);
    }

    System::SetRewinding(enabled);
    UpdateSpeedLimiterState();
  }
  else
  {
    DisplayHotkeyBlockedByChallengeModeMessage();
  }
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

std::string CommonHostInterface::GetGameSaveStateFileName(const char* game_code, s32 slot) const
{
  if (slot < 0)
    return GetUserDirectoryRelativePath("savestates" FS_OSPATH_SEPARATOR_STR "%s_resume.sav", game_code);
  else
    return GetUserDirectoryRelativePath("savestates" FS_OSPATH_SEPARATOR_STR "%s_%d.sav", game_code, slot);
}

std::string CommonHostInterface::GetGlobalSaveStateFileName(s32 slot) const
{
  if (slot < 0)
    return GetUserDirectoryRelativePath("savestates" FS_OSPATH_SEPARATOR_STR "resume.sav");
  else
    return GetUserDirectoryRelativePath("savestates" FS_OSPATH_SEPARATOR_STR "savestate_%d.sav", slot);
}

void CommonHostInterface::RenameCurrentSaveStateToBackup(const char* filename)
{
  if (!GetBoolSettingValue("General", "CreateSaveStateBackups", false))
    return;

  if (!FileSystem::FileExists(filename))
    return;

  const std::string backup_filename(Path::ReplaceExtension(filename, "bak"));
  if (!FileSystem::RenamePath(filename, backup_filename.c_str()))
  {
    Log_ErrorPrintf("Failed to rename save state backup '%s'", backup_filename.c_str());
    return;
  }

  Log_InfoPrintf("Renamed save state '%s' to '%s'", filename, backup_filename.c_str());
}

std::vector<CommonHostInterface::SaveStateInfo> CommonHostInterface::GetAvailableSaveStates(const char* game_code) const
{
  std::vector<SaveStateInfo> si;
  std::string path;

  auto add_path = [&si](std::string path, s32 slot, bool global) {
    FILESYSTEM_STAT_DATA sd;
    if (!FileSystem::StatFile(path.c_str(), &sd))
      return;

    si.push_back(SaveStateInfo{std::move(path), sd.ModificationTime, static_cast<s32>(slot), global});
  };

  if (game_code && std::strlen(game_code) > 0)
  {
    add_path(GetGameSaveStateFileName(game_code, -1), -1, false);
    for (s32 i = 1; i <= PER_GAME_SAVE_STATE_SLOTS; i++)
      add_path(GetGameSaveStateFileName(game_code, i), i, false);
  }

  for (s32 i = 1; i <= GLOBAL_SAVE_STATE_SLOTS; i++)
    add_path(GetGlobalSaveStateFileName(i), i, true);

  return si;
}

std::optional<CommonHostInterface::SaveStateInfo> CommonHostInterface::GetSaveStateInfo(const char* game_code, s32 slot)
{
  const bool global = (!game_code || game_code[0] == 0);
  std::string path = global ? GetGlobalSaveStateFileName(slot) : GetGameSaveStateFileName(game_code, slot);

  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
    return std::nullopt;

  return SaveStateInfo{std::move(path), sd.ModificationTime, slot, global};
}

std::optional<CommonHostInterface::ExtendedSaveStateInfo>
CommonHostInterface::GetExtendedSaveStateInfo(ByteStream* stream)
{
  SAVE_STATE_HEADER header;
  if (!stream->Read(&header, sizeof(header)) || header.magic != SAVE_STATE_MAGIC)
    return std::nullopt;

  ExtendedSaveStateInfo ssi;
  if (header.version < SAVE_STATE_MINIMUM_VERSION || header.version > SAVE_STATE_VERSION)
  {
    ssi.title = StringUtil::StdStringFromFormat(
      TranslateString("CommonHostInterface", "Invalid version %u (%s version %u)"), header.version,
      header.version > SAVE_STATE_VERSION ? "maximum" : "minimum",
      header.version > SAVE_STATE_VERSION ? SAVE_STATE_VERSION : SAVE_STATE_MINIMUM_VERSION);
    return ssi;
  }

  header.title[sizeof(header.title) - 1] = 0;
  ssi.title = header.title;
  header.game_code[sizeof(header.game_code) - 1] = 0;
  ssi.game_code = header.game_code;

  if (header.media_filename_length > 0 &&
      (header.offset_to_media_filename + header.media_filename_length) <= stream->GetSize())
  {
    stream->SeekAbsolute(header.offset_to_media_filename);
    ssi.media_path.resize(header.media_filename_length);
    if (!stream->Read2(ssi.media_path.data(), header.media_filename_length))
      std::string().swap(ssi.media_path);
  }

  if (header.screenshot_width > 0 && header.screenshot_height > 0 && header.screenshot_size > 0 &&
      (static_cast<u64>(header.offset_to_screenshot) + static_cast<u64>(header.screenshot_size)) <= stream->GetSize())
  {
    stream->SeekAbsolute(header.offset_to_screenshot);
    ssi.screenshot_data.resize((header.screenshot_size + 3u) / 4u);
    if (stream->Read2(ssi.screenshot_data.data(), header.screenshot_size))
    {
      ssi.screenshot_width = header.screenshot_width;
      ssi.screenshot_height = header.screenshot_height;
    }
    else
    {
      decltype(ssi.screenshot_data)().swap(ssi.screenshot_data);
    }
  }

  return ssi;
}

std::optional<CommonHostInterface::ExtendedSaveStateInfo>
CommonHostInterface::GetExtendedSaveStateInfo(const char* game_code, s32 slot)
{
  const bool global = (!game_code || game_code[0] == 0);
  std::string path = global ? GetGlobalSaveStateFileName(slot) : GetGameSaveStateFileName(game_code, slot);

  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
    return std::nullopt;

  std::unique_ptr<ByteStream> stream =
    ByteStream::OpenFile(path.c_str(), BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_SEEKABLE);
  if (!stream)
    return std::nullopt;

  std::optional<ExtendedSaveStateInfo> ssi = GetExtendedSaveStateInfo(stream.get());
  if (!ssi)
    return std::nullopt;

  ssi->path = std::move(path);
  ssi->timestamp = sd.ModificationTime;
  ssi->slot = slot;
  ssi->global = global;

  return ssi;
}

std::optional<CommonHostInterface::ExtendedSaveStateInfo> CommonHostInterface::GetUndoSaveStateInfo()
{
  std::optional<ExtendedSaveStateInfo> ssi;
  if (m_undo_load_state)
  {
    m_undo_load_state->SeekAbsolute(0);
    ssi = GetExtendedSaveStateInfo(m_undo_load_state.get());
    m_undo_load_state->SeekAbsolute(0);

    if (ssi)
    {
      ssi->timestamp = 0;
      ssi->slot = 0;
      ssi->global = false;
    }
  }

  return ssi;
}

void CommonHostInterface::DeleteSaveStates(const char* game_code, bool resume)
{
  const std::vector<SaveStateInfo> states(GetAvailableSaveStates(game_code));
  for (const SaveStateInfo& si : states)
  {
    if (si.global || (!resume && si.slot < 0))
      continue;

    Log_InfoPrintf("Removing save state at '%s'", si.path.c_str());
    if (!FileSystem::DeleteFile(si.path.c_str()))
      Log_ErrorPrintf("Failed to delete save state file '%s'", si.path.c_str());
  }
}

std::string CommonHostInterface::GetMostRecentResumeSaveStatePath() const
{
  std::vector<FILESYSTEM_FIND_DATA> files;
  if (!FileSystem::FindFiles(GetUserDirectoryRelativePath("savestates").c_str(), "*resume.sav", FILESYSTEM_FIND_FILES,
                             &files) ||
      files.empty())
  {
    return {};
  }

  FILESYSTEM_FIND_DATA* most_recent = &files[0];
  for (FILESYSTEM_FIND_DATA& file : files)
  {
    if (file.ModificationTime > most_recent->ModificationTime)
      most_recent = &file;
  }

  return std::move(most_recent->FileName);
}

void CommonHostInterface::SetDefaultSettings(SettingsInterface& si)
{
  HostInterface::SetDefaultSettings(si);

  InputManager::SetDefaultConfig(si);

  si.SetBoolValue("Display", "InternalResolutionScreenshots", false);

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

void CommonHostInterface::LoadSettings(SettingsInterface& si)
{
  HostInterface::LoadSettings(si);

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

void CommonHostInterface::SaveSettings(SettingsInterface& si)
{
  HostInterface::SaveSettings(si);
}

void CommonHostInterface::FixIncompatibleSettings(bool display_osd_messages)
{
  // if challenge mode is enabled, disable things like rewind since they use save states
  if (IsCheevosChallengeModeActive())
  {
    g_settings.emulation_speed =
      (g_settings.emulation_speed != 0.0f) ? std::max(g_settings.emulation_speed, 1.0f) : 0.0f;
    g_settings.fast_forward_speed =
      (g_settings.fast_forward_speed != 0.0f) ? std::max(g_settings.fast_forward_speed, 1.0f) : 0.0f;
    g_settings.turbo_speed = (g_settings.turbo_speed != 0.0f) ? std::max(g_settings.turbo_speed, 1.0f) : 0.0f;
    g_settings.rewind_enable = false;
    g_settings.auto_load_cheats = false;
    if (g_settings.cpu_overclock_enable && g_settings.GetCPUOverclockPercent() < 100)
    {
      g_settings.cpu_overclock_enable = false;
      g_settings.UpdateOverclockActive();
    }
    g_settings.debugging.enable_gdb_server = false;
    g_settings.debugging.show_vram = false;
    g_settings.debugging.show_gpu_state = false;
    g_settings.debugging.show_cdrom_state = false;
    g_settings.debugging.show_spu_state = false;
    g_settings.debugging.show_timers_state = false;
    g_settings.debugging.show_mdec_state = false;
    g_settings.debugging.show_dma_state = false;
    g_settings.debugging.dump_cpu_to_vram_copies = false;
    g_settings.debugging.dump_vram_to_cpu_copies = false;
  }

  HostInterface::FixIncompatibleSettings(display_osd_messages);
}

void CommonHostInterface::ApplySettings(bool display_osd_messages)
{
  Settings old_settings(std::move(g_settings));
  {
    auto lock = Host::GetSettingsLock();
    LoadSettings(*Host::GetSettingsInterface());
    ApplyGameSettings(display_osd_messages);
    FixIncompatibleSettings(display_osd_messages);
    InputManager::ReloadSources(*Host::GetSettingsInterface(), lock);
    InputManager::ReloadBindings(*Host::GetSettingsInterface(), *Host::GetSettingsInterface());
  }

  CheckForSettingsChanges(old_settings);
}

void CommonHostInterface::SetDefaultSettings()
{
  Settings old_settings(std::move(g_settings));
  {
    auto lock = Host::GetSettingsLock();
    SetDefaultSettings(*Host::GetSettingsInterface());
    LoadSettings(*Host::GetSettingsInterface());
    InputManager::ReloadSources(*Host::GetSettingsInterface(), lock);
    InputManager::ReloadBindings(*Host::GetSettingsInterface(), *Host::GetSettingsInterface());
    ApplyGameSettings(true);
    FixIncompatibleSettings(true);
  }

  CheckForSettingsChanges(old_settings);
}

void CommonHostInterface::CheckForSettingsChanges(const Settings& old_settings)
{
  HostInterface::CheckForSettingsChanges(old_settings);

  if (System::IsValid())
  {
    if (g_settings.audio_backend != old_settings.audio_backend ||
        g_settings.audio_buffer_size != old_settings.audio_buffer_size ||
        g_settings.video_sync_enabled != old_settings.video_sync_enabled ||
        g_settings.audio_sync_enabled != old_settings.audio_sync_enabled ||
        g_settings.increase_timer_resolution != old_settings.increase_timer_resolution ||
        g_settings.emulation_speed != old_settings.emulation_speed ||
        g_settings.fast_forward_speed != old_settings.fast_forward_speed ||
        g_settings.display_max_fps != old_settings.display_max_fps ||
        g_settings.display_all_frames != old_settings.display_all_frames ||
        g_settings.audio_resampling != old_settings.audio_resampling ||
        g_settings.sync_to_host_refresh_rate != old_settings.sync_to_host_refresh_rate)
    {
      UpdateSpeedLimiterState();
    }

    if (g_settings.display_post_processing != old_settings.display_post_processing ||
        g_settings.display_post_process_chain != old_settings.display_post_process_chain)
    {
      if (g_settings.display_post_processing)
      {
        if (!m_display->SetPostProcessingChain(g_settings.display_post_process_chain))
          AddOSDMessage(TranslateStdString("OSDMessage", "Failed to load post processing shader chain."), 20.0f);
      }
      else
      {
        m_display->SetPostProcessingChain({});
      }
    }

    if (g_settings.inhibit_screensaver != old_settings.inhibit_screensaver)
    {
      if (g_settings.inhibit_screensaver)
        FrontendCommon::SuspendScreensaver(m_display->GetWindowInfo());
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

std::string CommonHostInterface::GetStringSettingValue(const char* section, const char* key,
                                                       const char* default_value /*= ""*/)
{
  return Host::GetStringSettingValue(section, key, default_value);
}

bool CommonHostInterface::GetBoolSettingValue(const char* section, const char* key, bool default_value /* = false */)
{
  return Host::GetBoolSettingValue(section, key, default_value);
}

int CommonHostInterface::GetIntSettingValue(const char* section, const char* key, int default_value /* = 0 */)
{
  return Host::GetIntSettingValue(section, key, default_value);
}

float CommonHostInterface::GetFloatSettingValue(const char* section, const char* key, float default_value /* = 0.0f */)
{
  return Host::GetFloatSettingValue(section, key, default_value);
}

std::vector<std::string> CommonHostInterface::GetSettingStringList(const char* section, const char* key)
{
  return Host::GetStringListSetting(section, key);
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
  m_display->Render();
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

bool CommonHostInterface::SaveResumeSaveState()
{
  if (System::IsShutdown())
    return false;

  const bool global = System::GetRunningCode().empty();
  return SaveState(global, -1);
}

bool CommonHostInterface::IsDumpingAudio() const
{
  return g_spu.IsDumpingAudio();
}

bool CommonHostInterface::StartDumpingAudio(const char* filename)
{
  if (System::IsShutdown())
    return false;

  std::string auto_filename;
  if (!filename)
  {
    const auto& code = System::GetRunningCode();
    if (code.empty())
    {
      auto_filename = GetUserDirectoryRelativePath("dump/audio/%s.wav", GetTimestampStringForFileName().GetCharArray());
    }
    else
    {
      auto_filename = GetUserDirectoryRelativePath("dump/audio/%s_%s.wav", code.c_str(),
                                                   GetTimestampStringForFileName().GetCharArray());
    }

    filename = auto_filename.c_str();
  }

  if (g_spu.StartDumpingAudio(filename))
  {
    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Started dumping audio to '%s'."), filename);
    return true;
  }
  else
  {
    AddFormattedOSDMessage(10.0f, TranslateString("OSDMessage", "Failed to start dumping audio to '%s'."), filename);
    return false;
  }
}

void CommonHostInterface::StopDumpingAudio()
{
  if (System::IsShutdown() || !g_spu.StopDumpingAudio())
    return;

  AddOSDMessage(TranslateStdString("OSDMessage", "Stopped dumping audio."), 5.0f);
}

bool CommonHostInterface::SaveScreenshot(const char* filename /* = nullptr */, bool full_resolution /* = true */,
                                         bool apply_aspect_ratio /* = true */, bool compress_on_thread /* = true */)
{
  if (System::IsShutdown())
    return false;

  std::string auto_filename;
  if (!filename)
  {
    const auto& code = System::GetRunningCode();
    const char* extension = "png";
    if (code.empty())
    {
      auto_filename = GetUserDirectoryRelativePath("screenshots" FS_OSPATH_SEPARATOR_STR "%s.%s",
                                                   GetTimestampStringForFileName().GetCharArray(), extension);
    }
    else
    {
      auto_filename = GetUserDirectoryRelativePath("screenshots" FS_OSPATH_SEPARATOR_STR "%s_%s.%s", code.c_str(),
                                                   GetTimestampStringForFileName().GetCharArray(), extension);
    }

    filename = auto_filename.c_str();
  }

  if (FileSystem::FileExists(filename))
  {
    AddFormattedOSDMessage(10.0f, TranslateString("OSDMessage", "Screenshot file '%s' already exists."), filename);
    return false;
  }

  const bool internal_resolution = GetBoolSettingValue("Display", "InternalResolutionScreenshots", false);
  const bool screenshot_saved =
    internal_resolution ?
      m_display->WriteDisplayTextureToFile(filename, full_resolution, apply_aspect_ratio, compress_on_thread) :
      m_display->WriteScreenshotToFile(filename, compress_on_thread);

  if (!screenshot_saved)
  {
    AddFormattedOSDMessage(10.0f, TranslateString("OSDMessage", "Failed to save screenshot to '%s'"), filename);
    return false;
  }

  AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Screenshot saved to '%s'."), filename);
  return true;
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
          TranslateString("ControllerType", Settings::GetControllerTypeDisplayName(supported_ctype)));
      }

      AddFormattedOSDMessage(
        30.0f,
        TranslateString("OSDMessage", "Controller in port %u (%s) is not supported for %s.\nSupported controllers: "
                                      "%s\nPlease configure a supported controller from the list above."),
        i + 1u, TranslateString("ControllerType", Settings::GetControllerTypeDisplayName(ctype)).GetCharArray(),
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

std::string CommonHostInterface::GetCheatFileName() const
{
  const std::string& title = System::GetRunningTitle();
  if (title.empty())
    return {};

  return GetUserDirectoryRelativePath("cheats/%s.cht", title.c_str());
}

bool CommonHostInterface::LoadCheatList(const char* filename)
{
  if (System::IsShutdown())
    return false;

  std::unique_ptr<CheatList> cl = std::make_unique<CheatList>();
  if (!cl->LoadFromFile(filename, CheatList::Format::Autodetect))
  {
    AddFormattedOSDMessage(15.0f, TranslateString("OSDMessage", "Failed to load cheats from '%s'."), filename);
    return false;
  }

  if (cl->GetEnabledCodeCount() > 0)
  {
    AddOSDMessage(TranslateStdString("OSDMessage", "%n cheats are enabled. This may result in instability.", "",
                                     cl->GetEnabledCodeCount()),
                  30.0f);
  }

  System::SetCheatList(std::move(cl));
  return true;
}

bool CommonHostInterface::LoadCheatListFromGameTitle()
{
  if (IsCheevosChallengeModeActive())
    return false;

  const std::string filename(GetCheatFileName());
  if (filename.empty() || !FileSystem::FileExists(filename.c_str()))
    return false;

  return LoadCheatList(filename.c_str());
}

bool CommonHostInterface::LoadCheatListFromDatabase()
{
  if (System::GetRunningCode().empty() || IsCheevosChallengeModeActive())
    return false;

  std::unique_ptr<CheatList> cl = std::make_unique<CheatList>();
  if (!cl->LoadFromPackage(System::GetRunningCode()))
    return false;

  Log_InfoPrintf("Loaded %u cheats from database.", cl->GetCodeCount());
  System::SetCheatList(std::move(cl));
  return true;
}

bool CommonHostInterface::SaveCheatList()
{
  if (!System::IsValid() || !System::HasCheatList())
    return false;

  const std::string filename(GetCheatFileName());
  if (filename.empty())
    return false;

  if (!System::GetCheatList()->SaveToPCSXRFile(filename.c_str()))
  {
    AddFormattedOSDMessage(15.0f, TranslateString("OSDMessage", "Failed to save cheat list to '%s'"), filename.c_str());
  }

  return true;
}

bool CommonHostInterface::SaveCheatList(const char* filename)
{
  if (!System::IsValid() || !System::HasCheatList())
    return false;

  if (!System::GetCheatList()->SaveToPCSXRFile(filename))
    return false;

  // This shouldn't be needed, but lupdate doesn't gather this string otherwise...
  const u32 code_count = System::GetCheatList()->GetCodeCount();
  AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Saved %n cheats to '%s'.", "", code_count), filename);
  return true;
}

bool CommonHostInterface::DeleteCheatList()
{
  if (!System::IsValid())
    return false;

  const std::string filename(GetCheatFileName());
  if (!filename.empty())
  {
    if (!FileSystem::DeleteFile(filename.c_str()))
      return false;

    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Deleted cheat list '%s'."), filename.c_str());
  }

  System::SetCheatList(nullptr);
  return true;
}

void CommonHostInterface::ClearCheatList(bool save_to_file)
{
  if (!System::IsValid())
    return;

  CheatList* cl = System::GetCheatList();
  if (!cl)
    return;

  while (cl->GetCodeCount() > 0)
    cl->RemoveCode(cl->GetCodeCount() - 1);

  if (save_to_file)
    SaveCheatList();
}

void CommonHostInterface::SetCheatCodeState(u32 index, bool enabled, bool save_to_file)
{
  if (!System::IsValid() || !System::HasCheatList())
    return;

  CheatList* cl = System::GetCheatList();
  if (index >= cl->GetCodeCount())
    return;

  CheatCode& cc = cl->GetCode(index);
  if (cc.enabled == enabled)
    return;

  cc.enabled = enabled;
  if (!enabled)
    cc.ApplyOnDisable();

  if (enabled)
  {
    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Cheat '%s' enabled."), cc.description.c_str());
  }
  else
  {
    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Cheat '%s' disabled."), cc.description.c_str());
  }

  if (save_to_file)
    SaveCheatList();
}

void CommonHostInterface::ApplyCheatCode(u32 index)
{
  if (!System::HasCheatList() || index >= System::GetCheatList()->GetCodeCount())
    return;

  const CheatCode& cc = System::GetCheatList()->GetCode(index);
  if (!cc.enabled)
  {
    cc.Apply();
    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Applied cheat '%s'."), cc.description.c_str());
  }
  else
  {
    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Cheat '%s' is already enabled."),
                           cc.description.c_str());
  }
}

void CommonHostInterface::TogglePostProcessing()
{
  if (!m_display)
    return;

  g_settings.display_post_processing = !g_settings.display_post_processing;
  if (g_settings.display_post_processing)
  {
    AddKeyedOSDMessage("PostProcessing", TranslateStdString("OSDMessage", "Post-processing is now enabled."), 10.0f);

    if (!m_display->SetPostProcessingChain(g_settings.display_post_process_chain))
      AddOSDMessage(TranslateStdString("OSDMessage", "Failed to load post processing shader chain."), 20.0f);
  }
  else
  {
    AddKeyedOSDMessage("PostProcessing", TranslateStdString("OSDMessage", "Post-processing is now disabled."), 10.0f);
    m_display->SetPostProcessingChain({});
  }
}

void CommonHostInterface::ReloadPostProcessingShaders()
{
  if (!m_display || !g_settings.display_post_processing)
    return;

  if (!m_display->SetPostProcessingChain(g_settings.display_post_process_chain))
    AddOSDMessage(TranslateStdString("OSDMessage", "Failed to load post-processing shader chain."), 20.0f);
  else
    AddOSDMessage(TranslateStdString("OSDMessage", "Post-processing shaders reloaded."), 10.0f);
}

void CommonHostInterface::ToggleWidescreen()
{
  Panic("Fixme");
#if 0
  g_settings.gpu_widescreen_hack = !g_settings.gpu_widescreen_hack;

  const GameSettings::Entry* gs = m_game_list->GetGameSettings(System::GetRunningPath(), System::GetRunningCode());
  DisplayAspectRatio user_ratio;
  if (gs && gs->display_aspect_ratio.has_value())
  {
    user_ratio = gs->display_aspect_ratio.value();
  }
  else
  {
    std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
    user_ratio = Settings::ParseDisplayAspectRatio(
                   m_settings_interface
                     ->GetStringValue("Display", "AspectRatio",
                                      Settings::GetDisplayAspectRatioName(Settings::DEFAULT_DISPLAY_ASPECT_RATIO))
                     .c_str())
                   .value_or(DisplayAspectRatio::Auto);
  }

  if (user_ratio == DisplayAspectRatio::Auto || user_ratio == DisplayAspectRatio::PAR1_1 ||
      user_ratio == DisplayAspectRatio::R4_3)
  {
    g_settings.display_aspect_ratio = g_settings.gpu_widescreen_hack ? DisplayAspectRatio::R16_9 : user_ratio;
  }
  else
  {
    g_settings.display_aspect_ratio = g_settings.gpu_widescreen_hack ? user_ratio : DisplayAspectRatio::Auto;
  }

  if (g_settings.gpu_widescreen_hack)
  {
    AddKeyedFormattedOSDMessage(
      "WidescreenHack", 5.0f,
      TranslateString("OSDMessage", "Widescreen hack is now enabled, and aspect ratio is set to %s."),
      TranslateString("DisplayAspectRatio", Settings::GetDisplayAspectRatioName(g_settings.display_aspect_ratio))
        .GetCharArray());
  }
  else
  {
    AddKeyedFormattedOSDMessage(
      "WidescreenHack", 5.0f,
      TranslateString("OSDMessage", "Widescreen hack is now disabled, and aspect ratio is set to %s."),
      TranslateString("DisplayAspectRatio", Settings::GetDisplayAspectRatioName(g_settings.display_aspect_ratio))
        .GetCharArray());
  }

  GTE::UpdateAspectRatio();
#endif
}

void CommonHostInterface::SwapMemoryCards()
{
  System::SwapMemoryCards();

  if (System::HasMemoryCard(0) && System::HasMemoryCard(1))
  {
    g_host_interface->AddOSDMessage(
      g_host_interface->TranslateStdString("OSDMessage", "Swapped memory card ports. Both ports have a memory card."),
      10.0f);
  }
  else if (System::HasMemoryCard(1))
  {
    g_host_interface->AddOSDMessage(
      g_host_interface->TranslateStdString("OSDMessage",
                                           "Swapped memory card ports. Port 2 has a memory card, Port 1 is empty."),
      10.0f);
  }
  else if (System::HasMemoryCard(0))
  {
    g_host_interface->AddOSDMessage(
      g_host_interface->TranslateStdString("OSDMessage",
                                           "Swapped memory card ports. Port 1 has a memory card, Port 2 is empty."),
      10.0f);
  }
  else
  {
    g_host_interface->AddOSDMessage(
      g_host_interface->TranslateStdString("OSDMessage", "Swapped memory card ports. Neither port has a memory card."),
      10.0f);
  }
}

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

  const float y_scale =
    (static_cast<float>(m_display->GetDisplayWidth()) / static_cast<float>(m_display->GetDisplayHeight())) /
    m_display->GetDisplayAspectRatio();

  const u32 requested_width =
    std::max<u32>(static_cast<u32>(std::ceil(static_cast<float>(m_display->GetDisplayWidth()) * scale)), 1);
  const u32 requested_height =
    std::max<u32>(static_cast<u32>(std::ceil(static_cast<float>(m_display->GetDisplayHeight()) * y_scale * scale)), 1);

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

void CommonHostInterface::SetDiscordPresenceEnabled(bool enabled)
{
  if (m_discord_presence_enabled == enabled)
    return;

  m_discord_presence_enabled = enabled;
  if (enabled)
    InitializeDiscordPresence();
  else
    ShutdownDiscordPresence();
}

void CommonHostInterface::InitializeDiscordPresence()
{
  if (m_discord_presence_active)
    return;

  DiscordEventHandlers handlers = {};
  Discord_Initialize("705325712680288296", &handlers, 0, nullptr);
  m_discord_presence_active = true;

  UpdateDiscordPresence(false);
}

void CommonHostInterface::ShutdownDiscordPresence()
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

void CommonHostInterface::UpdateDiscordPresence(bool rich_presence_only)
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

void CommonHostInterface::PollDiscordPresence()
{
  if (!m_discord_presence_active)
    return;

  UpdateDiscordPresence(true);

  Discord_RunCallbacks();
}

#endif

#ifdef WITH_CHEEVOS

void CommonHostInterface::UpdateCheevosActive(SettingsInterface& si)
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
        ReportError("Failed to initialize cheevos after settings change.");
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