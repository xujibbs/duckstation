#pragma once

#include <QtCore/QMap>
#include <QtWidgets/QWidget>
#include <array>
#include <vector>

class QTabWidget;
class QGridLayout;

class ControllerSettingsDialog;

class HotkeySettingsWidget : public QWidget
{
  Q_OBJECT

public:
  HotkeySettingsWidget(QWidget* parent, ControllerSettingsDialog* dialog);
  ~HotkeySettingsWidget();

private:
  void createUi();
  void createButtons();

  ControllerSettingsDialog* m_dialog;
  QTabWidget* m_tab_widget;

  struct Category
  {
    QWidget* container;
    QGridLayout* layout;
  };
  QMap<QString, Category> m_categories;
};
