# Todoist Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a read-only Todoist "Today" view to the CrossPoint Reader, with optional sleep-screen integration backed by an SD-card-only configuration.

**Architecture:** All Todoist state lives on the SD card under `/.crosspoint/`. A `TodoistConfig` singleton owns load/save with atomic temp-file-and-rename writes. A stateless `TodoistClient` performs HTTPS GETs against `api.todoist.com` using `esp_crt_bundle_attach` (the existing Mozilla CA bundle pattern from `KOReaderSyncClient`). A `TodoistActivity` brings WiFi up, fetches once on entry, renders, captures a snapshot via `ScreenshotUtil::saveFramebufferAsBmpOriented`, and drops WiFi. `SleepActivity` gains a pre-check that blits the Todoist snapshot when the toggle is on and the snapshot orientation matches the configured snapshot orientation. SPIFFS / `CrossPointSettings` is **not** modified.

**Tech Stack:** ESP32-C3 (ESP-IDF + Arduino), C++20 no-RTTI no-exceptions, ArduinoJson 7.4.2, `esp_http_client` + `esp_crt_bundle_attach`, ESP-IDF SNTP, existing UITheme/HalStorage/HalDisplay abstractions.

**Spec:** `docs/superpowers/specs/2026-05-04-todoist-integration-design.md`

**Revision note (2026-10-09):** The original 5-phase plan included weather,
geolocation, timezone, date-filter, date-format, and design-mode subsystems.
Those were removed as scope creep; the plan below is the reduced 3-phase
version matching the shipped v1.

**Invariants enforced across phases:**
- All user-facing strings via `tr(STR_TODOIST_*)`. No hardcoded UI strings.
- All logging via `LOG_DBG`/`LOG_ERR` with module tag `"TDST"`.
- All UI rendering via the `GUI` macro (UITheme).
- All file I/O via `Storage` (HalStorage). Never use SdFat directly.
- No SPIFFS schema changes. No additions to `CrossPointSettings`.
- Reserve all `std::vector` capacity before `push_back`. No heap growth in render loops.
- `string_view`s never crossed into C APIs without explicit null-termination.

**Verification model (firmware project — no unit test harness):** Each task ends in a `pio run` build. Phase-end smoke tests are run manually on hardware (Xteink X4) and reported by the user. Build environments verified at the end of every phase: `default`, `gh_release`, `slim`.

---

## File Structure

**New files (9 created total across the project):**
```
src/integrations/todoist/TodoistConfig.h          Phase 1
src/integrations/todoist/TodoistConfig.cpp        Phase 1
src/integrations/todoist/TodoistTask.h            Phase 1
src/integrations/todoist/TodoistClient.h          Phase 2
src/integrations/todoist/TodoistClient.cpp        Phase 2
src/activities/integrations/TodoistActivity.h     Phase 2
src/activities/integrations/TodoistActivity.cpp   Phase 2
src/activities/settings/TodoistSettingsActivity.h   Phase 3
src/activities/settings/TodoistSettingsActivity.cpp Phase 3
```

**Files modified:**
```
src/main.cpp                                    Phase 1 (boot-time config load)
lib/I18n/translations/english.yaml              Phase 1 (STR_TODOIST_*)
src/activities/home/HomeActivity.h              Phase 2 (menu entry)
src/activities/home/HomeActivity.cpp            Phase 2 (menu entry + dispatch)
src/activities/ActivityManager.h                Phase 2 (goToTodoist + HomeMenuItem)
src/activities/ActivityManager.cpp              Phase 2 (goToTodoist + goHome mapping)
src/activities/boot_sleep/SleepActivity.cpp     Phase 3 (pre-check)
src/activities/boot_sleep/SleepActivity.h       Phase 3 (pre-check declaration)
src/activities/settings/SettingsActivity.h      Phase 3 (Todoist submenu entry)
src/activities/settings/SettingsActivity.cpp    Phase 3 (Todoist submenu entry)
src/components/themes/BaseTheme.h               Phase 2 (UIIcon::Checklist)
src/components/themes/lyra/LyraTheme.cpp        Phase 2 (Checklist icon mapping)
src/components/icons/checklist.h                Phase 2 (icon bitmap)
src/util/ScreenshotUtil.{h,cpp}                 Phase 2 (orientation-aware BMP save)
```

