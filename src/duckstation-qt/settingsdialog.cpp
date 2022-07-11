#include "settingsdialog.h"
#include "advancedsettingswidget.h"
#include "audiosettingswidget.h"
#include "biossettingswidget.h"
#include "consolesettingswidget.h"
#include "core/host.h"
#include "core/host_settings.h"
#include "displaysettingswidget.h"
#include "emulationsettingswidget.h"
#include "enhancementsettingswidget.h"
#include "gamelistsettingswidget.h"
#include "generalsettingswidget.h"
#include "memorycardsettingswidget.h"
#include "postprocessingsettingswidget.h"
#include "qthostinterface.h"
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QTextEdit>

#ifdef WITH_CHEEVOS
#include "achievementsettingswidget.h"
#include "frontend-common/cheevos.h"
#endif

static constexpr char DEFAULT_SETTING_HELP_TEXT[] = "";

static QList<SettingsDialog*> s_open_game_properties_dialogs;

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent)
{
  setupUi(nullptr);
}

SettingsDialog::SettingsDialog(QWidget* parent, std::unique_ptr<SettingsInterface> sif, const GameListEntry* game,
                               std::string serial)
  : QDialog(parent), m_sif(std::move(sif))
{
  setupUi(game);

  s_open_game_properties_dialogs.push_back(this);
}

void SettingsDialog::setupUi(const GameListEntry* game)
{
  m_ui.setupUi(this);
  setCategoryHelpTexts();

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  m_general_settings = new GeneralSettingsWidget(this, m_ui.settingsContainer);
  m_bios_settings = new BIOSSettingsWidget(this, m_ui.settingsContainer);
  m_console_settings = new ConsoleSettingsWidget(this, m_ui.settingsContainer);
  m_emulation_settings = new EmulationSettingsWidget(this, m_ui.settingsContainer);
  m_game_list_settings = new GameListSettingsWidget(this, m_ui.settingsContainer);
  m_memory_card_settings = new MemoryCardSettingsWidget(this, m_ui.settingsContainer);
  m_display_settings = new DisplaySettingsWidget(this, m_ui.settingsContainer);
  m_enhancement_settings = new EnhancementSettingsWidget(this, m_ui.settingsContainer);
  m_post_processing_settings = new PostProcessingSettingsWidget(this, m_ui.settingsContainer);
  m_audio_settings = new AudioSettingsWidget(this, m_ui.settingsContainer);
  m_advanced_settings = new AdvancedSettingsWidget(this, m_ui.settingsContainer);

#ifdef WITH_CHEEVOS
  if (!Cheevos::IsUsingRAIntegration())
    m_achievement_settings = new AchievementSettingsWidget(this, m_ui.settingsContainer);
#endif

  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::GeneralSettings), m_general_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::BIOSSettings), m_bios_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::ConsoleSettings), m_console_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::EmulationSettings), m_emulation_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::GameListSettings), m_game_list_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::MemoryCardSettings), m_memory_card_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::DisplaySettings), m_display_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::EnhancementSettings), m_enhancement_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::PostProcessingSettings), m_post_processing_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::AudioSettings), m_audio_settings);

#ifdef WITH_CHEEVOS
  if (Cheevos::IsUsingRAIntegration())
  {
    QLabel* placeholder_label =
      new QLabel(QStringLiteral("RAIntegration is being used, built-in RetroAchievements support is disabled."),
                 m_ui.settingsContainer);
    placeholder_label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_ui.settingsContainer->insertWidget(static_cast<int>(Category::AchievementSettings), placeholder_label);
  }
  else
  {
    m_ui.settingsContainer->insertWidget(static_cast<int>(Category::AchievementSettings), m_achievement_settings);
  }
#else
  QLabel* placeholder_label =
    new QLabel(tr("This DuckStation build was not compiled with RetroAchievements support."), m_ui.settingsContainer);
  placeholder_label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::AchievementSettings), placeholder_label);
