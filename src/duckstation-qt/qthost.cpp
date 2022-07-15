#include "qthost.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/crash_handler.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "core/cheats.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host.h"
#include "core/host_settings.h"
#include "core/memory_card.h"
#include "core/spu.h"
#include "core/system.h"
#include "displaywidget.h"
#include "frontend-common/fullscreen_ui.h"
#include "frontend-common/game_list.h"
#include "frontend-common/imgui_manager.h"
#include "frontend-common/input_manager.h"
#include "frontend-common/opengl_host_display.h"
#include "frontend-common/sdl_audio_stream.h"
#include "frontend-common/vulkan_host_display.h"
#include "imgui.h"
#include "mainwindow.h"
#include "qtprogresscallback.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
#include "util/audio_stream.h"
#include "util/ini_settings_interface.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QTranslator>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
Log_SetChannel(EmuThread);

#ifdef _WIN32
#include "common/windows_headers.h"
#include "frontend-common/d3d11_host_display.h"
#include "frontend-common/d3d12_host_display.h"
#include <KnownFolders.h>
#include <ShlObj.h>
#endif

#ifdef WITH_CHEEVOS
#include "frontend-common/cheevos.h"
#endif

static constexpr u32 SETTINGS_VERSION = 3;
static constexpr u32 SETTINGS_SAVE_DELAY = 1000;

/// Interval at which the controllers are polled when the system is not active.
static constexpr u32 BACKGROUND_CONTROLLER_POLLING_INTERVAL = 100;

//////////////////////////////////////////////////////////////////////////
// Local function declarations
//////////////////////////////////////////////////////////////////////////
namespace QtHost {
static void RegisterTypes();
static bool InitializeConfig();
static bool ShouldUsePortableMode();
static void SetAppRoot();
static void SetResourcesDirectory();
static void SetDataDirectory();
static bool SetCriticalFolders();
static void SetDefaultConfig(SettingsInterface& si);
static void SaveSettings();
static void InstallTranslator();
static std::string GetFontPath(const char* name);
static void HookSignals();
} // namespace QtHost

static std::unique_ptr<INISettingsInterface> s_base_settings_interface;
static std::unique_ptr<QTimer> s_settings_save_timer;
static std::unique_ptr<HostDisplay> s_host_display;
static std::vector<QTranslator*> m_translators;
static bool s_batch_mode = false;
static bool s_nogui_mode = false;
static bool s_start_fullscreen_ui = false;

EmuThread* g_emu_thread;

EmuThread::EmuThread(QThread* ui_thread) : QThread(), m_ui_thread(ui_thread) {}

EmuThread::~EmuThread() = default;

void QtHost::RegisterTypes()
{
  // Register any standard types we need elsewhere
  qRegisterMetaType<std::optional<bool>>();
  qRegisterMetaType<std::function<void()>>();
  qRegisterMetaType<std::shared_ptr<SystemBootParameters>>();
  qRegisterMetaType<const GameList::Entry*>();
  qRegisterMetaType<GPURenderer>();
}

std::vector<std::pair<QString, QString>> QtHost::GetAvailableLanguageList()
{
  return {{QStringLiteral("English"), QStringLiteral("en")},
          {QStringLiteral("Deutsch"), QStringLiteral("de")},
          {QStringLiteral("Español de Hispanoamérica"), QStringLiteral("es")},
          {QStringLiteral("Español de España"), QStringLiteral("es-es")},
          {QStringLiteral("Français"), QStringLiteral("fr")},
          {QStringLiteral("עברית"), QStringLiteral("he")},
          {QStringLiteral("日本語"), QStringLiteral("ja")},
          {QStringLiteral("Italiano"), QStringLiteral("it")},
          {QStringLiteral("Nederlands"), QStringLiteral("nl")},
          {QStringLiteral("Polski"), QStringLiteral("pl")},
          {QStringLiteral("Português (Pt)"), QStringLiteral("pt-pt")},
          {QStringLiteral("Português (Br)"), QStringLiteral("pt-br")},
          {QStringLiteral("Русский"), QStringLiteral("ru")},
          {QStringLiteral("Türkçe"), QStringLiteral("tr")},
          {QStringLiteral("简体中文"), QStringLiteral("zh-cn")}};
}

bool QtHost::Initialize()
{
  if (!InitializeConfig())
    return false;

  InstallTranslator();
  EmuThread::start();
  return true;
}

void QtHost::Shutdown()
{
  EmuThread::stop();

  if (s_settings_save_timer)
  {
    s_settings_save_timer.reset();
    QtHost::SaveSettings();
  }
}

bool QtHost::InBatchMode()
{
  return s_batch_mode;
}

bool QtHost::InNoGUIMode()
{
  return s_nogui_mode;
}

QString QtHost::GetAppNameAndVersion()
{
  return QStringLiteral("DuckStation %1 (%2)").arg(g_scm_tag_str).arg(g_scm_branch_str);
}

QString QtHost::GetAppConfigSuffix()
{
#if defined(_DEBUGFAST)
  return QStringLiteral(" [DebugFast]");
#elif defined(_DEBUG)
  return QStringLiteral(" [Debug]");
#else
  return QString();
#endif
}

bool QtHost::InitializeConfig()
{
  if (!SetCriticalFolders())
    return false;

  const std::string path(Path::Combine(EmuFolders::DataRoot, "settings.ini"));
  Log_InfoPrintf("Loading config from %s.", path.c_str());
  s_base_settings_interface = std::make_unique<INISettingsInterface>(std::move(path));
  Host::Internal::SetBaseSettingsLayer(s_base_settings_interface.get());

  uint settings_version;
  if (!s_base_settings_interface->Load() ||
      !s_base_settings_interface->GetUIntValue("Main", "SettingsVersion", &settings_version) ||
      settings_version != SETTINGS_VERSION)
  {
    if (s_base_settings_interface->ContainsValue("Main", "SettingsVersion"))
    {
      // NOTE: No point translating this, because there's no config loaded, so no language loaded.
      Host::ReportErrorAsync("Error", fmt::format("Settings version {} does not match expected version {}, resetting.",
                                                  settings_version, SETTINGS_VERSION));
    }

    SetDefaultConfig(*s_base_settings_interface);
    s_base_settings_interface->Save();
  }

  EmuFolders::LoadConfig(*s_base_settings_interface.get());
  EmuFolders::EnsureFoldersExist();
  return true;
}

bool QtHost::SetCriticalFolders()
{
  SetAppRoot();
  SetResourcesDirectory();
  SetDataDirectory();

  // logging of directories in case something goes wrong super early
  Log_DevPrintf("AppRoot Directory: %s", EmuFolders::AppRoot.c_str());
  Log_DevPrintf("DataRoot Directory: %s", EmuFolders::DataRoot.c_str());
  Log_DevPrintf("Resources Directory: %s", EmuFolders::Resources.c_str());

  // Write crash dumps to the data directory, since that'll be accessible for certain.
  CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

  if (!FileSystem::SetWorkingDirectory(EmuFolders::DataRoot.c_str()))
    Log_ErrorPrintf("Failed to set working directory to '%s'", EmuFolders::DataRoot.c_str());

  // the resources directory should exist, bail out if not
  if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
  {
    QMessageBox::critical(nullptr, QStringLiteral("Error"),
                          QStringLiteral("Resources directory is missing, your installation is incomplete."));
    return false;
  }

  return true;
}