**Phase file budgets:**
- Phase 1: 5 files (3 created, 2 modified)
- Phase 2: 12 files (5 created, 7 modified)
- Phase 3: 6 files (2 created, 4 modified)

---

## Phase 1 — Config Foundation

**Scope:** `TodoistConfig` singleton, `TodoistTask` POD, i18n string additions, boot-time load. No network, no rendering, no UI yet.

**Why first:** Every other phase depends on `TodoistConfig` reads, `TodoistTask` shape, and translated strings. We land this phase on its own and verify a clean build before touching network code.

### Task 1.1: Add i18n strings

**Files:**
- Modify: `lib/I18n/translations/english.yaml`

- [ ] **Step 1: Append Todoist strings to English YAML**

Add the following block (flat top-level mapping, near the other setting strings):

```yaml
STR_TODOIST: "Todoist"
STR_TODOIST_FETCHING: "Fetching tasks..."
STR_TODOIST_NO_TOKEN: "No Todoist token configured"
STR_TODOIST_OFFLINE: "WiFi unavailable"
STR_TODOIST_FETCH_FAILED: "Could not fetch tasks"
STR_TODOIST_NO_TASKS: "All clear today"
STR_TODOIST_SLEEP_SCREEN: "Todoist sleep screen"
STR_TODOIST_ACTIVITY_ORIENTATION: "Activity orientation"
STR_TODOIST_SNAPSHOT_ORIENTATION: "Sleep screen orientation"
STR_TODOIST_FORGET: "Forget Todoist"
STR_TODOIST_TODAY_HEADER: "Updated %02d:%02d"
STR_TODOIST_INVALID_TOKEN: "Invalid token"
STR_TODOIST_RATE_LIMITED: "Rate limited - try later"
```

- [ ] **Step 2: Regenerate i18n headers**

Run: `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`

Expected: command exits 0; files `lib/I18n/I18nKeys.h`, `lib/I18n/I18nStrings.h`, `lib/I18n/I18nStrings.cpp` updated. These files are gitignored — they will not be committed but must exist for the build.

- [ ] **Step 3: Verify build**

Run: `pio run`

Expected: PASS, 0 errors.

- [ ] **Step 4: Commit**

```bash
git add lib/I18n/translations/english.yaml
git commit -m "feat: add Todoist i18n strings"
```

---

### Task 1.2: Create `TodoistTask` POD

**Files:**
- Create: `src/integrations/todoist/TodoistTask.h`

- [ ] **Step 1: Create header**

