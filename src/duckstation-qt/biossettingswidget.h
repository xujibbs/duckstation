#pragma once
#include "core/types.h"
#include <QtWidgets/QWidget>

#include "ui_biossettingswidget.h"

class SettingsDialog;

class BIOSSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit BIOSSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~BIOSSettingsWidget();

private Q_SLOTS:
  void refreshList();

private:
  Ui::BIOSSettingsWidget m_ui;

  SettingsDialog* m_dialog;
};
