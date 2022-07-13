#pragma once
#include "common/event.h"
#include "core/host.h"
#include "core/host_settings.h"
#include "core/system.h"
#include "frontend-common/common_host.h"
#include "frontend-common/game_list.h"
#include "frontend-common/input_manager.h"
#include "qtutils.h"
#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QThread>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

class ByteStream;

class QActionGroup;
class QEventLoop;
class QMenu;
class QWidget;
class QTimer;
class QTranslator;

class INISettingsInterface;

class HostDisplay;

class MainWindow;
class QtDisplayWidget;

Q_DECLARE_METATYPE(std::shared_ptr<SystemBootParameters>);
Q_DECLARE_METATYPE(const GameListEntry*);
Q_DECLARE_METATYPE(GPURenderer);

class QtHostInterface final : public QObject
{
  Q_OBJECT

public:
  explicit QtHostInterface(QObject* parent = nullptr);
  ~QtHostInterface();

  bool Initialize();
  void Shutdown();

  void RunLater(std::function<void()> func);

public:
  ALWAYS_INLINE const GameList* getGameList() const { return CommonHost::GetGameList(); }
  ALWAYS_INLINE GameList* getGameList() { return CommonHost::GetGameList(); }
  void refreshGameList(bool invalidate_cache = false, bool invalidate_database = false);

  ALWAYS_INLINE void requestExit() { RequestExit(); }

  ALWAYS_INLINE bool isOnWorkerThread() const { return QThread::currentThread() == m_worker_thread; }

  ALWAYS_INLINE MainWindow* getMainWindow() const { return m_main_window; }
  void setMainWindow(MainWindow* window);


  ALWAYS_INLINE QEventLoop* getEventLoop() const { return m_worker_thread_event_loop; }

  void reinstallTranslator();

  void populateLoadStateMenu(const char* game_code, QMenu* menu);
  void populateSaveStateMenu(const char* game_code, QMenu* menu);

  /// Fills menu with save state info and handlers.
  void populateGameListContextMenu(const GameListEntry* entry, QWidget* parent_window, QMenu* menu);

  /// Fills menu with the current playlist entries. The disc index is marked as checked.
  void populateChangeDiscSubImageMenu(QMenu* menu, QActionGroup* action_group);

  /// Fills menu with the current cheat options.
  void populateCheatsMenu(QMenu* menu);

  void saveInputProfile(const QString& profile_path);

  /// Returns a list of supported languages and codes (suffixes for translation files).
  static std::vector<std::pair<QString, QString>> getAvailableLanguageList();

  /// Called back from the GS thread when the display state changes (e.g. fullscreen, render to main).
  static HostDisplay* createHostDisplay();
  HostDisplay* acquireHostDisplay();
  void connectDisplaySignals(QtDisplayWidget* widget);
  void releaseHostDisplay();
  void updateDisplay();
  void renderDisplay();

