#pragma once

#include <QtWidgets/QWidget>

#include "ui_consolesettingswidget.h"

class SettingsDialog;

class ConsoleSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ConsoleSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~ConsoleSettingsWidget();

Q_SIGNALS:
  void multitapModeChanged();

private Q_SLOTS:
  void onEnableCPUClockSpeedControlChecked(int state);
  void onCPUClockSpeedValueChanged(int value);
  void updateCPUClockSpeedLabel();
  void onCDROMReadSpeedupValueChanged(int value);

private:
  void calculateCPUClockValue();

  Ui::ConsoleSettingsWidget m_ui;

  SettingsDialog* m_dialog;
};