#endif

  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::AdvancedSettings), m_advanced_settings);

  m_ui.settingsCategory->setCurrentRow(0);
  m_ui.settingsContainer->setCurrentIndex(0);
  m_ui.helpText->setText(m_category_help_text[0]);
  connect(m_ui.settingsCategory, &QListWidget::currentRowChanged, this, &SettingsDialog::onCategoryCurrentRowChanged);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &SettingsDialog::accept);
  connect(m_ui.buttonBox, &QDialogButtonBox::clicked, this, [this](QAbstractButton* button) {
    if (m_ui.buttonBox->buttonRole(button) == QDialogButtonBox::ResetRole)
    {
      onRestoreDefaultsClicked();
    }
  });
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::setCategoryHelpTexts()
{
  m_category_help_text[static_cast<int>(Category::GeneralSettings)] = tr(
    "<strong>General Settings</strong><hr>These options control how the emulator looks and behaves.<br><br>Mouse over "
    "an option for additional information.");
  m_category_help_text[static_cast<int>(Category::ConsoleSettings)] =
    tr("<strong>Console Settings</strong><hr>These options determine the configuration of the simulated "
       "console.<br><br>Mouse over an option for additional information.");
  m_category_help_text[static_cast<int>(Category::GameListSettings)] =
    tr("<strong>Game List Settings</strong><hr>The list above shows the directories which will be searched by "
       "DuckStation to populate the game list. Search directories can be added, removed, and switched to "
       "recursive/non-recursive.");
#if 0
  m_category_help_text[static_cast<int>(Category::HotkeySettings)] = tr(
    "<strong>Hotkey Settings</strong><hr>Binding a hotkey allows you to trigger events such as a resetting or taking "
    "screenshots at the press of a key/controller button. Hotkey titles are self-explanatory. Clicking a binding will "
    "start a countdown, in which case you should press the key or controller button/axis you wish to bind. If no "
    "button  is pressed and the timer lapses, the binding will be unchanged. To clear a binding, right-click the "
    "button. To  bind multiple buttons, hold Shift and click the button.");
  m_category_help_text[static_cast<int>(Category::ControllerSettings)] = tr(
    "<strong>Controller Settings</strong><hr>This page lets you choose the type of controller you wish to simulate for "
    "the console, and rebind the keys or host game controller buttons to your choosing. Clicking a binding will start "
    "a countdown, in which case you should press the key or controller button/axis you wish to bind. (For rumble, "
    "press any button/axis on the controller you wish to send rumble to.) If no button is pressed and the timer "
    "lapses, the binding will be unchanged. To clear a binding, right-click the button. To bind multiple buttons, hold "
    "Shift and click the button.");
#endif
  m_category_help_text[static_cast<int>(Category::MemoryCardSettings)] =
    tr("<strong>Memory Card Settings</strong><hr>This page lets you control what mode the memory card emulation will "
       "function in, and where the images for these cards will be stored on disk.");
  m_category_help_text[static_cast<int>(Category::DisplaySettings)] =
    tr("<strong>Display Settings</strong><hr>These options control the how the frames generated by the console are "
       "displayed on the screen.");
  m_category_help_text[static_cast<int>(Category::EnhancementSettings)] =
    tr("<strong>Enhancement Settings</strong><hr>These options control enhancements which can improve visuals compared "
       "to the original console. Mouse over each option for additional information.");
  m_category_help_text[static_cast<int>(Category::PostProcessingSettings)] =
    tr("<strong>Post-Processing Settings</strong><hr>Post processing allows you to alter the appearance of the image "
       "displayed on the screen with various filters. Shaders will be executed in sequence.");
  m_category_help_text[static_cast<int>(Category::AudioSettings)] =
    tr("<strong>Audio Settings</strong><hr>These options control the audio output of the console. Mouse over an option "
       "for additional information.");
  m_category_help_text[static_cast<int>(Category::AdvancedSettings)] = tr(
    "<strong>Advanced Settings</strong><hr>These options control logging and internal behavior of the emulator. Mouse "
    "over an option for additional information.");
}