  void onSystemStarted();
  void onSystemPaused();
  void onSystemResumed();
  void onSystemDestroyed();

Q_SIGNALS:
  void errorReported(const QString& title, const QString& message);
  bool messageConfirmed(const QString& title, const QString& message);
  void debuggerMessageReported(const QString& message);
  void settingsResetToDefault();
  void onInputDevicesEnumerated(const QList<QPair<QString, QString>>& devices);
  void onInputDeviceConnected(const QString& identifier, const QString& device_name);
  void onInputDeviceDisconnected(const QString& identifier);
  void onVibrationMotorsEnumerated(const QList<InputBindingKey>& motors);
  void emulationStarting();
  void emulationStarted();
  void emulationStopped();
  void emulationPaused(bool paused);
  void gameListRefreshed();
  QtDisplayWidget* createDisplayRequested(QThread* worker_thread, bool fullscreen, bool render_to_main);
  QtDisplayWidget* updateDisplayRequested(QThread* worker_thread, bool fullscreen, bool render_to_main);
  void displaySizeRequested(qint32 width, qint32 height);
  void focusDisplayWidgetRequested();
  void destroyDisplayRequested();
  void systemPerformanceCountersUpdated(float speed, float fps, float vps, float avg_frame_time, float worst_frame_time,
                                        GPURenderer renderer, quint32 render_width, quint32 render_height,
                                        bool render_interlaced);
  void runningGameChanged(const QString& filename, const QString& game_code, const QString& game_title);
  void exitRequested();
  void inputProfileLoaded();
  void mouseModeRequested(bool relative, bool hide_cursor);
  void achievementsLoaded(quint32 id, const QString& game_info_string, quint32 total, quint32 points);
  void cheatEnabled(quint32 index, bool enabled);

public Q_SLOTS:
  void setDefaultSettings();
  void applySettings(bool display_osd_messages = false);
  void reloadGameSettings();
  void applyInputProfile(const QString& profile_path);
  void reloadInputSources();
  void reloadInputBindings();
  void enumerateInputDevices();
  void enumerateVibrationMotors();
  void bootSystem(std::shared_ptr<SystemBootParameters> params);
  void resumeSystemFromState(const QString& filename, bool boot_on_failure);
  void resumeSystemFromMostRecentState();
  void powerOffSystem();
  void powerOffSystemWithoutSaving();
  void synchronousPowerOffSystem();
  void resetSystem();
  void pauseSystem(bool paused, bool wait_until_paused = false);
  void changeDisc(const QString& new_disc_filename);
  void changeDiscFromPlaylist(quint32 index);
  void loadState(const QString& filename);
  void loadState(bool global, qint32 slot);
  void saveState(const QString& filename, bool block_until_done = false);
  void saveState(bool global, qint32 slot, bool block_until_done = false);
  void undoLoadState();
  void setAudioOutputVolume(int volume, int fast_forward_volume);
  void setAudioOutputMuted(bool muted);
  void startDumpingAudio();
  void stopDumpingAudio();
  void singleStepCPU();
  void dumpRAM(const QString& filename);
  void dumpVRAM(const QString& filename);
  void dumpSPURAM(const QString& filename);
  void saveScreenshot();
  void redrawDisplayWindow();
  void toggleFullscreen();
  void loadCheatList(const QString& filename);
  void setCheatEnabled(quint32 index, bool enabled);
  void applyCheat(quint32 index);
  void reloadPostProcessingShaders();
  void requestRenderWindowScale(qreal scale);
  void executeOnEmulationThread(std::function<void()> callback, bool wait = false);

private Q_SLOTS:
  void doStopThread();
  void onDisplayWindowMouseMoveEvent(float x, float y);
  void onDisplayWindowMouseButtonEvent(int button, bool pressed);
  void onDisplayWindowMouseWheelEvent(const QPoint& delta_angle);
  void onDisplayWindowResized(int width, int height);
  void onDisplayWindowFocused();
  void onDisplayWindowKeyEvent(int key, bool pressed);
  void doBackgroundControllerPoll();

protected:
  bool IsFullscreen() const;
  bool SetFullscreen(bool enabled);

  void RequestExit();

private:
  enum : u32
  {
    BACKGROUND_CONTROLLER_POLLING_INTERVAL =
      100, /// Interval at which the controllers are polled when the system is not active.
  };

  using InputButtonHandler = std::function<void(bool)>;
  using InputAxisHandler = std::function<void(float)>;

  class Thread : public QThread
  {
  public:
    Thread(QtHostInterface* parent);
    ~Thread();

    void setInitResult(bool result);
    bool waitForInit();

  protected:
    void run() override;

  private:
    QtHostInterface* m_parent;
    std::atomic_bool m_init_result{false};
    Common::Event m_init_event;
  };

  void createBackgroundControllerPollTimer();
  void destroyBackgroundControllerPollTimer();
  void startBackgroundControllerPollTimer();
  void stopBackgroundControllerPollTimer();

  void setImGuiFont();

  void createThread();
  void stopThread();
  void threadEntryPoint();
  bool initializeOnThread();
  void shutdownOnThread();
  void installTranslator();
  void checkRenderToMainState();
  void updateDisplayState();
  void queueSettingsSave();
  void wakeThread();

  MainWindow* m_main_window = nullptr;
  QThread* m_original_thread = nullptr;
  Thread* m_worker_thread = nullptr;
  QEventLoop* m_worker_thread_event_loop = nullptr;
  Common::Event m_worker_thread_sync_execute_done;

  std::atomic_bool m_shutdown_flag{false};

  QTimer* m_background_controller_polling_timer = nullptr;
  std::vector<QTranslator*> m_translators;

  bool m_is_rendering_to_main = false;
  bool m_is_fullscreen = false;
  bool m_is_exclusive_fullscreen = false;
  bool m_lost_exclusive_fullscreen = false;
};

extern QtHostInterface* g_emu_thread;

namespace QtHost {
/// Sets batch mode (exit after game shutdown).
bool InBatchMode();

/// Sets NoGUI mode (implys batch mode, does not display main window, exits on shutdown).
bool InNoGUIMode();

/// Executes a function on the UI thread.
void RunOnUIThread(const std::function<void()>& func, bool block = false);

/// Returns the application name and version, optionally including debug/devel config indicator.
// QString GetAppNameAndVersion();

/// Returns the debug/devel config indicator.
// QString GetAppConfigSuffix();

/// Returns the base path for resources. This may be : prefixed, if we're using embedded resources.
// QString GetResourcesBasePath();

/// Thread-safe settings access.
void QueueSettingsSave();
} // namespace QtHost
