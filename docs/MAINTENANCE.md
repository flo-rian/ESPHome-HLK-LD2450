# Maintenance notes — ESPHome API compatibility

**Last updated:** 2026-06-11 · **Last verified against:** ESPHome 2026.5.3

This document is the running record of compatibility fixes between
this component (`components/LD2450/`) and the ESPHome core
framework. It is written for two audiences:

- A human maintainer picking the code back up.
- A future LLM/agent asked to fix the next breakage.

The point is to make the next fix cheap: by the time you finish
reading the relevant section, you should know *exactly* which file
to touch, which ESPHome API changed, and where to look in the
ESPHome source for the replacement.

## TL;DR

ESPHome 2026.x removed `unit_of_measurement_` and deprecated
`raw_state` on `sensor::Sensor`, no longer transitively pulls in
`Action<Ts...>` from `automation.h`, and now requires the
keyword-only `synchronous=` argument on `register_action()`. The
component was updated accordingly on branch
`fix/esphome-2026.5-compat` (commits `fc93472`, `f8a88d3`).

## How to use this document

1. Find the latest **Fix:** section below. Read the **Symptoms**
   block — if your compile error matches verbatim, the rest of
   that section is the answer.
2. If it does not match, run the **Reproduction recipe** to get a
   clean local build and capture the new error.
3. Use **Where to look in ESPHome** to locate the new API in the
   installed package.
4. Apply the fix using the same pattern as the previous section,
   then append a new **Fix:** section using **Template for
   documenting a new fix**.

## Environment and conventions

| | |
|---|---|
| ESPHome version this fix targets | **2026.5.3** |
| Component path | `components/LD2450/` |
| Test configs | `tests/base.yaml` (minimal), `tests/full.yaml` (full) |
| Setup script | `scripts/setup-build-env.sh --esphome-version <X.Y.Z>` |
| venv path | `.venv/` (created by the script) |
| PlatformIO cache | `~/.platformio/` |
| ESP32 framework | **esp-idf** (ESPHome default since 2026.1.0) |
| Test board | `esp32dev` |

The two test configs do not pin a framework. The component builds
cleanly under esp-idf, which is now the default; no `framework:`
key is required.

---

## Fix: ESPHome 2026.5.3 API compat

- **Date:** 2026-06-11
- **Branch:** `fix/esphome-2026.5-compat`
- **Commits:** `fc93472` (code), `f8a88d3` (gitignore for deploy key)
- **HA-side trigger:** Component fails to compile inside the HA
  ESPHome add-on, which at the time ships esphome 2026.5.3.

### Symptoms

Verbatim errors from `esphome compile tests/base.yaml`:

1. `polling_sensor.h:18:17: error: 'unit_of_measurement_' was not
   declared in this scope; did you mean 'get_unit_of_measurement'?`
2. `zone.h:191:46: error: expected template-name before '<'
   token` on `class UpdatePolygonAction : public Action<Ts...>`
3. `polling_sensor.h:29: warning: 'esphome::sensor::Sensor::raw_state'
   is deprecated: Use get_raw_state() instead of .raw_state. Will
   be removed in 2026.10.0.`
4. Bootstrap: `WARNING register_action('LD2450.zone.update_polygon',
   ...) is missing the synchronous= parameter. Defaulting to
   synchronous=False`

Items 1, 2, 4 are hard failures. Item 3 is a warning today but
becomes a hard error in 2026.10.0 — fix it now.

### Root cause

1. **`sensor::Sensor::unit_of_measurement_` removed.** The
   `const char*` member is gone. The replacement lives on
   `EntityBase` (the new public base of `Sensor`):
   `get_unit_of_measurement_ref()` returns a non-owning
   `StringRef`; the older `get_unit_of_measurement()` returns
   `std::string` and is itself deprecated, removed in 2026.9.0.
   Authoritative source: `esphome/core/entity_base.h:145-150`.

2. **`sensor::Sensor::raw_state` deprecated.** The public field
   is still present for now but is `ESPDEPRECATED` and will be
   removed in 2026.10.0. The replacement method is
   `get_raw_state()`. The accessor intentionally suppresses the
   deprecation warning internally, so calling it is clean.
   Source: `esphome/components/sensor/sensor.h:100-105`.

3. **`Action<Ts...>` not transitively included.** The action base
   class lives in `esphome/core/automation.h`. Older ESPHome
   pulled this in via `binary_sensor/automation.h` or
   `sensor/automation.h`; those transitive paths no longer
   reach it. The new code must include
   `esphome/core/automation.h` directly to get `Action<Ts...>`,
   `Automation<Ts...>`, and `TEMPLATABLE_VALUE`. Source:
   `esphome/core/automation.h:508` (`Action`), `:475-504`
   (`Automation`, `Trigger`), `:103-110` (`TEMPLATABLE_VALUE`).

4. **`automation.register_action()` requires `synchronous=`.** The
   3-arg form was removed. The new signature is keyword-only:
   `register_action(name, type, schema, *, synchronous)`. Choose
   `synchronous=True` if `play()` returns before the next action
   runs (no callbacks, timers, or `Component::loop()` deferral);
   `synchronous=False` is required for deferred actions so the
   framework can use owning `std::string` for trigger args
   instead of non-owning `StringRef`. Source:
   `esphome/automation.py:66-98`.