bool QtHost::ShouldUsePortableMode()
{
  // Check whether portable.ini exists in the program directory.
  return (FileSystem::FileExists(Path::Combine(EmuFolders::AppRoot, "portable.txt").c_str()) ||
          FileSystem::FileExists(Path::Combine(EmuFolders::AppRoot, "settings.ini").c_str()));
}

void QtHost::SetAppRoot()
{
  std::string program_path(FileSystem::GetProgramPath());
  Log_InfoPrintf("Program Path: %s", program_path.c_str());

  EmuFolders::AppRoot = Path::Canonicalize(Path::GetDirectory(program_path));
}

void QtHost::SetResourcesDirectory()
{
#ifndef __APPLE__
  // On Windows/Linux, these are in the binary directory.
  EmuFolders::Resources = Path::Combine(EmuFolders::AppRoot, "resources");
#else
  // On macOS, this is in the bundle resources directory.
  EmuFolders::Resources = Path::Canonicalize(Path::Combine(EmuFolders::AppRoot, "../Resources"));
#endif
}

void QtHost::SetDataDirectory()
{
  if (ShouldUsePortableMode())
  {
    EmuFolders::DataRoot = EmuFolders::AppRoot;
    return;
  }

#if defined(_WIN32)
  // On Windows, use My Documents\PCSX2 to match old installs.
  PWSTR documents_directory;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &documents_directory)))
  {
    if (std::wcslen(documents_directory) > 0)
      EmuFolders::DataRoot = Path::Combine(StringUtil::WideStringToUTF8String(documents_directory), "DuckStation");
    CoTaskMemFree(documents_directory);
  }
#elif defined(__linux__)
  // Use $XDG_CONFIG_HOME/PCSX2 if it exists.
  const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
  if (xdg_config_home && Path::IsAbsolute(xdg_config_home))
  {
    EmuFolders::DataRoot = Path::Combine(xdg_config_home, "PCSX2");
  }
  else
  {
    // Use ~/PCSX2 for non-XDG, and ~/.config/PCSX2 for XDG.
    // Maybe we should drop the former when Qt goes live.
    const char* home_dir = getenv("HOME");
    if (home_dir)
    {
      // ~/.local/share should exist, but just in case it doesn't and this is a fresh profile..
      const std::string local_dir(Path::Combine(home_dir, ".local"));
      const std::string share_dir(Path::Combine(local_dir, "share"));
      FileSystem::EnsureDirectoryExists(local_dir.c_str(), false);
      FileSystem::EnsureDirectoryExists(share_dir.c_str(), false);
      EmuFolders::DataRoot = Path::Combine(share_dir, "duckstation");
    }
  }
#elif defined(__APPLE__)
  static constexpr char MAC_DATA_DIR[] = "Library/Application Support/DuckStation";
  const char* home_dir = getenv("HOME");
  if (home_dir)
    EmuFolders::DataRoot = Path::Combine(home_dir, MAC_DATA_DIR);
#endif

  // make sure it exists
  if (!EmuFolders::DataRoot.empty() && !FileSystem::DirectoryExists(EmuFolders::DataRoot.c_str()))
  {
    // we're in trouble if we fail to create this directory... but try to hobble on with portable
    if (!FileSystem::EnsureDirectoryExists(EmuFolders::DataRoot.c_str(), false))
      EmuFolders::DataRoot.clear();
  }

  // couldn't determine the data directory? fallback to portable.
  if (EmuFolders::DataRoot.empty())
    EmuFolders::DataRoot = EmuFolders::AppRoot;
}

#if 0
void QtHost::UpdateFolders()
{
  // TODO: This should happen with the VM thread paused.
  auto lock = Host::GetSettingsLock();
  EmuFolders::LoadConfig(*s_base_settings_interface.get());
  EmuFolders::EnsureFoldersExist();
}
#endif

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
  CommonHost::LoadSettings(si, lock);
  g_emu_thread->loadSettings(si);
}

void EmuThread::loadSettings(SettingsInterface& si)
{
  //
}

void EmuThread::setInitialState()
{
  m_is_fullscreen = Host::GetBaseBoolSettingValue("Main", "StartFullscreen", false);
  m_is_rendering_to_main = shouldRenderToMain();
  m_is_surfaceless = false;
}

void Host::CheckForSettingsChanges(const Settings& old_settings)
{
  CommonHost::CheckForSettingsChanges(old_settings);
  g_emu_thread->checkForSettingsChanges(old_settings);
}

void EmuThread::checkForSettingsChanges(const Settings& old_settings)
{
  const bool render_to_main = shouldRenderToMain();
  if (m_is_rendering_to_main != render_to_main)
  {
    m_is_rendering_to_main = render_to_main;
    updateDisplayState();
  }

  QMetaObject::invokeMethod(g_main_window, &MainWindow::checkForSettingChanges, Qt::QueuedConnection);
}

void EmuThread::setDefaultSettings()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "setDefaultSettings", Qt::QueuedConnection);
    return;
  }

  auto lock = Host::GetSettingsLock();
  QtHost::SetDefaultConfig(*s_base_settings_interface);
  QtHost::QueueSettingsSave();
  emit settingsResetToDefault();
}

void QtHost::SetDefaultConfig(SettingsInterface& si)
{
  si.SetUIntValue("Main", "SettingsVersion", SETTINGS_VERSION);

  System::SetDefaultSettings(si);
  CommonHost::SetDefaultSettings(si);
  EmuFolders::Save(si);
}

bool EmuThread::shouldRenderToMain() const
{
  return !Host::GetBaseBoolSettingValue("Main", "RenderToSeparateWindow", false) && !QtHost::InNoGUIMode();
}

void Host::RequestResizeHostDisplay(s32 new_window_width, s32 new_window_height)
{
  if (g_emu_thread->isFullscreen())
    return;

  emit g_emu_thread->displaySizeRequested(new_window_width, new_window_height);
}

static std::string QtHost::GetFontPath(const char* name)
{
#ifdef _WIN32
  PWSTR folder_path;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &folder_path)))
    return StringUtil::StdStringFromFormat("C:\\Windows\\Fonts\\%s", name);

  std::string font_path(StringUtil::WideStringToUTF8String(folder_path));
  CoTaskMemFree(folder_path);
  font_path += "\\";
  font_path += name;
  return font_path;
#else
  return name;
#endif
}

