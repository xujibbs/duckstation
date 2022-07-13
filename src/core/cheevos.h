#pragma once
#include "common/types.h"

class StateWrapper;

namespace Cheevos {

#ifdef WITH_CHEEVOS

// Implemented in Host.
extern void Reset();
extern bool DoState(StateWrapper& sw);

/// Returns true if features such as save states should be disabled.
extern bool IsChallengeModeActive();

extern void DisplayBlockedByChallengeModeMessage();

#else

// Make noops when compiling without cheevos.
static inline void Reset() {}
static inline bool DoState(StateWrapper& sw)
{
  return true;
}
static constexpr inline bool IsChallengeModeActive()
{
  return false;
}
static inline void DisplayBlockedByChallengeModeMessage() {}

#endif

} // namespace Cheevos
