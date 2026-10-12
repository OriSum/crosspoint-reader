# MS To Do Filter Settings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two user-facing settings to the shipped MS To Do integration — hide overdue tasks, and choose the forward fetch window (1–30 days) — exposed as rows in Settings → MS To Do and persisted in `/.crosspoint/mstodo.json`.

**Architecture:** Two fields on the existing `MsTodoConfig` singleton (`showOverdue`, `futureDays`), a two-field `FetchOptions` POD passed into `MsTodoClient::fetchPlanned`, and two new rows in the existing `MsTodoSettingsActivity`. The `$filter` date bounds are rebuilt per fetch from the options. No new module, no new dependency, no change to auth, snapshot, or sleep-screen paths.

**Tech Stack:** ESP32-C3 (ESP-IDF + Arduino), C++20 no-RTTI no-exceptions, ArduinoJson 7.4.2, `esp_http_client`, existing UITheme/HalStorage abstractions.

**Spec:** `docs/superpowers/specs/2026-10-12-mstodo-filter-settings-design.md`
**User guide:** `docs/mstodo-integration.md`
**Base branch:** `develop-mstodo` (the integration branch; all MS To Do phases are merged there).

**PR strategy:** one PR. The change is small and self-contained; splitting it would leave a half-wired settings screen. The PR builds clean on `default`, `gh_release`, and `slim`, passes `./bin/clang-format-fix -g`, and is independently revertable.

**Invariants enforced:**
- All user-facing strings via `tr(STR_MSTODO_*)`. No hardcoded UI strings.
- All logging via `LOG_DBG`/`LOG_ERR` with module tag `"MSTD"`.
- All file I/O via `Storage` (HalStorage). Never use SdFat directly.
- No SPIFFS schema changes. No additions to `CrossPointSettings`.
- No new heap allocation. `FetchOptions` is a stack POD; the task vector reservation is unchanged.
- Tokens never logged, not even truncated.

**Verification model (firmware project — no unit test harness):** Each task ends in a `pio run` build. Hardware smoke tests are run manually on the Xteink X4 and reported by the user. Build environments verified at the end: `default`, `gh_release`, `slim`.

---

## File Structure

**Modified files (6):**
```
src/integrations/mstodo/MsTodoConfig.h                 +2 fields, +2 getters, +2 setters
src/integrations/mstodo/MsTodoConfig.cpp               load/persist/clamp
src/integrations/mstodo/MsTodoClient.h                 +FetchOptions struct, signature change
src/integrations/mstodo/MsTodoClient.cpp               $filter rebuilt from options
src/activities/integrations/MsTodoActivity.cpp         build FetchOptions at the call site
src/activities/settings/MsTodoSettingsActivity.{h,cpp} kItemCount 2 → 4, +2 rows
lib/I18n/translations/english.yaml                     +4 keys
docs/mstodo-integration.md                             user guide updated
```

**New files:** none.

---

## Task 1 — Config fields

**Goal:** `MsTodoConfig` owns `showOverdue` and `futureDays`, with clamping and atomic persistence.

- [ ] 1.1 In `MsTodoConfig.h`, add to the public surface:
  ```cpp
  // Toggle: include overdue tasks in the fetch. Default true (shipped behavior).
  bool isShowOverdue() const { return showOverdue; }

  // Forward window in days, inclusive of today. Clamped to [kMinFutureDays,
  // kMaxFutureDays] on load and on set.
  int getFutureDays() const { return futureDays; }

  bool setShowOverdue(bool enabled);
  bool setFutureDays(int days);
  ```
  Add the constants next to the class (or in the anonymous namespace of the `.cpp` if they are only needed there — but the settings activity needs the ladder, so expose `kMinFutureDays`/`kMaxFutureDays` as `static constexpr int` members):
  ```cpp
  static constexpr int kMinFutureDays = 1;
  static constexpr int kMaxFutureDays = 30;
  ```
  Add the members:
  ```cpp
  bool showOverdue = true;
  int futureDays = 7;
  ```
  Update the class comment: the config now also carries the two display filters.

- [ ] 1.2 In `MsTodoConfig.cpp`:
  - `load()`: reset both fields to defaults at the top (alongside the existing resets), then
    ```cpp
    showOverdue = doc["show_overdue"] | true;
    futureDays = doc["future_days"] | 7;
    if (futureDays < kMinFutureDays) futureDays = kMinFutureDays;
    if (futureDays > kMaxFutureDays) futureDays = kMaxFutureDays;
    ```
    Extend the existing `LOG_DBG` line with `show_overdue=%d, future_days=%d`.
  - `persist()`: add `doc["show_overdue"] = showOverdue;` and `doc["future_days"] = futureDays;`.
  - Add the two setters, following the existing value-change-guard pattern:
    ```cpp
    bool MsTodoConfig::setShowOverdue(bool enabled) {
      if (enabled == showOverdue) return true;
      showOverdue = enabled;
      return persist();
    }

    bool MsTodoConfig::setFutureDays(int days) {
      if (days < kMinFutureDays) days = kMinFutureDays;
      if (days > kMaxFutureDays) days = kMaxFutureDays;
      if (days == futureDays) return true;
      futureDays = days;
      return persist();
    }
    ```
  - `signOut()`: **do not** clear either field. Add a one-line comment next to the existing `sleepScreenEnabled` comment noting that all three display preferences are kept.
  - `forget()`: reset both to defaults (`showOverdue = true; futureDays = 7;`) alongside the existing resets.