void QtHost::InstallTranslator()
{
  const QString language(QString::fromStdString(Host::GetBaseStringSettingValue("Main", "Language", "en")));

  // install the base qt translation first
  const QString base_dir(QStringLiteral("%1/translations").arg(qApp->applicationDirPath()));
  const QString base_path(QStringLiteral("%1/qtbase_%2.qm").arg(base_dir).arg(language));
  if (QFile::exists(base_path))
  {
    QTranslator* base_translator = new QTranslator(qApp);
    if (!base_translator->load(base_path))
    {
      QMessageBox::warning(
        nullptr, QStringLiteral("Translation Error"),
        QStringLiteral("Failed to find load base translation file for '%1':\n%2").arg(language).arg(base_path));
      delete base_translator;
    }
    else
    {
      m_translators.push_back(base_translator);
      qApp->installTranslator(base_translator);
    }
  }

  const QString path = QStringLiteral("%1/duckstation-qt_%3.qm").arg(base_dir).arg(language);
  if (!QFile::exists(path))
  {
    QMessageBox::warning(
      nullptr, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to find translation file for language '%1':\n%2").arg(language).arg(path));
    return;
  }

  QTranslator* translator = new QTranslator(qApp);
  if (!translator->load(path))
  {
    QMessageBox::warning(
      nullptr, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to load translation file for language '%1':\n%2").arg(language).arg(path));
    delete translator;
    return;
  }

  Log_InfoPrintf("Loaded translation file for language %s", language.toUtf8().constData());
  qApp->installTranslator(translator);
  m_translators.push_back(translator);

#ifdef _WIN32
  if (language == QStringLiteral("ja"))
  {
    ImGuiManager::SetFontPath(GetFontPath("msgothic.ttc"));
    ImGuiManager::SetFontRange(ImGui::GetIO().Fonts->GetGlyphRangesJapanese());
  }
  else if (language == QStringLiteral("zh-cn"))
  {
    ImGuiManager::SetFontPath(GetFontPath("msyh.ttc"));
    ImGuiManager::SetFontRange(ImGui::GetIO().Fonts->GetGlyphRangesChineseSimplifiedCommon());
  }
#endif
}

void QtHost::ReinstallTranslator()
{
  for (QTranslator* translator : m_translators)
  {
    qApp->removeTranslator(translator);
    translator->deleteLater();
  }
  m_translators.clear();

  InstallTranslator();
}

void EmuThread::applySettings(bool display_osd_messages /* = false */)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "applySettings", Qt::QueuedConnection, Q_ARG(bool, display_osd_messages));
    return;
  }

  System::ApplySettings(display_osd_messages);
}

void EmuThread::reloadGameSettings(bool display_osd_messages /* = false */)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "reloadGameSettings", Qt::QueuedConnection, Q_ARG(bool, display_osd_messages));
    return;
  }

  System::ReloadGameSettings(display_osd_messages);
}

void EmuThread::bootSystem(std::shared_ptr<SystemBootParameters> params)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "bootSystem", Qt::QueuedConnection,
                              Q_ARG(std::shared_ptr<SystemBootParameters>, std::move(params)));
    return;
  }

  setInitialState();

  if (!System::BootSystem(std::move(params)))
    return;

  // force a frame to be drawn to repaint the window
  renderDisplay();
}

void EmuThread::bootOrLoadState(std::string path)
{
  DebugAssert(isOnThread());

  if (System::IsValid())
  {
    System::LoadState(path.c_str());
  }
  else
  {
    std::shared_ptr<SystemBootParameters> params = std::make_shared<SystemBootParameters>();
    params->save_state = std::move(path);
    bootSystem(std::move(params));
  }
}

void EmuThread::resumeSystemFromMostRecentState()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::resumeSystemFromMostRecentState, Qt::QueuedConnection);
    return;
  }

  // shouldn't be doing this with a system running
  if (System::IsValid())
    return;

  std::string state_filename(System::GetMostRecentResumeSaveStatePath());
  if (state_filename.empty())
  {
    emit errorReported(tr("Error"), tr("No resume save state found."));
    return;
  }

  bootOrLoadState(std::move(state_filename));
}

void EmuThread::onDisplayWindowKeyEvent(int key, bool pressed)
{
  DebugAssert(isOnThread());

  InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(key), static_cast<float>(pressed),
                             GenericInputBinding::Unknown);
}

void EmuThread::onDisplayWindowMouseMoveEvent(bool relative, float x, float y)
{
  // display might be null here if the event happened after shutdown
  DebugAssert(isOnThread());
  if (!relative)
  {
    if (s_host_display)
      s_host_display->SetMousePosition(static_cast<s32>(x), static_cast<s32>(y));

    InputManager::UpdatePointerAbsolutePosition(0, x, y);
  }
  else
  {
    if (x != 0.0f)
      InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::X, x);
    if (y != 0.0f)
      InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::Y, y);
  }
}

void EmuThread::onDisplayWindowMouseButtonEvent(int button, bool pressed)
{
  DebugAssert(isOnThread());

  InputManager::InvokeEvents(InputManager::MakePointerButtonKey(0, button), static_cast<float>(pressed),
                             GenericInputBinding::Unknown);
}

void EmuThread::onDisplayWindowMouseWheelEvent(const QPoint& delta_angle)
{
  DebugAssert(isOnThread());

  const float dx = std::clamp(static_cast<float>(delta_angle.x()) / QtUtils::MOUSE_WHEEL_DELTA, -1.0f, 1.0f);
  if (dx != 0.0f)
    InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelX, dx);

  const float dy = std::clamp(static_cast<float>(delta_angle.y()) / QtUtils::MOUSE_WHEEL_DELTA, -1.0f, 1.0f);
  if (dy != 0.0f)
    InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelY, dy);
}

void EmuThread::onDisplayWindowResized(int width, int height)
{
  // this can be null if it was destroyed and the main thread is late catching up
  if (!s_host_display)
    return;

  Log_DevPrintf("Display window resized to %dx%d", width, height);
  s_host_display->ResizeRenderWindow(width, height);
  ImGuiManager::WindowResized();
  System::HostDisplayResized();

  // re-render the display, since otherwise it will be out of date and stretched if paused
  if (!System::IsShutdown())
  {
    if (m_is_exclusive_fullscreen && !s_host_display->IsFullscreen())
    {
      // we lost exclusive fullscreen, switch to borderless
      Host::AddOSDMessage(Host::TranslateStdString("OSDMessage", "Lost exclusive fullscreen."), 10.0f);
      m_is_exclusive_fullscreen = false;
      m_is_fullscreen = false;
      m_lost_exclusive_fullscreen = true;
    }

    // force redraw if we're paused
    if (!FullscreenUI::IsInitialized())
      renderDisplay();
  }
}

void EmuThread::redrawDisplayWindow()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "redrawDisplayWindow", Qt::QueuedConnection);
    return;
  }

  if (!s_host_display || System::IsShutdown())
    return;

  renderDisplay();
}

void EmuThread::toggleFullscreen()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "toggleFullscreen", Qt::QueuedConnection);
    return;
  }

  setFullscreen(!m_is_fullscreen);
}

void EmuThread::setFullscreen(bool fullscreen)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "setFullscreen", Qt::QueuedConnection, Q_ARG(bool, fullscreen));
    return;
  }

  if (!s_host_display || m_is_fullscreen == fullscreen)
    return;

  m_is_fullscreen = fullscreen;
  updateDisplayState();
}

void EmuThread::setSurfaceless(bool surfaceless)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "setSurfaceless", Qt::QueuedConnection, Q_ARG(bool, surfaceless));
    return;
  }

  if (!s_host_display || m_is_surfaceless == surfaceless)
    return;

  m_is_surfaceless = surfaceless;
  updateDisplayState();
}

void EmuThread::requestDisplaySize(float scale)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "requestDisplaySize", Qt::QueuedConnection, Q_ARG(float, scale));
    return;
  }

  if (!System::IsValid())
    return;

  System::RequestDisplaySize(scale);
}

