#include "hotkeysettingswidget.h"
#include "controllersettingsdialog.h"
#include "frontend-common/input_manager.h"
#include "inputbindingwidgets.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QTabWidget>

HotkeySettingsWidget::HotkeySettingsWidget(QWidget* parent, ControllerSettingsDialog* dialog)
  : QWidget(parent), m_dialog(dialog)
{
  createUi();
}

HotkeySettingsWidget::~HotkeySettingsWidget() = default;

void HotkeySettingsWidget::createUi()
{
  QGridLayout* layout = new QGridLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  m_tab_widget = new QTabWidget(this);

  createButtons();

  layout->addWidget(m_tab_widget, 0, 0, 1, 1);

  setLayout(layout);
}

void HotkeySettingsWidget::createButtons()
{
  const std::vector<const HotkeyInfo*> hotkeys(InputManager::GetHotkeyList());
  for (const HotkeyInfo* hotkey : hotkeys)
  {
    const auto category = qApp->translate("Hotkeys", hotkey->category);

    auto iter = m_categories.find(category);
    if (iter == m_categories.end())
    {
      QScrollArea* scroll = new QScrollArea(m_tab_widget);
      QWidget* container = new QWidget(scroll);
      QVBoxLayout* vlayout = new QVBoxLayout(container);
      QGridLayout* layout = new QGridLayout();
      layout->setContentsMargins(0, 0, 0, 0);
      vlayout->addLayout(layout);
      vlayout->addStretch(1);
      iter = m_categories.insert(category, Category{container, layout});
      scroll->setWidget(container);
      scroll->setWidgetResizable(true);
      scroll->setBackgroundRole(QPalette::Base);
      scroll->setFrameShape(QFrame::NoFrame);
      m_tab_widget->addTab(scroll, category);
    }

    QWidget* container = iter->container;
    QGridLayout* layout = iter->layout;
    const int target_row = layout->count() / 2;

    layout->addWidget(new QLabel(qApp->translate("Hotkeys", hotkey->display_name), container), target_row, 0);
    layout->addWidget(
      new InputBindingWidget(container, m_dialog->getProfileSettingsInterface(), "Hotkeys", hotkey->name), target_row,
      1);
  }
}
