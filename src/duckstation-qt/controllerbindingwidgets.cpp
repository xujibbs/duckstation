#include "controllerbindingwidgets.h"
#include "common/log.h"
#include "common/string_util.h"
#include "controllersettingsdialog.h"
#include "controllersettingwidgetbinder.h"
#include "core/host_settings.h"
#include "frontend-common/input_manager.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <algorithm>

Log_SetChannel(ControllerBindingWidget);

ControllerBindingWidget::ControllerBindingWidget(QWidget* parent, ControllerSettingsDialog* dialog, u32 port)
  : QWidget(parent), m_dialog(dialog), m_config_section(StringUtil::StdStringFromFormat("Pad%u", port + 1u)),
    m_port_number(port)
{
  m_ui.setupUi(this);
  populateControllerTypes();
  onTypeChanged();

  connect(m_ui.controllerType, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &ControllerBindingWidget::onTypeChanged);
  connect(m_ui.automaticBinding, &QPushButton::clicked, this, &ControllerBindingWidget::doAutomaticBinding);
  connect(m_ui.clearBindings, &QPushButton::clicked, this, &ControllerBindingWidget::doClearBindings);
}

ControllerBindingWidget::~ControllerBindingWidget() = default;

QIcon ControllerBindingWidget::getIcon() const
{
  return m_current_widget->getIcon();
}

void ControllerBindingWidget::populateControllerTypes()
{
  for (u32 i = 0; i < static_cast<u32>(ControllerType::Count); i++)
  {
    const ControllerType ctype = static_cast<ControllerType>(i);
    const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(ctype);
    if (!cinfo)
      continue;

    m_ui.controllerType->addItem(qApp->translate("ControllerType", cinfo->display_name), QVariant(static_cast<int>(i)));
  }

  const std::string controller_type_name(
    m_dialog->getStringValue(m_config_section.c_str(), "Type", Controller::GetDefaultPadType(m_port_number)));
  m_controller_type = Settings::ParseControllerTypeName(controller_type_name.c_str()).value_or(ControllerType::None);

  const int index = m_ui.controllerType->findData(QVariant(static_cast<int>(m_controller_type)));
  if (index >= 0 && index != m_ui.controllerType->currentIndex())
  {
    QSignalBlocker sb(m_ui.controllerType);
    m_ui.controllerType->setCurrentIndex(index);
  }
}

void ControllerBindingWidget::populateBindingWidget()
{
  const bool is_initializing = (m_current_widget == nullptr);
  if (!is_initializing)
  {
    m_ui.verticalLayout->removeWidget(m_current_widget);
    delete m_current_widget;
    m_current_widget = nullptr;
  }

  if (m_controller_type == ControllerType::DigitalController)
    m_current_widget = ControllerBindingWidget_DigitalController::createInstance(this);
  else if (m_controller_type == ControllerType::AnalogController)
    m_current_widget = ControllerBindingWidget_AnalogController::createInstance(this);
  else
    m_current_widget = new ControllerBindingWidget_Base(this);

  m_ui.verticalLayout->addWidget(m_current_widget, 1);

  // no need to do this on first init, only changes
  if (!is_initializing)
    m_dialog->updateListDescription(m_port_number, this);
}

void ControllerBindingWidget::onTypeChanged()
{
  bool ok;
  const int index = m_ui.controllerType->currentData().toInt(&ok);
  if (!ok || index < 0 || index >= static_cast<int>(ControllerType::Count))
    return;

  m_controller_type = static_cast<ControllerType>(index);

  SettingsInterface* sif = m_dialog->getProfileSettingsInterface();
  if (sif)
    sif->SetStringValue(m_config_section.c_str(), "Type", Settings::GetControllerTypeName(m_controller_type));
  else
    Host::SetBaseStringSettingValue(m_config_section.c_str(), "Type", Settings::GetControllerTypeName(m_controller_type));

  // TODO: reloadInputProfile() ?
  QtHostInterface::GetInstance()->applySettings();

  populateBindingWidget();
}

