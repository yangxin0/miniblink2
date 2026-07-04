// miniblink2.h — the miniblink2 public C API (mb* ABI): compatibility umbrella.
//
// The API is split by audience:
//   view.h  the interactive-embedder core (lifecycle, loads, paint, input,
//           resource hooks) — safe in a host's frame tick.
//   auto.h  the automation/testing kit (waits, selector driving, shots/PDF,
//           emulation, storage snapshots, request log); several calls pump
//           the engine.
// Including this header gets both, exactly as before the split.

#ifndef MINIBLINK2_MINIBLINK2_H_
#define MINIBLINK2_MINIBLINK2_H_

#include "auto.h"
#include "view.h"

#endif  // MINIBLINK2_MINIBLINK2_H_