- [ ] 1.3 Build: `pio run -e default`. `./bin/clang-format-fix -g`.

**Deliverable:** config round-trips both fields; a file without them loads with the shipped defaults.

---

## Task 2 — Client options

**Goal:** `fetchPlanned` takes explicit options and rebuilds the `$filter` bounds from them.

- [ ] 2.1 In `MsTodoClient.h`, add above the class:
  ```cpp
  // Fetch window options, supplied by the caller (the activity reads them from
  // MsTodoConfig). The client stays stateless and never reads config itself.
  struct FetchOptions {
    bool showOverdue = true;
    int futureDays = 7;
  };
  ```
  Change the signature:
  ```cpp
  static FetchResult fetchPlanned(const std::string& accessToken, const FetchOptions& options,
                                  std::vector<MsTodoTask>& outTasks);
  ```
  Update the doc comment: the window is `overdue (optional) + today + next options.futureDays days`.

- [ ] 2.2 In `MsTodoClient.cpp`:
  - Remove the `kTaskWindowDays` constant (its role moves into `FetchOptions::futureDays`). Keep `kMaxTasks`, `kMaxLists`, `kMaxListPages`, `kTasksPageSize`, `kMaxPagesPerList`, `kMaxResponseBytes` unchanged.
  - Change the definition to `FetchResult MsTodoClient::fetchPlanned(const std::string& accessToken, const FetchOptions& options, std::vector<MsTodoTask>& outTasks)`.
  - Replace the filter construction:
    ```cpp
    // Lower bound: overdue included (a very old literal, since a null
    // comparison is rejected) or today only. Upper bound: today + futureDays.
    // formatDatePlusDays() writes "9999-12-31" when the clock is unset, so a
    // "today" lower bound would collapse the window to nothing on an unset
    // clock. Fail open instead: keep the very old literal when the clock is
    // not trustworthy, so a bad clock shows too much rather than nothing.
    char lowerBound[11];
    if (options.showOverdue) {
      snprintf(lowerBound, sizeof(lowerBound), "1970-01-01");
    } else {
      formatDatePlusDays(0, lowerBound, sizeof(lowerBound));
      if (strcmp(lowerBound, "9999-12-31") == 0) {
        snprintf(lowerBound, sizeof(lowerBound), "1970-01-01");
      }
    }
    char upperBound[11];
    formatDatePlusDays(options.futureDays, upperBound, sizeof(upperBound));

    std::string filter = "status ne 'completed' and dueDateTime/dateTime ge '";
    filter += lowerBound;
    filter += "' and dueDateTime/dateTime le '";
    filter += upperBound;
    filter += "'";
    ```
    Keep the existing `urlEncode(filter)` call and the comment explaining why the value must be encoded.
  - Update the comment block above the filter to describe both bounds and the `show_overdue` switch.
  - Extend the final `LOG_DBG` with the window (`show_overdue=%d, future_days=%d`) so a serial log shows which window was fetched.

- [ ] 2.3 Build: `pio run -e default`. `./bin/clang-format-fix -g`.

**Deliverable:** the client compiles with the new signature; the filter reflects the options.

---

## Task 3 — Activity call site

**Goal:** the activity reads the two settings and passes them to the client.

- [ ] 3.1 In `MsTodoActivity.cpp`, in `proceedWithFetch()`, replace the fetch call:
  ```cpp
  mstodo::FetchOptions options;
  options.showOverdue = MSTODO_CONFIG.isShowOverdue();
  options.futureDays = MSTODO_CONFIG.getFutureDays();
  FetchResult r = mstodo::MsTodoClient::fetchPlanned(MSTODO_CONFIG.getAccessToken(), options, _tasks);
  ```
  No other change: the sort, the today/overdue split, the snapshot capture, and the error mapping are untouched. With `show_overdue = false` the client simply returns no overdue tasks, so the existing `!` glyph path is never reached.

- [ ] 3.2 Build: `pio run -e default`. `./bin/clang-format-fix -g`.

**Deliverable:** the activity honors both settings end to end.

---

## Task 4 — Settings rows

**Goal:** Settings → MS To Do exposes both settings.

- [ ] 4.1 In `MsTodoSettingsActivity.h`:
  - `kItemCount` 2 → 4.
  - Update the class comment: rows are sleep-screen toggle, show-overdue toggle, future-days cycler, sign out.
  - No new members: the value text is produced by a file-local helper in the `.cpp` (see 4.2), so the header only changes the count and the comment.