### Per-file changes

#### `components/LD2450/polling_sensor.h`

Two private-member reads in the `PollingSensor` template — both
must switch to public accessors.

**Before (lines 15–31):**

```cpp
void setup() override
{
    // Determine unit conversion
    if (unit_of_measurement_ != nullptr)
    {
        if (strcmp(unit_of_measurement_, "m") == 0)
            conversion_factor_ = 0.001f;
        else if ((strcmp(unit_of_measurement_, "cm") == 0))
            conversion_factor_ = 0.1f;
    }
}

void update() override
{
    if (raw_state != value_ && !(std::isnan(raw_state) && std::isnan(value_)))
        publish_state(value_);
}
```

**After:**

```cpp
void setup() override
{
    // Determine unit conversion
    const StringRef unit = this->get_unit_of_measurement_ref();
    if (unit == "m")
        conversion_factor_ = 0.001f;
    else if (unit == "cm")
        conversion_factor_ = 0.1f;
}

void update() override
{
    const float current = this->get_raw_state();
    if (current != value_ && !(std::isnan(current) && std::isnan(value_)))
        publish_state(value_);
}
```

Notes:
- `StringRef` has `operator==` against `const char*` literals, so
  the direct comparison is clean and there is no null check —
  `StringRef` default-constructs to an empty string and never
  compares equal to a non-empty unit.
- For `raw_state`, the result of `get_raw_state()` is cached in
  `current` so the conditional calls it once, not twice. The
  accessor itself suppresses the deprecation warning internally.
- No new include is needed: `StringRef` is in
  `esphome/core/string_ref.h`, transitively included via
  `entity_base.h` → `sensor.h` → the existing `sensor.h` include
  in `polling_sensor.h`.

#### `components/LD2450/zone.h`

Add an explicit include for `automation.h`.

**Before (lines 1–2):**

```cpp
#pragma once
#include <map>
```

**After:**

```cpp
#pragma once
#include <map>
#include "esphome/core/automation.h"
```

The class `UpdatePolygonAction` derives from `Action<Ts...>` and
declares a `TEMPLATABLE_VALUE`. Both come from this header.

#### `components/LD2450/LD2450.cpp` and `components/LD2450/zone.cpp`

Two more `raw_state` reads in the per-target-update loops.

**`LD2450.cpp:348`:**

```diff
-if (target_count_sensor_ != nullptr && target_count_sensor_->raw_state != target_count)
+if (target_count_sensor_ != nullptr && target_count_sensor_->get_raw_state() != target_count)
     target_count_sensor_->publish_state(target_count);
```

**`zone.cpp:88`:**

```diff
-if (target_count_sensor_ != nullptr && (target_count_sensor_->raw_state != target_count))
+if (target_count_sensor_ != nullptr && (target_count_sensor_->get_raw_state() != target_count))
     target_count_sensor_->publish_state(target_count);
```

#### `components/LD2450/__init__.py`

Pass `synchronous=True` to `register_action`.

**Before (lines 721–730):**

```python
@automation.register_action(
    "LD2450.zone.update_polygon",
    UpdatePolygonAction,
    cv.All(
        {
            cv.Required(CONF_ID): cv.use_id(Zone),
            cv.Required(CONF_POLYGON): cv.templatable(cv.ensure_list(Point)),
        }
    ),
)
```

**After:**

```python
@automation.register_action(
    "LD2450.zone.update_polygon",
    UpdatePolygonAction,
    cv.All(
        {
            cv.Required(CONF_ID): cv.use_id(Zone),
            cv.Required(CONF_POLYGON): cv.templatable(cv.ensure_list(Point)),
        }
    ),
    synchronous=True,
)
```

`UpdatePolygonAction::play()` calls
`parent_->update_polygon(polygon)` synchronously and returns; the
next action can run immediately, so `synchronous=True` is correct.

### Verification

```bash
source .venv/bin/activate
esphome compile tests/base.yaml   # 0 errors, 0 deprecation warnings
esphome compile tests/full.yaml   # 0 errors, 0 deprecation warnings
```

Resulting binaries:

| Config | Flash | RAM |
|---|---|---|
| `tests/base.yaml` | 220 631 B (12.0%) | 22 332 B (6.8%) |
| `tests/full.yaml` | 230 003 B (12.5%) | 24 796 B (7.6%) |

The absence of the `register_action` warning in the second build
is the proof that `synchronous=True` was the right pick.

### Reproduction recipe

```bash
git clone <repo-url>
cd ESPHome-HLK-LD2450
./scripts/setup-build-env.sh --esphome-version 2026.5.3
source .venv/bin/activate
esphome compile tests/base.yaml
# Should succeed. To see the original error, revert the changes
# from this fix (git revert fc93472) and re-run.
```

To probe a newer ESPHome release:

```bash
./scripts/setup-build-env.sh --esphome-version <newer>
esphome compile tests/base.yaml
esphome compile tests/full.yaml
```

