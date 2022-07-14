#pragma once
#include "core/game_database.h"
#include "frontend-common/game_list.h"
#include "frontend-common/game_settings.h"
#include "ui_gamepropertiesdialog.h"
#include <QtWidgets/QDialog>
#include <QtWidgets/QPushButton>
#include <array>

class GamePropertiesDialog final : public QDialog
{
  Q_OBJECT

public:
  GamePropertiesDialog(QWidget* parent = nullptr);
  ~GamePropertiesDialog();

  static void showForEntry(const GameList::Entry* ge, QWidget* parent);

public Q_SLOTS:
  void clear();
  void populate(const GameList::Entry* ge);

protected:
  void closeEvent(QCloseEvent* ev);
  void resizeEvent(QResizeEvent* ev);

private Q_SLOTS:
  void saveCompatibilityInfo();
  void saveCompatibilityInfoIfChanged();
  void setCompatibilityInfoChanged();

  void onSetVersionTestedToCurrentClicked();
  void onComputeHashClicked();
  void onExportCompatibilityInfoClicked();
  void updateCPUClockSpeedLabel();
  void onEnableCPUClockSpeedControlChecked(int state);

private:
  void setupAdditionalUi();
  void connectUi();
  void populateCompatibilityInfo(const std::string& game_code);
  void populateTracksInfo(const std::string& image_path);
  void populateGameSettings();
  void populateBooleanUserSetting(QCheckBox* cb, const std::optional<bool>& value);
  void connectBooleanUserSetting(QCheckBox* cb, std::optional<bool>* value);
  void saveGameSettings();
  void fillEntryFromUi();
  void computeTrackHashes(std::string& redump_keyword);
  void onResize();
  void onUserAspectRatioChanged();

  Ui::GamePropertiesDialog m_ui;
  std::array<QCheckBox*, static_cast<u32>(GameDatabase::Trait::Count)> m_trait_checkboxes{};
  QPushButton* m_exportCompatibilityInfo;
  QPushButton* m_computeHashes;

  std::string m_path;
  std::string m_game_code;
  std::string m_game_title;
  std::string m_redump_search_keyword;

  // GameSettings::Entry m_game_settings;

  bool m_compatibility_info_changed = false;
};
