#include "generalsettingswidget.h"
#include "autoupdaterdialog.h"
#include "mainwindow.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"

GeneralSettingsWidget::GeneralSettingsWidget(SettingsDialog* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pauseOnStart, "Main", "StartPaused", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pauseOnFocusLoss, "Main", "PauseOnFocusLoss", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.startFullscreen, "Main", "StartFullscreen", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.hideCursorInFullscreen, "Main", "HideCursorInFullscreen",
                                               true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.inhibitScreensaver, "Main", "InhibitScreensaver", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.renderToMain, "Main", "RenderToMainWindow", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.saveStateOnExit, "Main", "SaveStateOnExit", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.confirmPowerOff, "Main", "ConfirmPowerOff", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.loadDevicesFromSaveStates, "Main", "LoadDevicesFromSaveStates",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.applyGameSettings, "Main", "ApplyGameSettings", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.autoLoadCheats, "Main", "AutoLoadCheats", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableFullscreenUI, "Main", "EnableFullscreenUI", false);

  dialog->registerWidgetHelp(
    m_ui.confirmPowerOff, tr("Confirm Power Off"), tr("Checked"),
    tr("Determines whether a prompt will be displayed to confirm shutting down the emulator/game "
       "when the hotkey is pressed."));
  dialog->registerWidgetHelp(m_ui.saveStateOnExit, tr("Save State On Exit"), tr("Checked"),
                             tr("Automatically saves the emulator state when powering down or exiting. You can then "
                                "resume directly from where you left off next time."));
  dialog->registerWidgetHelp(m_ui.startFullscreen, tr("Start Fullscreen"), tr("Unchecked"),
                             tr("Automatically switches to fullscreen mode when a game is started."));
  dialog->registerWidgetHelp(m_ui.hideCursorInFullscreen, tr("Hide Cursor In Fullscreen"), tr("Checked"),
                             tr("Hides the mouse pointer/cursor when the emulator is in fullscreen mode."));
  dialog->registerWidgetHelp(
    m_ui.inhibitScreensaver, tr("Inhibit Screensaver"), tr("Checked"),
    tr("Prevents the screen saver from activating and the host from sleeping while emulation is running."));
  dialog->registerWidgetHelp(
    m_ui.renderToMain, tr("Render To Main Window"), tr("Checked"),
    tr("Renders the display of the simulated console to the main window of the application, over "
       "the game list. If unchecked, the display will render in a separate window."));
  dialog->registerWidgetHelp(m_ui.pauseOnStart, tr("Pause On Start"), tr("Unchecked"),
                             tr("Pauses the emulator when a game is started."));
  dialog->registerWidgetHelp(m_ui.pauseOnFocusLoss, tr("Pause On Focus Loss"), tr("Unchecked"),
                             tr("Pauses the emulator when you minimize the window or switch to another application, "
                                "and unpauses when you switch back."));
  dialog->registerWidgetHelp(
    m_ui.loadDevicesFromSaveStates, tr("Load Devices From Save States"), tr("Unchecked"),
    tr("When enabled, memory cards and controllers will be overwritten when save states are loaded. This can "
       "result in lost saves, and controller type mismatches. For deterministic save states, enable this option, "
       "otherwise leave disabled."));
  dialog->registerWidgetHelp(
    m_ui.applyGameSettings, tr("Apply Per-Game Settings"), tr("Checked"),
    tr("When enabled, per-game settings will be applied, and incompatible enhancements will be disabled. You should "
       "leave this option enabled except when testing enhancements with incompatible games."));
  dialog->registerWidgetHelp(m_ui.autoLoadCheats, tr("Automatically Load Cheats"), tr("Unchecked"),
                             tr("Automatically loads and applies cheats on game start."));
  dialog->registerWidgetHelp(
    m_ui.enableFullscreenUI, tr("Enable Fullscreen UI"), tr("Unchecked"),
    tr("Enables the fullscreen UI mode, suitable for controller operation which is used in the NoGUI frontend."));

  // Since this one is compile-time selected, we don't put it in the .ui file.
  int current_col = 0;
  int current_row = m_ui.formLayout_4->rowCount() - current_col;
#ifdef WITH_DISCORD_PRESENCE
  {
    QCheckBox* enableDiscordPresence = new QCheckBox(tr("Enable Discord Presence"), m_ui.groupBox_4);
    SettingWidgetBinder::BindWidgetToBoolSetting(sif, enableDiscordPresence, "Main", "EnableDiscordPresence", false);
    m_ui.formLayout_4->addWidget(enableDiscordPresence, current_row, current_col);
    dialog->registerWidgetHelp(enableDiscordPresence, tr("Enable Discord Presence"), tr("Unchecked"),
                               tr("Shows the game you are currently playing as part of your profile in Discord."));
    current_col++;
    current_row += (current_col / 2);
    current_col %= 2;
  }
#endif
  if (AutoUpdaterDialog::isSupported())
  {
    SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.autoUpdateEnabled, "AutoUpdater", "CheckAtStartup", true);
    dialog->registerWidgetHelp(m_ui.autoUpdateEnabled, tr("Enable Automatic Update Check"), tr("Checked"),
                               tr("Automatically checks for updates to the program on startup. Updates can be deferred "
                                  "until later or skipped entirely."));

    m_ui.autoUpdateTag->addItems(AutoUpdaterDialog::getTagList());
    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.autoUpdateTag, "AutoUpdater", "UpdateTag",
                                                   AutoUpdaterDialog::getDefaultTag());

    m_ui.autoUpdateCurrentVersion->setText(tr("%1 (%2)").arg(g_scm_tag_str).arg(g_scm_date_str));
    connect(m_ui.checkForUpdates, &QPushButton::clicked, [this]() { g_main_window->checkForUpdates(true); });
    current_col++;
    current_row += (current_col / 2);
    current_col %= 2;
  }
  else
  {
    m_ui.verticalLayout->removeWidget(m_ui.automaticUpdaterGroup);
    m_ui.automaticUpdaterGroup->hide();
  }
}

GeneralSettingsWidget::~GeneralSettingsWidget() = default;