---

## Where to look in ESPHome when this breaks again

The authoritative source for the new API is the installed package
in the venv. Example path on this host:

```
.venv/lib/python3.13/site-packages/esphome/
```

Start at these files for the common breakages this component hits:

| Concern | File | Symbols |
|---|---|---|
| Action base | `core/automation.h` | `Action<Ts...>`, `Automation<Ts...>`, `Trigger<Ts...>`, `TEMPLATABLE_VALUE` |
| Entity metadata | `core/entity_base.h` | `get_unit_of_measurement_ref()`, `get_device_class_to()`, `get_icon_to()` |
| Sensor state | `components/sensor/sensor.h` | `Sensor::state`, `Sensor::get_raw_state()`, `Sensor::publish_state()` |
| Binary sensor | `components/binary_sensor/binary_sensor.h` | `BinarySensor::publish_state()` |
| Python registry | `automation.py` | `register_action`, `register_condition`, `Trigger`, `Action`, `Condition` |
| Number base | `components/number/number.h` | `Number::publish_state()` |

Useful greps:

```bash
# Find anything recently deprecated in the public API
grep -rn 'ESPDEPRECATED' .venv/lib/python3.13/site-packages/esphome/core/

# Find new accessors next to a removed field
grep -rn 'get_raw_state' .venv/lib/python3.13/site-packages/esphome/

# Find all uses of the new register_action signature
grep -rn 'synchronous=' .venv/lib/python3.13/site-packages/esphome/automation.py
```

To diff ESPHome versions, clone https://github.com/esphome/esphome
and `git log` the files above between two tags.

## Common breakage patterns → likely cause

| Symptom | Likely cause | First place to look |
|---|---|---|
| `'X' was not declared in this scope; did you mean 'Y'?` | Private member removed; new public accessor is the suggestion | `core/entity_base.h`, `components/*/X.h` |
| `expected template-name before '<' token` on a base class | Transitive include no longer reaches the base header | Add the explicit include (`automation.h`, `helpers.h`, `component.h`, ...) |
| `'X' is deprecated: ... Will be removed in YYYY.X.0` | Public field is on its way out | Switch to the `get_X()` accessor now |
| `missing the X parameter` on a Python decorator | New keyword-only arg added to the registry function | `esphome/automation.py`, `esphome/codegen.py` |
| `the default framework for ESP32 is ESP-IDF` | ESPHome 2026.1.0+ default flipped | Either pin `framework: { type: arduino }` in YAML or accept esp-idf |
| Linker errors for `esphome::X` symbols | `X` was moved to a different header / namespace | `git log` on the file in the esphome source |

## Template for documenting a new fix

Append a new top-level section using this shape:

```markdown
## Fix: ESPHome <X.Y.Z> API compat

- **Date:** <YYYY-MM-DD>
- **Branch:** `<branch-name>`
- **Commits:** `<sha>` (code), `<sha>` (other)
- **HA-side trigger:** <one sentence>

### Symptoms

<verbatim error output>

### Root cause

<one paragraph per error: which API changed, where the new
API lives in the installed esphome package, link to the
relevant file:symbol>

### Per-file changes

<one subsection per file with before/after code blocks and
a one-line reason for each change>

### Verification

<commands run, with success criteria and binary sizes>
```

---

## Glossary

- **`Action<Ts...>`** — Base class for things you do in response
  to a trigger. The `Ts...` is the trigger argument tuple (empty
  for a button press, `(float, float)` for an `x,y` sensor pair,
  etc.). Subclass and override `play(const Ts&...)`.
- **`Automation<Ts...>`** — A chain of actions run in response to
  a trigger. Auto-generates the `play_complex` machinery. You
  usually use this via the `Action` base, not directly.
- **`Trigger<Ts...>`** — Base class for things that fire
  actions: button presses, sensor thresholds, etc.
- **`TEMPLATABLE_VALUE(type, name)`** — Macro that declares a
  field of an action class which can be either a literal or a
  lambda. The Python `cg.templatable()` call sets it; without a
  corresponding `cg.templatable()` you get a static-assert error
  at compile time.
- **`StringRef`** — ESPHome's `std::string_view`-like type for
  non-owning references to strings owned by something else.
  Defined in `esphome/core/string_ref.h`. Has `operator==` with
  `const char*` and `std::string` for convenient comparison.
- **`register_action(name, type, schema, *, synchronous)`** —
  Python decorator that wires a C++ action class into ESPHome's
  YAML action system. The `synchronous` flag tells the framework
  whether the action can use non-owning `StringRef` for trigger
  args (`True`, default-false-safe but slower) or needs to
  capture them by value (`False`, required if the action
  defers via callback/timer/`loop()`).
- **`get_unit_of_measurement_ref()`** — Returns `StringRef` to the
  entity's configured unit. Empty (not null) when no unit is set.
  Use this in preference to the deprecated `get_unit_of_measurement()`.
- **`get_raw_state()`** — Returns the raw, unfiltered value of a
  sensor. Replaces direct reads of the deprecated `raw_state`
  field.