- [ ] 4.2 In `MsTodoSettingsActivity.cpp`:
  - Extend `rowNames` to four entries in order: `STR_MSTODO_SLEEP_SCREEN`, `STR_MSTODO_SHOW_OVERDUE`, `STR_MSTODO_FUTURE_DAYS`, `STR_MSTODO_SIGNOUT`.
  - `activateIndex()`:
    ```cpp
    case 1:
      MSTODO_CONFIG.setShowOverdue(!MSTODO_CONFIG.isShowOverdue());
      return;
    case 2: {
      // Cycle the ladder; wrap 30 → 1.
      static constexpr int kLadder[] = {1, 3, 7, 14, 30};
      const int cur = MSTODO_CONFIG.getFutureDays();
      int next = kLadder[0];
      for (size_t i = 0; i < sizeof(kLadder) / sizeof(kLadder[0]); ++i) {
        if (kLadder[i] == cur) {
          next = kLadder[(i + 1) % (sizeof(kLadder) / sizeof(kLadder[0]))];
          break;
        }
      }
      MSTODO_CONFIG.setFutureDays(next);
      return;
    }
    case 3:
      MSTODO_CONFIG.signOut();
      return;
    ```
    Note: a hand-edited value outside the ladder (e.g. 5) falls to `kLadder[0]` = 1 on the first press, which is a sane recovery.
  - `buildScreen()`: extend the value switch:
    ```cpp
    case 1:
      rowValues_[i] = std::string(MSTODO_CONFIG.isShowOverdue() ? "On" : "Off");
      break;
    case 2:
      formatFutureDays(MSTODO_CONFIG.getFutureDays(), rowValues_[i]);
      break;
    case 3:
      rowValues_[i].clear();
      break;
    ```
    Add `GUI.setCheckboxRow(rowItems_[1], MSTODO_CONFIG.isShowOverdue());` next to the existing row-0 call. Row 2 is a value cycler (no checkbox); row 3 is an action (no value).
  - Keep the existing `props` setup unchanged — the list already handles four rows.
  - Add a file-local helper in the anonymous namespace (next to `rowNames`):
    ```cpp
    // "1 day" / "N days" for the future-days row value.
    void formatFutureDays(int days, std::string& out) {
      char buf[24];
      snprintf(buf, sizeof(buf), I18N.get(days == 1 ? StrId::STR_MSTODO_DAY_SINGULAR : StrId::STR_MSTODO_DAY_PLURAL),
               days);
      out = buf;
    }
    ```
    `I18N.get()` returns a `const char*` format string; `snprintf` fills the `%d`. Assigning into the existing `rowValues_[i]` string reuses its capacity (no per-render allocation), matching the existing pattern.

- [ ] 4.3 Add the i18n keys to `lib/I18n/translations/english.yaml` after the existing `STR_MSTODO_*` block:
  ```yaml
  STR_MSTODO_SHOW_OVERDUE: "Show overdue"
  STR_MSTODO_FUTURE_DAYS: "Future days"
  STR_MSTODO_DAY_SINGULAR: "%d day"
  STR_MSTODO_DAY_PLURAL: "%d days"
  ```
  Run `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`.

- [ ] 4.4 Build: `pio run -e default`, `-e gh_release`, `-e slim`. `./bin/clang-format-fix -g`.

- [ ] 4.5 Hardware smoke test: four rows render in order; both toggles and the cycler persist across a power-cycle; sign-out keeps all three display preferences.

**Deliverable:** the settings screen exposes both features.

---

## Task 5 — Documentation

**Goal:** the user guide describes the shipped behavior.

- [ ] 5.1 In `docs/mstodo-integration.md`:
  - §2 field reference: add `show_overdue` and `future_days` rows; update the "Full config" JSON example.
  - §5 settings table: add the two rows; note that all three display preferences survive sign-out.
  - "What the activity shows": describe the configurable window and the overdue toggle.
  - "Why the requests look the way they do": update the `$filter` block to show both bounds and the `show_overdue` switch.
  - Remove the two "Future work" bullets (they are now shipped).
- [ ] 5.2 Verify every path, row label, and serial tag in the guide matches the code.
- [ ] 5.3 Build: `pio run -e default` (docs-only change, but confirm the tree still builds).

**Deliverable:** the guide matches the firmware.

---

## Phase-end verification

- `pio run -t clean && pio run -e default` — 0 errors, 0 new warnings.
- `pio run -e gh_release && pio run -e slim` — pass.
- `./bin/clang-format-fix -g` — idempotent.
- `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/` — generated headers in sync.
- Manual review: no hardcoded UI strings; all logging via `LOG_*` tag `"MSTD"`; all file I/O via `Storage`; no token values in logs; no new heap allocation.

## Hardware checklist (before the PR merges)

See spec §10.2. Minimum gate: four settings rows render and persist; `show_overdue = false` removes the `!` glyph from the list and the snapshot; `future_days` 1 and 30 both fetch successfully; a hand-edited out-of-range value is clamped; an existing `mstodo.json` without the new fields behaves exactly as before; heap stable across 20 open/close cycles.