static void createHostDisplay()
{
  Assert(!s_host_display);

  switch (g_settings.gpu_renderer)
  {
    case GPURenderer::HardwareVulkan:
      s_host_display = std::make_unique<FrontendCommon::VulkanHostDisplay>();
      break;

    case GPURenderer::HardwareOpenGL:
#ifndef _WIN32
    default:
#endif
      s_host_display = std::make_unique<FrontendCommon::OpenGLHostDisplay>();
      break;

#ifdef _WIN32
    case GPURenderer::HardwareD3D12:
      s_host_display = std::make_unique<FrontendCommon::D3D12HostDisplay>();
      break;

    case GPURenderer::HardwareD3D11:
    default:
      s_host_display = std::make_unique<FrontendCommon::D3D11HostDisplay>();
      break;
#endif
  }
}

HostDisplay* EmuThread::acquireHostDisplay()
{
  createHostDisplay();

  DisplayWidget* display_widget = createDisplayRequested(m_is_fullscreen, m_is_rendering_to_main);
  if (!display_widget || !s_host_display->HasRenderDevice())
  {
    emit destroyDisplayRequested();
    s_host_display.reset();
    return nullptr;
  }

  if (!s_host_display->MakeRenderContextCurrent() ||
      !s_host_display->InitializeRenderDevice(EmuFolders::Cache, g_settings.gpu_use_debug_device,
                                              g_settings.gpu_threaded_presentation) ||
      !ImGuiManager::Initialize() || !CommonHost::CreateHostDisplayResources())
  {
    ImGuiManager::Shutdown();
    CommonHost::ReleaseHostDisplayResources();
    s_host_display->DestroyRenderDevice();
    emit destroyDisplayRequested();
    s_host_display.reset();
    return nullptr;
  }

  m_is_exclusive_fullscreen = s_host_display->IsFullscreen();
  return s_host_display.get();
}

void EmuThread::connectDisplaySignals(DisplayWidget* widget)
{
  widget->disconnect(this);

  connect(widget, &DisplayWidget::windowResizedEvent, this, &EmuThread::onDisplayWindowResized);
  connect(widget, &DisplayWidget::windowRestoredEvent, this, &EmuThread::redrawDisplayWindow);
  connect(widget, &DisplayWidget::windowKeyEvent, this, &EmuThread::onDisplayWindowKeyEvent);
  connect(widget, &DisplayWidget::windowMouseMoveEvent, this, &EmuThread::onDisplayWindowMouseMoveEvent);
  connect(widget, &DisplayWidget::windowMouseButtonEvent, this, &EmuThread::onDisplayWindowMouseButtonEvent);
  connect(widget, &DisplayWidget::windowMouseWheelEvent, this, &EmuThread::onDisplayWindowMouseWheelEvent);
}

void EmuThread::updateDisplayState()
{
  if (!s_host_display)
    return;

  // this expects the context to get moved back to us afterwards
  s_host_display->DoneRenderContextCurrent();

  DisplayWidget* display_widget =
    updateDisplayRequested(m_is_fullscreen, m_is_rendering_to_main && !m_is_fullscreen, m_is_surfaceless);
  if (!display_widget || !s_host_display->MakeRenderContextCurrent())
    Panic("Failed to make device context current after updating");

  m_is_exclusive_fullscreen = s_host_display->IsFullscreen();
  ImGuiManager::WindowResized();
  System::HostDisplayResized();

  if (!System::IsShutdown())
  {
    System::UpdateSoftwareCursor();

    if (!FullscreenUI::IsInitialized())
      redrawDisplayWindow();
  }

  System::UpdateSpeedLimiterState();
}

void EmuThread::releaseHostDisplay()
{
  Assert(s_host_display);

  CommonHost::ReleaseHostDisplayResources();
  ImGuiManager::Shutdown();
  s_host_display->DestroyRenderDevice();
  emit destroyDisplayRequested();
  s_host_display.reset();
  m_is_fullscreen = false;
}

void Host::OnSystemStarting()
{
  CommonHost::OnSystemStarting();

  emit g_emu_thread->systemStarting();
}

void Host::OnSystemStarted()
{
  CommonHost::OnSystemStarted();

  g_emu_thread->wakeThread();
  g_emu_thread->stopBackgroundControllerPollTimer();

  emit g_emu_thread->systemStarted();
}

void Host::OnSystemPaused()
{
  CommonHost::OnSystemPaused();

  emit g_emu_thread->systemPaused();
  g_emu_thread->startBackgroundControllerPollTimer();
  g_emu_thread->renderDisplay();
}

void Host::OnSystemResumed()
{
  CommonHost::OnSystemResumed();

  emit g_emu_thread->systemResumed();

  g_emu_thread->wakeThread();
  g_emu_thread->stopBackgroundControllerPollTimer();
}

void Host::OnSystemDestroyed()
{
  CommonHost::OnSystemDestroyed();

  g_emu_thread->startBackgroundControllerPollTimer();
  emit g_emu_thread->systemDestroyed();
}

void EmuThread::reloadInputSources()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::reloadInputSources, Qt::QueuedConnection);
    return;
  }

  std::unique_lock<std::mutex> lock = Host::GetSettingsLock();
  SettingsInterface* si = Host::GetSettingsInterface();
  SettingsInterface* bindings_si = Host::GetSettingsInterfaceForBindings();
  InputManager::ReloadSources(*si, lock);
  InputManager::ReloadBindings(*si, *bindings_si);
}

void EmuThread::reloadInputBindings()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::reloadInputBindings, Qt::QueuedConnection);
    return;
  }

  auto lock = Host::GetSettingsLock();
  SettingsInterface* si = Host::GetSettingsInterface();
  SettingsInterface* bindings_si = Host::GetSettingsInterfaceForBindings();
  InputManager::ReloadBindings(*si, *bindings_si);
}

void EmuThread::enumerateInputDevices()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::enumerateInputDevices, Qt::QueuedConnection);
    return;
  }

  const std::vector<std::pair<std::string, std::string>> devs(InputManager::EnumerateDevices());
  QList<QPair<QString, QString>> qdevs;
  qdevs.reserve(devs.size());
  for (const std::pair<std::string, std::string>& dev : devs)
    qdevs.emplace_back(QString::fromStdString(dev.first), QString::fromStdString(dev.second));

  onInputDevicesEnumerated(qdevs);
}

void EmuThread::enumerateVibrationMotors()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::enumerateVibrationMotors, Qt::QueuedConnection);
    return;
  }

  const std::vector<InputBindingKey> motors(InputManager::EnumerateMotors());
  QList<InputBindingKey> qmotors;
  qmotors.reserve(motors.size());
  for (InputBindingKey key : motors)
    qmotors.push_back(key);

  onVibrationMotorsEnumerated(qmotors);
}

void EmuThread::shutdownSystem(bool save_state /* = true */)
{
  if (!isOnThread())
  {
    System::CancelPendingStartup();
    QMetaObject::invokeMethod(this, "shutdownSystem", Qt::QueuedConnection, Q_ARG(bool, save_state));
    return;
  }

  System::ShutdownSystem(save_state);
}