void ControllerBindingWidget::doAutomaticBinding()
{
  QMenu menu(this);
  bool added = false;

  for (const QPair<QString, QString>& dev : m_dialog->getDeviceList())
  {
    // we set it as data, because the device list could get invalidated while the menu is up
    QAction* action = menu.addAction(QStringLiteral("%1 (%2)").arg(dev.first).arg(dev.second));
    action->setData(dev.first);
    connect(action, &QAction::triggered, this,
            [this, action]() { doDeviceAutomaticBinding(action->data().toString()); });
    added = true;
  }

  if (!added)
  {
    QAction* action = menu.addAction(tr("No devices available"));
    action->setEnabled(false);
  }

  menu.exec(QCursor::pos());
}

void ControllerBindingWidget::doClearBindings()
{
  if (QMessageBox::question(
        QtUtils::GetRootWidget(this), tr("Clear Bindings"),
        tr("Are you sure you want to clear all bindings for this controller? This action cannot be undone.")) !=
      QMessageBox::Yes)
  {
    return;
  }

  if (m_dialog->isEditingGlobalSettings())
  {
    auto lock = Host::GetSettingsLock();
    InputManager::ClearPortBindings(*Host::Internal::GetBaseSettingsLayer(), m_port_number);
  }
  else
  {
    InputManager::ClearPortBindings(*m_dialog->getProfileSettingsInterface(), m_port_number);
  }

  saveAndRefresh();
}

void ControllerBindingWidget::doDeviceAutomaticBinding(const QString& device)
{
  std::vector<std::pair<GenericInputBinding, std::string>> mapping =
    InputManager::GetGenericBindingMapping(device.toStdString());
  if (mapping.empty())
  {
    QMessageBox::critical(QtUtils::GetRootWidget(this), tr("Automatic Binding"),
                          tr("No generic bindings were generated for device '%1'").arg(device));
    return;
  }

  bool result;
  if (m_dialog->isEditingGlobalSettings())
  {
    auto lock = Host::GetSettingsLock();
    result = InputManager::MapController(*Host::Internal::GetBaseSettingsLayer(), m_port_number, mapping);
  }
  else
  {
    result = InputManager::MapController(*m_dialog->getProfileSettingsInterface(), m_port_number, mapping);
    m_dialog->getProfileSettingsInterface()->Save();
    QtHostInterface::GetInstance()->reloadInputBindings();
  }

  // force a refresh after mapping
  if (result)
    saveAndRefresh();
}

void ControllerBindingWidget::saveAndRefresh()
{
  onTypeChanged();
  QtHost::QueueSettingsSave();
  QtHostInterface::GetInstance()->applySettings();
}

//////////////////////////////////////////////////////////////////////////

ControllerBindingWidget_Base::ControllerBindingWidget_Base(ControllerBindingWidget* parent) : QWidget(parent) {}

ControllerBindingWidget_Base::~ControllerBindingWidget_Base() {}

QIcon ControllerBindingWidget_Base::getIcon() const
{
  return QIcon::fromTheme("artboard-2-line");
}

