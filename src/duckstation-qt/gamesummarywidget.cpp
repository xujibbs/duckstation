#include "common/string_util.h"

#include "frontend-common/game_list.h"

#include "gamesummarywidget.h"
#include "qthost.h"
#include "settingsdialog.h"

GameSummaryWidget::GameSummaryWidget(const std::string& path, const std::string& serial, DiscRegion region,
                                     const GameDatabase::Entry* entry, SettingsDialog* dialog, QWidget* parent)
  : m_dialog(dialog)
{
  m_ui.setupUi(this);

#if 0
  const QString base_path(QtHost::GetResourcesBasePath());
  for (int i = 0; i < m_ui.region->count(); i++)
  {
    m_ui.region->setItemIcon(i, QIcon(
                    QStringLiteral("%1/icons/flags/%2.png").arg(base_path).arg(GameList::RegionToString(static_cast<GameList::Region>(i)))));
  }
  for (int i = 1; i < m_ui.compatibility->count(); i++)
  {
    m_ui.compatibility->setItemIcon(i, QIcon(
                         QStringLiteral("%1/icons/star-%2.png").arg(base_path).arg(i)));
  }
#endif

  populateUi(path, serial, region, entry);

  // connect(m_ui.inputProfile, &QComboBox::currentIndexChanged, this, &GameSummaryWidget::onInputProfileChanged);
}

GameSummaryWidget::~GameSummaryWidget() = default;

void GameSummaryWidget::populateUi(const std::string& path, const std::string& serial, DiscRegion region,
                                   const GameDatabase::Entry* entry)
{
  m_ui.path->setText(QString::fromStdString(path));
  m_ui.serial->setText(QString::fromStdString(serial));
  m_ui.region->setCurrentIndex(static_cast<int>(region));

  if (entry)
  {
    m_ui.title->setText(QString::fromStdString(entry->title));
    m_ui.compatibility->setCurrentIndex(static_cast<int>(entry->compatibility));
  }

#if 0
  for (const std::string& name : PAD::GetInputProfileNames())
    m_ui.inputProfile->addItem(QString::fromStdString(name));

  std::optional<std::string> profile(m_dialog->getStringValue("EmuCore", "InputProfileName", std::nullopt));
  if (profile.has_value())
    m_ui.inputProfile->setCurrentIndex(m_ui.inputProfile->findText(QString::fromStdString(profile.value())));
  else
    m_ui.inputProfile->setCurrentIndex(0);
#endif
}

void GameSummaryWidget::onInputProfileChanged(int index)
{
#if 0
  if (index == 0)
    m_dialog->setStringSettingValue("EmuCore", "InputProfileName", std::nullopt);
  else
    m_dialog->setStringSettingValue("EmuCore", "InputProfileName", m_ui.inputProfile->itemText(index).toUtf8());
#endif
}