void EmuThread::resetSystem()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::resetSystem, Qt::QueuedConnection);
    return;
  }

  System::ResetSystem();
}

void EmuThread::setSystemPaused(bool paused, bool wait_until_paused /* = false */)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "setSystemPaused",
                              wait_until_paused ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                              Q_ARG(bool, paused), Q_ARG(bool, wait_until_paused));
    return;
  }

  System::PauseSystem(paused);
}

void EmuThread::changeDisc(const QString& new_disc_filename)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "changeDisc", Qt::QueuedConnection, Q_ARG(const QString&, new_disc_filename));
    return;
  }

  if (System::IsShutdown())
    return;

  if (!new_disc_filename.isEmpty())
    System::InsertMedia(new_disc_filename.toStdString().c_str());
  else
    System::RemoveMedia();
}

void EmuThread::changeDiscFromPlaylist(quint32 index)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "changeDiscFromPlaylist", Qt::QueuedConnection, Q_ARG(quint32, index));
    return;
  }

  if (System::IsShutdown())
    return;

  if (!System::SwitchMediaSubImage(index))
    Host::ReportFormattedErrorAsync("Error", "Failed to switch to subimage %u", index);
}

static QString FormatTimestampForSaveStateMenu(u64 timestamp)
{
  const QDateTime qtime(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(timestamp)));
  return qtime.toString(QLocale::system().dateTimeFormat(QLocale::ShortFormat));
}

void EmuThread::populateLoadStateMenu(const char* game_code, QMenu* menu)
{
  auto add_slot = [this, game_code, menu](const QString& title, const QString& empty_title, bool global, s32 slot) {
    std::optional<SaveStateInfo> ssi = System::GetSaveStateInfo(global ? nullptr : game_code, slot);

    const QString menu_title =
      ssi.has_value() ? title.arg(slot).arg(FormatTimestampForSaveStateMenu(ssi->timestamp)) : empty_title.arg(slot);

    QAction* load_action = menu->addAction(menu_title);
    load_action->setEnabled(ssi.has_value());
    if (ssi.has_value())
    {
      const QString path(QString::fromStdString(ssi->path));
      connect(load_action, &QAction::triggered, [this, path]() { loadState(path); });
    }
  };

  menu->clear();

  connect(menu->addAction(tr("Load From File...")), &QAction::triggered, [this]() {
    const QString path(
      QFileDialog::getOpenFileName(g_main_window, tr("Select Save State File"), QString(), tr("Save States (*.sav)")));
    if (path.isEmpty())
      return;

    loadState(path);
  });
  QAction* load_from_state = menu->addAction(tr("Undo Load State"));
  load_from_state->setEnabled(System::CanUndoLoadState());
  connect(load_from_state, &QAction::triggered, this, &EmuThread::undoLoadState);
  menu->addSeparator();

  if (game_code && std::strlen(game_code) > 0)
  {
    for (u32 slot = 1; slot <= System::PER_GAME_SAVE_STATE_SLOTS; slot++)
      add_slot(tr("Game Save %1 (%2)"), tr("Game Save %1 (Empty)"), false, static_cast<s32>(slot));

    menu->addSeparator();
  }

  for (u32 slot = 1; slot <= System::GLOBAL_SAVE_STATE_SLOTS; slot++)
    add_slot(tr("Global Save %1 (%2)"), tr("Global Save %1 (Empty)"), true, static_cast<s32>(slot));
}

void EmuThread::populateSaveStateMenu(const char* game_code, QMenu* menu)
{
  auto add_slot = [this, game_code, menu](const QString& title, const QString& empty_title, bool global, s32 slot) {
    std::optional<SaveStateInfo> ssi = System::GetSaveStateInfo(global ? nullptr : game_code, slot);

    const QString menu_title =
      ssi.has_value() ? title.arg(slot).arg(FormatTimestampForSaveStateMenu(ssi->timestamp)) : empty_title.arg(slot);

    QAction* save_action = menu->addAction(menu_title);
    connect(save_action, &QAction::triggered, [this, global, slot]() { saveState(global, slot); });
  };

  menu->clear();

  connect(menu->addAction(tr("Save To File...")), &QAction::triggered, [this]() {
    if (!System::IsValid())
      return;

    const QString path(
      QFileDialog::getSaveFileName(g_main_window, tr("Select Save State File"), QString(), tr("Save States (*.sav)")));
    if (path.isEmpty())
      return;

    saveState(path);
  });
  menu->addSeparator();

  if (game_code && std::strlen(game_code) > 0)
  {
    for (u32 slot = 1; slot <= System::PER_GAME_SAVE_STATE_SLOTS; slot++)
      add_slot(tr("Game Save %1 (%2)"), tr("Game Save %1 (Empty)"), false, static_cast<s32>(slot));

    menu->addSeparator();
  }

  for (u32 slot = 1; slot <= System::GLOBAL_SAVE_STATE_SLOTS; slot++)
    add_slot(tr("Global Save %1 (%2)"), tr("Global Save %1 (Empty)"), true, static_cast<s32>(slot));
}

void EmuThread::populateChangeDiscSubImageMenu(QMenu* menu, QActionGroup* action_group)
{
  if (!System::IsValid() || !System::HasMediaSubImages())
    return;

  const u32 count = System::GetMediaSubImageCount();
  const u32 current = System::GetMediaSubImageIndex();
  for (u32 i = 0; i < count; i++)
  {
    QAction* action = action_group->addAction(QString::fromStdString(System::GetMediaSubImageTitle(i)));
    action->setCheckable(true);
    action->setChecked(i == current);
    connect(action, &QAction::triggered, [this, i]() { changeDiscFromPlaylist(i); });
    menu->addAction(action);
  }
}

void EmuThread::populateCheatsMenu(QMenu* menu)
{
  Assert(!isOnThread());
  if (!System::IsValid())
    return;

  const bool has_cheat_list = System::HasCheatList();

  QMenu* enabled_menu = menu->addMenu(tr("&Enabled Cheats"));
  enabled_menu->setEnabled(false);
  QMenu* apply_menu = menu->addMenu(tr("&Apply Cheats"));
  apply_menu->setEnabled(false);
  if (has_cheat_list)
  {
    CheatList* cl = System::GetCheatList();
    for (const std::string& group : cl->GetCodeGroups())
    {
      QMenu* enabled_submenu = nullptr;
      QMenu* apply_submenu = nullptr;

      for (u32 i = 0; i < cl->GetCodeCount(); i++)
      {
        CheatCode& cc = cl->GetCode(i);
        if (cc.group != group)
          continue;

        QString desc(QString::fromStdString(cc.description));
        if (cc.IsManuallyActivated())
        {
          if (!apply_submenu)
          {
            apply_menu->setEnabled(true);
            apply_submenu = apply_menu->addMenu(QString::fromStdString(group));
          }

          QAction* action = apply_submenu->addAction(desc);
          connect(action, &QAction::triggered, [this, i]() { applyCheat(i); });
        }
        else
        {
          if (!enabled_submenu)
          {
            enabled_menu->setEnabled(true);
            enabled_submenu = enabled_menu->addMenu(QString::fromStdString(group));
          }

          QAction* action = enabled_submenu->addAction(desc);
          action->setCheckable(true);
          action->setChecked(cc.enabled);
          connect(action, &QAction::toggled, [this, i](bool enabled) { setCheatEnabled(i, enabled); });
        }
      }
    }
  }
}

