#pragma once

/// Liquid Cooling Control: vendor-neutral liquid-cooling control runtime.
///
/// Public API surface.  Everything in this header set is covered by the
/// installed CMake package; internal headers under src/ are not installed.

#include "liquidcooling/adapter.hpp"
#include "liquidcooling/attempt.hpp"
#include "liquidcooling/authority.hpp"
#include "liquidcooling/enum_support.hpp"
#include "liquidcooling/evidence.hpp"
#include "liquidcooling/ids.hpp"
#include "liquidcooling/model.hpp"
#include "liquidcooling/policy.hpp"
#include "liquidcooling/runtime.hpp"
#include "liquidcooling/status.hpp"
#include "liquidcooling/store.hpp"
#include "liquidcooling/synthetic_adapter.hpp"
#include "liquidcooling/time.hpp"
#include "liquidcooling/transition.hpp"
#include "liquidcooling/units.hpp"
#include "liquidcooling/version.hpp"
