#pragma once

#include <QtWidgets/QWidget>

#include "ui_emulationsettingswidget.h"

class SettingsDialog;

class EmulationSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit EmulationSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~EmulationSettingsWidget();

private Q_SLOTS:
  void onEmulationSpeedIndexChanged(int index);
  void onFastForwardSpeedIndexChanged(int index);
  void onTurboSpeedIndexChanged(int index);
  void updateRewind();

private:
  bool runaheadEnabled() { return m_ui.runaheadFrames->currentIndex() > 0; }

  Ui::EmulationSettingsWidget m_ui;

  SettingsDialog* m_dialog;
};