void EmuThread::loadCheatList(const QString& filename)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "loadCheatList", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  System::LoadCheatList(filename.toUtf8().constData());
}

void EmuThread::setCheatEnabled(quint32 index, bool enabled)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "setCheatEnabled", Qt::QueuedConnection, Q_ARG(quint32, index),
                              Q_ARG(bool, enabled));
    return;
  }

  System::SetCheatCodeState(index, enabled, g_settings.auto_load_cheats);
  emit cheatEnabled(index, enabled);
}

void EmuThread::applyCheat(quint32 index)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "applyCheat", Qt::QueuedConnection, Q_ARG(quint32, index));
    return;
  }

  System::ApplyCheatCode(index);
}

void EmuThread::reloadPostProcessingShaders()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "reloadPostProcessingShaders", Qt::QueuedConnection);
    return;
  }

  System::ReloadPostProcessingShaders();
}

void EmuThread::runOnEmuThread(std::function<void()> callback)
{
  callback();
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
  if (g_emu_thread->isOnThread())
  {
    // probably shouldn't ever happen, but just in case..
    function();
    return;
  }

  QMetaObject::invokeMethod(g_emu_thread, "executeOnEmuThread",
                            block ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                            Q_ARG(const std::function<void()>&, std::move(function)));
}

void QtHost::RunOnUIThread(const std::function<void()>& func, bool block /*= false*/)
{
  // main window always exists, so it's fine to attach it to that.
  QMetaObject::invokeMethod(g_main_window, "runOnUIThread", block ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                            Q_ARG(const std::function<void()>&, func));
}

void EmuThread::loadState(const QString& filename)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "loadState", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  bootOrLoadState(filename.toStdString());
}

void EmuThread::loadState(bool global, qint32 slot)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "loadState", Qt::QueuedConnection, Q_ARG(bool, global), Q_ARG(qint32, slot));
    return;
  }

  // shouldn't even get here if we don't have a running game
  if (!global && System::GetRunningCode().empty())
    return;

  bootOrLoadState(global ? System::GetGlobalSaveStateFileName(slot) :
                           System::GetGameSaveStateFileName(System::GetRunningCode(), slot));
}

void EmuThread::saveState(const QString& filename, bool block_until_done /* = false */)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "saveState", block_until_done ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                              Q_ARG(const QString&, filename), Q_ARG(bool, block_until_done));
    return;
  }

  if (!System::IsValid())
    return;

  System::SaveState(filename.toUtf8().data(), g_settings.create_save_state_backups);
}

void EmuThread::saveState(bool global, qint32 slot, bool block_until_done /* = false */)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "saveState", block_until_done ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                              Q_ARG(bool, global), Q_ARG(qint32, slot), Q_ARG(bool, block_until_done));
    return;
  }

  if (!global && System::GetRunningCode().empty())
    return;

  System::SaveState((global ? System::GetGlobalSaveStateFileName(slot) :
                              System::GetGameSaveStateFileName(System::GetRunningCode(), slot))
                      .c_str(),
                    g_settings.create_save_state_backups);
}

void EmuThread::undoLoadState()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "undoLoadState", Qt::QueuedConnection);
    return;
  }

  System::UndoLoadState();
}

void EmuThread::setAudioOutputVolume(int volume, int fast_forward_volume)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "setAudioOutputVolume", Qt::QueuedConnection, Q_ARG(int, volume),
                              Q_ARG(int, fast_forward_volume));
    return;
  }

  g_settings.audio_output_volume = volume;
  g_settings.audio_fast_forward_volume = fast_forward_volume;
  System::UpdateVolume();
}

void EmuThread::setAudioOutputMuted(bool muted)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "setAudioOutputMuted", Qt::QueuedConnection, Q_ARG(bool, muted));
    return;
  }

  g_settings.audio_output_muted = muted;
  System::UpdateVolume();
}

void EmuThread::startDumpingAudio()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "startDumpingAudio", Qt::QueuedConnection);
    return;
  }

  System::StartDumpingAudio();
}

void EmuThread::stopDumpingAudio()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "stopDumpingAudio", Qt::QueuedConnection);
    return;
  }

  System::StopDumpingAudio();
}

void EmuThread::singleStepCPU()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "singleStepCPU", Qt::BlockingQueuedConnection);
    return;
  }

  if (!System::IsValid())
    return;

  System::SingleStepCPU();
  renderDisplay();
}

void EmuThread::dumpRAM(const QString& filename)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "dumpRAM", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  const std::string filename_str = filename.toStdString();
  if (System::DumpRAM(filename_str.c_str()))
    Host::AddOSDMessage(fmt::format("RAM dumped to '{}'", filename_str), 10.0f);
  else
    Host::ReportErrorAsync("Error", fmt::format("Failed to dump RAM to '{}'", filename_str));
}

void EmuThread::dumpVRAM(const QString& filename)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "dumpVRAM", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  const std::string filename_str = filename.toStdString();
  if (System::DumpVRAM(filename_str.c_str()))
    Host::AddOSDMessage(fmt::format("VRAM dumped to '{}'", filename_str), 10.0f);
  else
    Host::ReportErrorAsync("Error", fmt::format("Failed to dump VRAM to '{}'", filename_str));
}

void EmuThread::dumpSPURAM(const QString& filename)
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "dumpSPURAM", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  const std::string filename_str = filename.toStdString();
  if (System::DumpSPURAM(filename_str.c_str()))
    Host::AddOSDMessage(fmt::format("SPU RAM dumped to '{}'", filename_str), 10.0f);
  else
    Host::ReportErrorAsync("Error", fmt::format("Failed to dump SPU RAM to '{}'", filename_str));
}

void EmuThread::saveScreenshot()
{
  if (!isOnThread())
  {
    QMetaObject::invokeMethod(this, "saveScreenshot", Qt::QueuedConnection);
    return;
  }

  System::SaveScreenshot(nullptr, true, true);
}

void Cheevos::OnAchievementsRefreshed()
{
#ifdef WITH_CHEEVOS
  QString game_info;

  if (Cheevos::HasActiveGame())
  {
    game_info = qApp
                  ->translate("Achievements", "Game ID: %1\n"
                                              "Game Title: %2\n"
                                              "Game Developer: %3\n"
                                              "Game Publisher: %4\n"
                                              "Achievements: %5 (%6)\n\n")
                  .arg(Cheevos::GetGameID())
                  .arg(QString::fromStdString(Cheevos::GetGameTitle()))
                  .arg(QString::fromStdString(Cheevos::GetGameDeveloper()))
                  .arg(QString::fromStdString(Cheevos::GetGamePublisher()))
                  .arg(Cheevos::GetAchievementCount())
                  .arg(qApp->translate("Achievements", "%n points", "", Cheevos::GetMaximumPointsForGame()));

    const std::string& rich_presence_string = Cheevos::GetRichPresenceString();
    if (!rich_presence_string.empty())
      game_info.append(QString::fromStdString(rich_presence_string));
    else
      game_info.append(qApp->translate("Achievements", "Rich presence inactive or unsupported."));
  }
  else
  {
    game_info = qApp->translate("Achievements", "Game not loaded or no RetroAchievements available.");
  }

  emit g_emu_thread->achievementsLoaded(Cheevos::GetGameID(), game_info, Cheevos::GetAchievementCount(),
                                        Cheevos::GetMaximumPointsForGame());
#endif
}