void ControllerBindingWidget_Base::initBindingWidgets()
{
  SettingsInterface* sif = getDialog()->getProfileSettingsInterface();
  const ControllerType type = getControllerType();
  const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(type);
  if (!cinfo)
    return;

  const std::string& config_section = getConfigSection();
  for (u32 i = 0; i < cinfo->num_bindings; i++)
  {
    const Controller::ControllerBindingInfo& bi = cinfo->bindings[i];
    if (bi.type == Controller::ControllerBindingType::Unknown || bi.type == Controller::ControllerBindingType::Motor)
      continue;

    InputBindingWidget* widget = findChild<InputBindingWidget*>(QString::fromUtf8(bi.name));
    if (!widget)
    {
      Log_ErrorPrintf("No widget found for '%s' (%s)", bi.name, cinfo->name);
      continue;
    }

    widget->initialize(sif, config_section, bi.name);
  }

  switch (cinfo->vibration_caps)
  {
    case Controller::VibrationCapabilities::LargeSmallMotors:
    {
      InputVibrationBindingWidget* widget = findChild<InputVibrationBindingWidget*>(QStringLiteral("LargeMotor"));
      if (widget)
        widget->setKey(getDialog(), config_section, "LargeMotor");

      widget = findChild<InputVibrationBindingWidget*>(QStringLiteral("SmallMotor"));
      if (widget)
        widget->setKey(getDialog(), config_section, "SmallMotor");
    }
    break;

    case Controller::VibrationCapabilities::SingleMotor:
    {
      InputVibrationBindingWidget* widget = findChild<InputVibrationBindingWidget*>(QStringLiteral("Motor"));
      if (widget)
        widget->setKey(getDialog(), config_section, "Motor");
    }
    break;

    case Controller::VibrationCapabilities::NoVibration:
    default:
      break;
  }

  if (QSlider* widget = findChild<QSlider*>(QStringLiteral("Deadzone")); widget)
  {
    const float range = static_cast<float>(widget->maximum());
    QLabel* label = findChild<QLabel*>(QStringLiteral("DeadzoneLabel"));
    if (label)
    {
      connect(widget, &QSlider::valueChanged, this, [range, label](int value) {
        label->setText(tr("%1%").arg((static_cast<float>(value) / range) * 100.0f, 0, 'f', 0));
      });
    }

    ControllerSettingWidgetBinder::BindWidgetToInputProfileNormalized(sif, widget, config_section, "Deadzone", range,
                                                                      Controller::DEFAULT_STICK_DEADZONE);
  }

  if (QSlider* widget = findChild<QSlider*>(QStringLiteral("AxisScale")); widget)
  {
    // position 1.0f at the halfway point
    const float range = static_cast<float>(widget->maximum()) * 0.5f;
    QLabel* label = findChild<QLabel*>(QStringLiteral("AxisScaleLabel"));
    if (label)
    {
      connect(widget, &QSlider::valueChanged, this, [range, label](int value) {
        label->setText(tr("%1%").arg((static_cast<float>(value) / range) * 100.0f, 0, 'f', 0));
      });
    }

    ControllerSettingWidgetBinder::BindWidgetToInputProfileNormalized(sif, widget, config_section, "AxisScale", range,
                                                                      Controller::DEFAULT_STICK_SCALE);
  }

  if (QDoubleSpinBox* widget = findChild<QDoubleSpinBox*>(QStringLiteral("SmallMotorScale")); widget)
    ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, widget, config_section, "SmallMotorScale",
                                                                 Controller::DEFAULT_MOTOR_SCALE);
  if (QDoubleSpinBox* widget = findChild<QDoubleSpinBox*>(QStringLiteral("LargeMotorScale")); widget)
    ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, widget, config_section, "LargeMotorScale",
                                                                 Controller::DEFAULT_MOTOR_SCALE);
}

//////////////////////////////////////////////////////////////////////////

ControllerBindingWidget_DigitalController::ControllerBindingWidget_DigitalController(ControllerBindingWidget* parent)
  : ControllerBindingWidget_Base(parent)
{
  m_ui.setupUi(this);
  initBindingWidgets();
}

ControllerBindingWidget_DigitalController::~ControllerBindingWidget_DigitalController() {}

QIcon ControllerBindingWidget_DigitalController::getIcon() const
{
  return QIcon::fromTheme("gamepad-line");
}

ControllerBindingWidget_Base* ControllerBindingWidget_DigitalController::createInstance(ControllerBindingWidget* parent)
{
  return new ControllerBindingWidget_DigitalController(parent);
}

//////////////////////////////////////////////////////////////////////////

ControllerBindingWidget_AnalogController::ControllerBindingWidget_AnalogController(ControllerBindingWidget* parent)
  : ControllerBindingWidget_Base(parent)
{
  m_ui.setupUi(this);
  initBindingWidgets();
}

ControllerBindingWidget_AnalogController::~ControllerBindingWidget_AnalogController() {}

QIcon ControllerBindingWidget_AnalogController::getIcon() const
{
  return QIcon::fromTheme("gamepad-line");
}

ControllerBindingWidget_Base* ControllerBindingWidget_AnalogController::createInstance(ControllerBindingWidget* parent)
{
  return new ControllerBindingWidget_AnalogController(parent);
}

//////////////////////////////////////////////////////////////////////////