void SettingsDialog::setCategory(Category category)
{
  if (category >= Category::Count)
    return;

  m_ui.settingsCategory->setCurrentRow(static_cast<int>(category));
}

void SettingsDialog::onCategoryCurrentRowChanged(int row)
{
  Q_ASSERT(row < static_cast<int>(Category::Count));
  m_ui.settingsContainer->setCurrentIndex(row);
  m_ui.helpText->setText(m_category_help_text[row]);
}

void SettingsDialog::onRestoreDefaultsClicked()
{
  if (QMessageBox::question(this, tr("Confirm Restore Defaults"),
                            tr("Are you sure you want to restore the default settings? Any preferences will be lost."),
                            QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
  {
    return;
  }

  QtHostInterface::GetInstance()->setDefaultSettings();
}

void SettingsDialog::registerWidgetHelp(QObject* object, QString title, QString recommended_value, QString text)
{
  // construct rich text with formatted description
  QString full_text;
  full_text += "<table width='100%' cellpadding='0' cellspacing='0'><tr><td><strong>";
  full_text += title;
  full_text += "</strong></td><td align='right'><strong>";
  full_text += tr("Recommended Value");
  full_text += ": </strong>";
  full_text += recommended_value;
  full_text += "</td></table><hr>";
  full_text += text;

  m_widget_help_text_map[object] = std::move(full_text);
  object->installEventFilter(this);
}

bool SettingsDialog::eventFilter(QObject* object, QEvent* event)
{
  if (event->type() == QEvent::Enter)
  {
    auto iter = m_widget_help_text_map.constFind(object);
    if (iter != m_widget_help_text_map.end())
    {
      m_current_help_widget = object;
      m_ui.helpText->setText(iter.value());
    }
  }
  else if (event->type() == QEvent::Leave)
  {
    if (m_current_help_widget)
    {
      m_current_help_widget = nullptr;
      m_ui.helpText->setText(m_category_help_text[m_ui.settingsCategory->currentRow()]);
    }
  }

  return QDialog::eventFilter(object, event);
}

bool SettingsDialog::getEffectiveBoolValue(const char* section, const char* key, bool default_value) const
{
  bool value;
  if (m_sif && m_sif->GetBoolValue(section, key, &value))
    return value;
  else
    return Host::GetBaseBoolSettingValue(section, key, default_value);
}

int SettingsDialog::getEffectiveIntValue(const char* section, const char* key, int default_value) const
{
  int value;
  if (m_sif && m_sif->GetIntValue(section, key, &value))
    return value;
  else
    return Host::GetBaseIntSettingValue(section, key, default_value);
}

float SettingsDialog::getEffectiveFloatValue(const char* section, const char* key, float default_value) const
{
  float value;
  if (m_sif && m_sif->GetFloatValue(section, key, &value))
    return value;
  else
    return Host::GetBaseFloatSettingValue(section, key, default_value);
}

std::string SettingsDialog::getEffectiveStringValue(const char* section, const char* key,
                                                    const char* default_value) const
{
  std::string value;
  if (!m_sif || !m_sif->GetStringValue(section, key, &value))
    value = Host::GetBaseStringSettingValue(section, key, default_value);
  return value;
}

std::optional<bool> SettingsDialog::getBoolValue(const char* section, const char* key,
                                                 std::optional<bool> default_value) const
{
  std::optional<bool> value;
  if (m_sif)
  {
    bool bvalue;
    if (m_sif->GetBoolValue(section, key, &bvalue))
      value = bvalue;
    else
      value = default_value;
  }
  else
  {
    value = Host::GetBaseBoolSettingValue(section, key, default_value.value_or(false));
  }

  return value;
}

std::optional<int> SettingsDialog::getIntValue(const char* section, const char* key,
                                               std::optional<int> default_value) const
{
  std::optional<int> value;
  if (m_sif)
  {
    int ivalue;
    if (m_sif->GetIntValue(section, key, &ivalue))
      value = ivalue;
    else
      value = default_value;
  }
  else
  {
    value = Host::GetBaseIntSettingValue(section, key, default_value.value_or(0));
  }

  return value;
}

std::optional<float> SettingsDialog::getFloatValue(const char* section, const char* key,
                                                   std::optional<float> default_value) const
{
  std::optional<float> value;
  if (m_sif)
  {
    float fvalue;
    if (m_sif->GetFloatValue(section, key, &fvalue))
      value = fvalue;
    else
      value = default_value;
  }
  else
  {
    value = Host::GetBaseFloatSettingValue(section, key, default_value.value_or(0.0f));
  }

  return value;
}

std::optional<std::string> SettingsDialog::getStringValue(const char* section, const char* key,
                                                          std::optional<const char*> default_value) const
{
  std::optional<std::string> value;
  if (m_sif)
  {
    std::string svalue;
    if (m_sif->GetStringValue(section, key, &svalue))
      value = std::move(svalue);
    else if (default_value.has_value())
      value = default_value.value();
  }
  else
  {
    value = Host::GetBaseStringSettingValue(section, key, default_value.value_or(""));
  }

  return value;
}

void SettingsDialog::setBoolSettingValue(const char* section, const char* key, std::optional<bool> value)
{
  if (m_sif)
  {
    value.has_value() ? m_sif->SetBoolValue(section, key, value.value()) : m_sif->DeleteValue(section, key);
    m_sif->Save();
    QtHostInterface::GetInstance()->reloadGameSettings();
  }
  else
  {
    value.has_value() ? QtHost::SetBaseBoolSettingValue(section, key, value.value()) :
                        QtHost::RemoveBaseSettingValue(section, key);
    QtHostInterface::GetInstance()->applySettings();
  }
}

void SettingsDialog::setIntSettingValue(const char* section, const char* key, std::optional<int> value)
{
  if (m_sif)
  {
    value.has_value() ? m_sif->SetIntValue(section, key, value.value()) : m_sif->DeleteValue(section, key);
    m_sif->Save();
    QtHostInterface::GetInstance()->reloadGameSettings();
  }
  else
  {
    value.has_value() ? QtHost::SetBaseIntSettingValue(section, key, value.value()) :
                        QtHost::RemoveBaseSettingValue(section, key);
    QtHostInterface::GetInstance()->applySettings();
  }
}

void SettingsDialog::setFloatSettingValue(const char* section, const char* key, std::optional<float> value)
{
  if (m_sif)
  {
    value.has_value() ? m_sif->SetFloatValue(section, key, value.value()) : m_sif->DeleteValue(section, key);
    m_sif->Save();
    QtHostInterface::GetInstance()->reloadGameSettings();
  }
  else
  {
    value.has_value() ? QtHost::SetBaseFloatSettingValue(section, key, value.value()) :
                        QtHost::RemoveBaseSettingValue(section, key);
    QtHostInterface::GetInstance()->applySettings();
  }
}

void SettingsDialog::setStringSettingValue(const char* section, const char* key, std::optional<const char*> value)
{
  if (m_sif)
  {
    value.has_value() ? m_sif->SetStringValue(section, key, value.value()) : m_sif->DeleteValue(section, key);
    m_sif->Save();
    QtHostInterface::GetInstance()->reloadGameSettings();
  }
  else
  {
    value.has_value() ? QtHost::SetBaseStringSettingValue(section, key, value.value()) :
                        QtHost::RemoveBaseSettingValue(section, key);
    QtHostInterface::GetInstance()->applySettings();
  }
}

void SettingsDialog::removeSettingValue(const char* section, const char* key)
{
  if (m_sif)
  {
    m_sif->DeleteValue(section, key);
    m_sif->Save();
    QtHostInterface::GetInstance()->reloadGameSettings();
  }
  else
  {
    QtHost::RemoveBaseSettingValue(section, key);
    QtHostInterface::GetInstance()->applySettings();
  }
}