void EmuThread::doBackgroundControllerPoll()
{
  InputManager::PollSources();
}

void EmuThread::createBackgroundControllerPollTimer()
{
  DebugAssert(!m_background_controller_polling_timer);
  m_background_controller_polling_timer = new QTimer(this);
  m_background_controller_polling_timer->setSingleShot(false);
  m_background_controller_polling_timer->setTimerType(Qt::CoarseTimer);
  connect(m_background_controller_polling_timer, &QTimer::timeout, this, &EmuThread::doBackgroundControllerPoll);
}

void EmuThread::destroyBackgroundControllerPollTimer()
{
  delete m_background_controller_polling_timer;
  m_background_controller_polling_timer = nullptr;
}

void EmuThread::startBackgroundControllerPollTimer()
{
  if (m_background_controller_polling_timer->isActive())
    return;

  m_background_controller_polling_timer->start(BACKGROUND_CONTROLLER_POLLING_INTERVAL);
}

void EmuThread::stopBackgroundControllerPollTimer()
{
  if (!m_background_controller_polling_timer->isActive())
    return;

  m_background_controller_polling_timer->stop();
}

void EmuThread::start()
{
  AssertMsg(!g_emu_thread, "Emu thread does not exist");

  g_emu_thread = new EmuThread(QThread::currentThread());
  g_emu_thread->QThread::start();
  g_emu_thread->m_started_semaphore.acquire();
  g_emu_thread->moveToThread(g_emu_thread);
}

void EmuThread::stop()
{
  AssertMsg(g_emu_thread, "Emu thread exists");
  AssertMsg(!g_emu_thread->isOnThread(), "Not called on the emu thread");

  QMetaObject::invokeMethod(g_emu_thread, &EmuThread::stopInThread, Qt::QueuedConnection);
  while (g_emu_thread->isRunning())
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);
}

void EmuThread::stopInThread()
{
  m_shutdown_flag.store(true);
  m_event_loop->quit();
}

void EmuThread::run()
{
  m_event_loop = new QEventLoop();
  m_started_semaphore.release();

  // input source setup must happen on emu thread
  CommonHost::Initialize();

  // bind buttons/axises
  createBackgroundControllerPollTimer();
  startBackgroundControllerPollTimer();

  // main loop
  while (!m_shutdown_flag.load())
  {
    if (System::IsRunning())
    {
      System::Execute();
    }
    else
    {
      // we want to keep rendering the UI when paused and fullscreen UI is enabled
      if (!FullscreenUI::IsInitialized() || !System::IsValid())
      {
        // wait until we have a system before running
        m_event_loop->exec();
        continue;
      }

      m_event_loop->processEvents(QEventLoop::AllEvents);
      InputManager::PollSources();
      renderDisplay();
    }
  }

  if (System::IsValid())
    System::ShutdownSystem(false);

  destroyBackgroundControllerPollTimer();
  CommonHost::Shutdown();

  // move back to UI thread
  moveToThread(m_ui_thread);
}

void EmuThread::renderDisplay()
{
  ImGuiManager::RenderOSD();
  s_host_display->Render();
  ImGuiManager::NewFrame();
}

void Host::InvalidateDisplay()
{
  g_emu_thread->renderDisplay();
}

void Host::RenderDisplay()
{
  g_emu_thread->renderDisplay();
}

void EmuThread::wakeThread()
{
  if (isOnThread())
    m_event_loop->quit();
  else
    QMetaObject::invokeMethod(m_event_loop, "quit", Qt::QueuedConnection);
}

TinyString Host::TranslateString(const char* context, const char* str, const char* disambiguation /*= nullptr*/,
                                 int n /*= -1*/)
{
  const QByteArray bytes(qApp->translate(context, str, disambiguation, n).toUtf8());
  return TinyString(bytes.constData(), bytes.size());
}

std::string Host::TranslateStdString(const char* context, const char* str, const char* disambiguation /*= nullptr*/,
                                     int n /*= -1*/)
{
  return qApp->translate(context, str, disambiguation, n).toStdString();
}

void Host::ReportErrorAsync(const std::string_view& title, const std::string_view& message)
{
  if (!title.empty() && !message.empty())
  {
    Log_ErrorPrintf("ReportErrorAsync: %.*s: %.*s", static_cast<int>(title.size()), title.data(),
                    static_cast<int>(message.size()), message.data());
  }
  else if (!message.empty())
  {
    Log_ErrorPrintf("ReportErrorAsync: %.*s", static_cast<int>(message.size()), message.data());
  }

  QMetaObject::invokeMethod(
    g_main_window, "reportError", Qt::QueuedConnection,
    Q_ARG(const QString&, title.empty() ? QString() : QString::fromUtf8(title.data(), title.size())),
    Q_ARG(const QString&, message.empty() ? QString() : QString::fromUtf8(message.data(), message.size())));
}

bool Host::ConfirmMessage(const std::string_view& title, const std::string_view& message)
{
  return emit g_emu_thread->messageConfirmed(QString::fromUtf8(title.data(), title.size()),
                                             QString::fromUtf8(message.data(), message.size()));
}

void Host::ReportDebuggerMessage(const std::string_view& message)
{
  emit g_emu_thread->debuggerMessageReported(QString::fromUtf8(message));
}

void Host::OnInputDeviceConnected(const std::string_view& identifier, const std::string_view& device_name)
{
  emit g_emu_thread->onInputDeviceConnected(
    identifier.empty() ? QString() : QString::fromUtf8(identifier.data(), identifier.size()),
    device_name.empty() ? QString() : QString::fromUtf8(device_name.data(), device_name.size()));
}

void Host::OnInputDeviceDisconnected(const std::string_view& identifier)
{
  emit g_emu_thread->onInputDeviceDisconnected(
    identifier.empty() ? QString() : QString::fromUtf8(identifier.data(), identifier.size()));
}

std::optional<std::vector<u8>> Host::ReadResourceFile(const char* filename)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  std::optional<std::vector<u8>> ret(FileSystem::ReadBinaryFile(path.c_str()));
  if (!ret.has_value())
    Log_ErrorPrintf("Failed to read resource file '%s'", filename);
  return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(const char* filename)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  std::optional<std::string> ret(FileSystem::ReadFileToString(path.c_str()));
  if (!ret.has_value())
    Log_ErrorPrintf("Failed to read resource file to string '%s'", filename);
  return ret;
}

void Host::SetBaseBoolSettingValue(const char* section, const char* key, bool value)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetBoolValue(section, key, value);
  QtHost::QueueSettingsSave();
}

void Host::SetBaseIntSettingValue(const char* section, const char* key, int value)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetIntValue(section, key, value);
  QtHost::QueueSettingsSave();
}

void Host::SetBaseFloatSettingValue(const char* section, const char* key, float value)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetFloatValue(section, key, value);
  QtHost::QueueSettingsSave();
}

