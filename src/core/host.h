#pragma once

#include "common/types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Generic input bindings. These roughly match a DualShock 4 or XBox One controller.
/// They are used for automatic binding to PS2 controller types, and for big picture mode navigation.
enum class GenericInputBinding : u8
{
  Unknown,

  DPadUp,
  DPadRight,
  DPadLeft,
  DPadDown,

  LeftStickUp,
  LeftStickRight,
  LeftStickDown,
  LeftStickLeft,
  L3,

  RightStickUp,
  RightStickRight,
  RightStickDown,
  RightStickLeft,
  R3,

  Triangle, // Y on XBox pads.
  Circle,   // B on XBox pads.
  Cross,    // A on XBox pads.
  Square,   // X on XBox pads.

  Select, // Share on DS4, View on XBox pads.
  Start,  // Options on DS4, Menu on XBox pads.
  System, // PS button on DS4, Guide button on XBox pads.

  L1, // LB on Xbox pads.
  L2, // Left trigger on XBox pads.
  R1, // RB on XBox pads.
  R2, // Right trigger on Xbox pads.

  SmallMotor, // High frequency vibration.
  LargeMotor, // Low frequency vibration.

  Count,
};

namespace Host {
/// Reads a file from the resources directory of the application.
/// This may be outside of the "normal" filesystem on platforms such as Mac.
std::optional<std::vector<u8>> ReadResourceFile(const char* filename);

/// Reads a resource file file from the resources directory as a string.
std::optional<std::string> ReadResourceFileToString(const char* filename);

/// Adds OSD messages, duration is in seconds.
void AddOSDMessage(std::string message, float duration = 2.0f);
void AddKeyedOSDMessage(std::string key, std::string message, float duration = 2.0f);
void AddFormattedOSDMessage(float duration, const char* format, ...);
void AddKeyedFormattedOSDMessage(std::string key, float duration, const char* format, ...);
void RemoveKeyedOSDMessage(std::string key);
void ClearOSDMessages();

/// Displays an asynchronous error on the UI thread, i.e. doesn't block the caller.
void ReportErrorAsync(const std::string_view& title, const std::string_view& message);
void ReportFormattedErrorAsync(const std::string_view& title, const char* format, ...);

/// Internal method used by pads to dispatch vibration updates to input sources.
/// Intensity is normalized from 0 to 1.
void SetPadVibrationIntensity(u32 pad_index, float large_or_single_motor_intensity, float small_motor_intensity);

void OpenBackgroundProgressDialog(const char* str_id, std::string message, s32 min, s32 max, s32 value);
void UpdateBackgroundProgressDialog(const char* str_id, std::string message, s32 min, s32 max, s32 value);
void CloseBackgroundProgressDialog(const char* str_id);

void AddNotification(float duration, std::string title, std::string text, std::string image_path);
void ShowToast(std::string title, std::string message, float duration = 10.0f);
} // namespace Host