Fixed-size POD so a `std::vector<TodoistTask>` reserves one contiguous block with no per-task heap allocation:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace todoist {

struct TodoistTask {
  static constexpr size_t kTitleCapacity = 96;
  static constexpr size_t kDueTimeCapacity = 6;   // "HH:MM\0"
  static constexpr size_t kDueDateCapacity = 11;  // "YYYY-MM-DD\0"

  char title[kTitleCapacity];
  char dueDate[kDueDateCapacity];  // empty string if no due date
  char dueTime[kDueTimeCapacity];  // empty string if no time
  uint8_t priority;                // 1 (lowest) to 4 (highest), 0 = unknown
  bool overdue;
};

}  // namespace todoist
```

- [ ] **Step 2: Commit**

```bash
git add src/integrations/todoist/TodoistTask.h
git commit -m "feat: add TodoistTask plain-data struct"
```

---

### Task 1.3: Create `TodoistConfig` singleton

**Files:**
- Create: `src/integrations/todoist/TodoistConfig.h`, `src/integrations/todoist/TodoistConfig.cpp`

- [ ] **Step 1: Create header**

Config carries exactly four fields — API token, sleep toggle, activity orientation, snapshot orientation. No weather, location, timezone, filter, date-format, or design-mode state.

- [ ] **Step 2: Create implementation**

`load()` resets to defaults, bounds the read at 4 KB, parses with ArduinoJson, and falls back per-field. `persist()` serializes to `todoist.json.tmp`, closes, then renames over `todoist.json` (atomic write). Setters compare against the current value before writing (SD write throttling). `forget()` deletes `todoist.json`, `todoist_sleep.bmp`, `todoist_sleep.meta`, and any orphaned `.tmp` files, then resets in-memory state.

- [ ] **Step 3: Verify build and commit**

```bash
pio run
git add src/integrations/todoist/TodoistConfig.*
git commit -m "feat: add TodoistConfig singleton with atomic SD persistence"
```

---

### Task 1.4: Boot-time config load

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Load config in `setup()`**

After `SETTINGS.loadFromFile()`, add `TODOIST_CONFIG.load();` with the include. Config load must never block boot: a missing or corrupt file just leaves the integration disabled.

- [ ] **Step 2: Verify build and commit**

```bash
pio run
git add src/main.cpp
git commit -m "feat: load TodoistConfig at boot"
```

---

## Phase 2 — API Client + Activity

**Scope:** `TodoistClient` (fixed "today + overdue" query), `TodoistActivity` (WiFi bring-up, fetch, render, scroll, refresh, snapshot capture), home-screen menu entry, checklist icon.

### Task 2.1: Create `TodoistClient`

**Files:**
- Create: `src/integrations/todoist/TodoistClient.h`, `src/integrations/todoist/TodoistClient.cpp`

- [ ] **Step 1: Create header**

`FetchResult` enum: `Ok`, `InvalidToken` (401/403), `RateLimited` (429), `NetworkError`, `ServerError`, `ParseError`. Single static `fetch(apiToken, outTasks)`.

- [ ] **Step 2: Create implementation**

- HTTPS GET `https://api.todoist.com/api/v1/tasks/filter?query=` + url-encoded fixed query `(today) | (overdue & due after: -7 days)`.
- `Authorization: Bearer <token>`, `Accept: application/json`.
- `crt_bundle_attach = esp_crt_bundle_attach` (Mozilla CA bundle; Todoist uses Let's Encrypt).
- Response buffer: single `malloc` capped at 16 KB, allocated lazily in the HTTP event handler **after** the TLS handshake (mbedTLS needs ~32 KB contiguous during handshake; pre-allocating fragments the heap on the C3).
- Parse `{ "results": [...] }`, cap at 64 tasks, extract title/dueDate/dueTime/priority/overdue.
- Map HTTP status to `FetchResult`.

- [ ] **Step 3: Verify build and commit**

```bash
pio run
git add src/integrations/todoist/TodoistClient.*
git commit -m "feat: implement TodoistClient fetch over HTTPS"
```

---

### Task 2.2: Create `TodoistActivity`

**Files:**
- Create: `src/activities/integrations/TodoistActivity.h`, `src/activities/integrations/TodoistActivity.cpp`

- [ ] **Step 1: Create header**

States: `Loading`, `ShowingTasks`, `ShowingError`. Single render path `renderTaskList(drawHints)` — no design-mode dispatch.

- [ ] **Step 2: Create implementation**

- `onEnter`: save entry orientation, apply configured activity orientation, paint Loading, `startFetch()`.
- `startFetch`: token check → WiFi (launch `WifiSelectionActivity` if down) → `proceedWithFetch()`.
- `proceedWithFetch`: pin TZ to UTC, NTP sync with build-date fallback for TLS, `TodoistClient::fetch`, sort chronologically, capture hour/min + today's date, `captureSnapshotIfNeeded()`.
- `renderTaskList`: header ("Todoist" + `Updated HH:MM` stamp), bullet list rows (marker glyph, wrapped title, dd/mm date suffix), scroll bar, button hints. In snapshot mode (`drawHints=false`) the hint bar is omitted and the battery region is painted over.
- `drawTaskRows`: variable row height (1–2 wrapped lines), light-grey rounded cursor band (suppressed in snapshot mode), scroll bar when content overflows.
- `captureSnapshotIfNeeded`: render in snapshot orientation (render-twice trick when it differs from the activity orientation), save BMP via `ScreenshotUtil::saveFramebufferAsBmpOriented`, write orientation+time meta sidecar.
- `onExit`: stop SNTP, WiFi off, restore entry orientation.
- `loop`: Back = exit, Confirm = refresh, Up/Down = cursor with sticky scroll window.

- [ ] **Step 3: Verify build and commit**

```bash
pio run
git add src/activities/integrations/TodoistActivity.*
git commit -m "feat: render Todoist task list with fetch and scroll"
```

---

### Task 2.3: Home-screen menu entry

**Files:**
- Modify: `src/activities/home/HomeActivity.{h,cpp}`, `src/activities/ActivityManager.{h,cpp}`, `src/components/themes/BaseTheme.h`, `src/components/themes/lyra/LyraTheme.cpp`, `src/components/icons/checklist.h` (new), `src/util/ScreenshotUtil.{h,cpp}`

- [ ] **Step 1: Add `HomeMenuItem::TODOIST`** to the enum (last item) and the `menuItemToIndex`/`indexToMenuItem` converters in `HomeActivity.h`.

- [ ] **Step 2: Add menu item + icon** in `HomeActivity::render()` (`tr(STR_TODOIST)`, `UIIcon::Checklist`), dispatch in `loop()`, `onTodoistOpen()` → `activityManager.goToTodoist()`.

- [ ] **Step 3: Add `goToTodoist()`** to `ActivityManager` and the `"Todoist"` mapping in `goHome()`.

- [ ] **Step 4: Add `UIIcon::Checklist`** to the `BaseTheme.h` enum, map it in `LyraTheme::iconForName()`, and add the `checklist.h` bitmap.

- [ ] **Step 5: Add `ScreenshotUtil::saveFramebufferAsBmpOriented`** — writes a BMP whose dimensions match the logical screen for the given orientation by inverting `rotateCoordinates()`.

- [ ] **Step 6: Verify build and commit**

```bash
pio run
git add -A src/activities src/components src/util
git commit -m "feat: add Todoist entry to the home screen menu"
```

---

## Phase 3 — Sleep Integration + Settings

**Scope:** `SleepActivity` pre-check, Todoist settings submenu (4 rows), SettingsActivity registration.

### Task 3.1: Sleep-screen pre-check

**Files:**
- Modify: `src/activities/boot_sleep/SleepActivity.{h,cpp}`

- [ ] **Step 1: Add `tryRenderTodoistSleepScreen()`**

Called at the top of `SleepActivity::onEnter()`'s mode switch. Returns early (renders the snapshot) only when all of: toggle on, `todoist_sleep.bmp` exists, `todoist_sleep.meta` exists and parses, meta orientation matches the configured `snapshot_orientation`. Otherwise falls through to the existing sleep modes unchanged. The orientation check prevents rendering a BMP captured in a different orientation than the one now configured.

- [ ] **Step 2: Verify build and commit**

```bash
pio run
git add src/activities/boot_sleep/SleepActivity.*
git commit -m "feat: render Todoist snapshot on sleep when available"
```

---

### Task 3.2: Todoist settings submenu

**Files:**
- Create: `src/activities/settings/TodoistSettingsActivity.{h,cpp}`
- Modify: `src/activities/settings/SettingsActivity.{h,cpp}`

- [ ] **Step 1: Create the submenu**

Extends `UiListActivity` (labels set once in the constructor, `buildScreen()` refreshes live values, `activateIndex()` handles rows). Exactly four rows:

1. **Sleep screen** — checkbox toggling `sleep_screen_enabled`.
2. **Activity orientation** — cycles P → LCW → PI → LCCW.
3. **Sleep screen orientation** — cycles the same set independently.
4. **Forget Todoist** — wipes config + snapshot files, no confirmation.

No city entry, no geocoding, no filter/timezone/date-format/design-mode rows.

- [ ] **Step 2: Register in SettingsActivity**

Add `SettingAction::TodoistSettings` and a System settings row (`STR_TODOIST`) that launches the submenu.

- [ ] **Step 3: Verify build and commit**

```bash
pio run
git add src/activities/settings/
git commit -m "feat: add Todoist settings submenu"
```

---

## Final verification

- [ ] `pio run -t clean && pio run` — 0 errors
- [ ] `pio run -e gh_release && pio run -e slim` — both pass
- [ ] `./bin/clang-format-fix` — clean
- [ ] No weather/geolocation/filter/timezone symbols remain in the tree
- [ ] Hardware smoke test (user): config file → fetch → render → snapshot → sleep → forget