void Host::SetBaseStringSettingValue(const char* section, const char* key, const char* value)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetStringValue(section, key, value);
  QtHost::QueueSettingsSave();
}

void Host::SetBaseStringListSettingValue(const char* section, const char* key, const std::vector<std::string>& values)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetStringList(section, key, values);
  QtHost::QueueSettingsSave();
}

bool Host::AddValueToBaseStringListSetting(const char* section, const char* key, const char* value)
{
  auto lock = Host::GetSettingsLock();
  if (!s_base_settings_interface->AddToStringList(section, key, value))
    return false;

  QtHost::QueueSettingsSave();
  return true;
}

bool Host::RemoveValueFromBaseStringListSetting(const char* section, const char* key, const char* value)
{
  auto lock = Host::GetSettingsLock();
  if (!s_base_settings_interface->RemoveFromStringList(section, key, value))
    return false;

  QtHost::QueueSettingsSave();
  return true;
}

void Host::DeleteBaseSettingValue(const char* section, const char* key)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->DeleteValue(section, key);
  QtHost::QueueSettingsSave();
}

void Host::CommitBaseSettingChanges()
{
  if (g_emu_thread->isOnThread())
    QtHost::RunOnUIThread([]() { QtHost::QueueSettingsSave(); });
  else
    QtHost::QueueSettingsSave();
}

HostDisplay* Host::AcquireHostDisplay()
{
  return g_emu_thread->acquireHostDisplay();
}

void Host::ReleaseHostDisplay()
{
  g_emu_thread->releaseHostDisplay();
}

HostDisplay* Host::GetHostDisplay()
{
  return s_host_display.get();
}

void Host::OnPerformanceMetricsUpdated()
{
  GPURenderer renderer = GPURenderer::Count;
  u32 render_width = 0;
  u32 render_height = 0;
  bool render_interlaced = false;

  if (g_gpu)
  {
    renderer = g_gpu->GetRendererType();
    std::tie(render_width, render_height) = g_gpu->GetEffectiveDisplayResolution();
    render_interlaced = g_gpu->IsInterlacedDisplayEnabled();
  }

  emit g_emu_thread->systemPerformanceCountersUpdated(System::GetEmulationSpeed(), System::GetFPS(), System::GetVPS(),
                                                      System::GetAverageFrameTime(), System::GetWorstFrameTime(),
                                                      renderer, render_width, render_height, render_interlaced);
}

void Host::OnGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name)
{
  emit g_emu_thread->runningGameChanged(QString::fromStdString(disc_path), QString::fromStdString(game_serial),
                                        QString::fromStdString(game_name));
}

void Host::SetMouseMode(bool relative, bool hide_cursor)
{
  emit g_emu_thread->mouseModeRequested(relative, hide_cursor);
}

void Host::PumpMessagesOnCPUThread()
{
  g_emu_thread->getEventLoop()->processEvents(QEventLoop::AllEvents);
  CommonHost::PumpMessagesOnCPUThread(); // calls InputManager::PollSources()
}

void QtHost::SaveSettings()
{
  AssertMsg(!g_emu_thread->isOnThread(), "Saving should happen on the UI thread.");

  {
    auto lock = Host::GetSettingsLock();
    if (!s_base_settings_interface->Save())
      Log_ErrorPrintf("Failed to save settings.");
  }

  s_settings_save_timer->deleteLater();
  s_settings_save_timer.release();
}

void QtHost::QueueSettingsSave()
{
  if (s_settings_save_timer)
    return;

  // TODO: Thread check here
  Assert(!g_emu_thread->isOnThread());

  s_settings_save_timer = std::make_unique<QTimer>();
  s_settings_save_timer->connect(s_settings_save_timer.get(), &QTimer::timeout, SaveSettings);
  s_settings_save_timer->setSingleShot(true);
  s_settings_save_timer->start(SETTINGS_SAVE_DELAY);
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

static void SignalHandler(int signal)
{
  // First try the normal (graceful) shutdown/exit.
  static bool graceful_shutdown_attempted = false;
  if (!graceful_shutdown_attempted && g_main_window)
  {
    std::fprintf(stderr, "Received CTRL+C, attempting graceful shutdown. Press CTRL+C again to force.\n");
    graceful_shutdown_attempted = true;

    // This could be a bit risky invoking from a signal handler... hopefully it's okay.
    QMetaObject::invokeMethod(g_main_window, &MainWindow::requestExit, Qt::QueuedConnection);
    return;
  }

  std::signal(signal, SIG_DFL);

  // MacOS is missing std::quick_exit() despite it being C++11...
#ifndef __APPLE__
  std::quick_exit(1);
#else
  _Exit(1);
#endif
}

void QtHost::HookSignals()
{
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
}

static bool ParseCommandLineParameters(QApplication& app, std::unique_ptr<SystemBootParameters>* boot_params)
{
  const QStringList args(app.arguments());
  std::vector<std::string> converted_args;
  std::vector<char*> converted_argv;
  converted_args.reserve(args.size());
  converted_argv.reserve(args.size());

  for (const QString& arg : args)
    converted_args.push_back(arg.toStdString());

  for (std::string& arg : converted_args)
    converted_argv.push_back(arg.data());

  return CommonHost::ParseCommandLineParameters(args.size(), converted_argv.data(), boot_params);
}

int main(int argc, char* argv[])
{
  CrashHandler::Install();

  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
  QtHost::RegisterTypes();

  QApplication app(argc, argv);

  // TODO: Remove me
  Log::SetFilterLevel(LOGLEVEL_DEBUG);
  Log::SetConsoleOutputParams(true, nullptr, LOGLEVEL_DEBUG);

  std::unique_ptr<SystemBootParameters> autoboot;
  if (!ParseCommandLineParameters(app, &autoboot))
    return EXIT_FAILURE;

  // Bail out if we can't find any config.
  if (!QtHost::InitializeConfig())
  {
    // NOTE: No point translating this, because no config means the language won't be loaded anyway.
    QMessageBox::critical(nullptr, QStringLiteral("Error"), QStringLiteral("Failed to initialize config."));
    return EXIT_FAILURE;
  }

  // Start up the CPU thread.
  MainWindow* main_window = new MainWindow();
  QtHost::HookSignals();
  EmuThread::start();

  // Create all window objects, the emuthread might still be starting up at this point.
  main_window->initialize();

  // When running in batch mode, ensure game list is loaded, but don't scan for any new files.
  if (!s_batch_mode)
    main_window->refreshGameList(false);
  else
    GameList::Refresh(false, true);

  // Don't bother showing the window in no-gui mode.
  if (!s_nogui_mode)
    main_window->show();

  // Initialize big picture mode if requested.
  if (s_start_fullscreen_ui)
  {
    // g_emu_thread->startFullscreenUI();
  }

  // Skip the update check if we're booting a game directly.
  if (autoboot)
    g_emu_thread->bootSystem(std::move(autoboot));
  else if (!s_nogui_mode)
    main_window->startupUpdateCheck();

  // This doesn't return until we exit.
  const int result = app.exec();

  // Shutting down.
  EmuThread::stop();
  if (g_main_window)
  {
    g_main_window->close();
    delete g_main_window;
  }

  // Ensure log is flushed.
  Log::SetFileOutputParams(false, nullptr);

  return result;
}
