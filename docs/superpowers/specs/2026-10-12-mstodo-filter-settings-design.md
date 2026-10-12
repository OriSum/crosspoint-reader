# MS To Do Filter Settings — Design Spec

**Date:** 2026-10-12
**Project:** CrossPoint Reader (Xteink X4, ESP32-C3)
**Scope:** Two user-facing settings for the shipped MS To Do integration: hide overdue tasks, and choose how many days ahead to fetch.
**Status:** Draft v1 — extends `docs/superpowers/specs/2026-10-10-mstodo-integration-design.md`.
**User guide:** `docs/mstodo-integration.md`
**Plan:** `docs/superpowers/plans/2026-10-12-mstodo-filter-settings-plan.md`

---

## 1. Summary

The shipped MS To Do integration renders a fixed "Planned" view: overdue + today + the next seven days, across every list. Two knobs were traced as future work in the original spec (§13) and the user guide:

- **Show overdue** — hide overdue tasks when the list has accumulated stale items that are no longer actionable.
- **Future days** — choose how many days ahead the fetch window reaches.

This spec promotes both from future work to shipped features. Both are exposed as rows in the existing **Settings → MS To Do** submenu (matching the on-device settings pattern used elsewhere in the firmware), persisted in `/.crosspoint/mstodo.json`, and survive sign-out.

The change is deliberately narrow: it adds two config fields, threads them into the existing server-side `$filter`, and adds two settings rows. No new module, no new dependency, no change to the auth, snapshot, or sleep-screen paths.

## 2. Goals

- Let the user hide overdue tasks from the activity and the sleep snapshot.
- Let the user choose the forward window (1–30 days) used by the fetch.
- Expose both as on-device settings rows, with atomic write-through to `mstodo.json`.
- Keep the defaults identical to today's behavior (`show_overdue = true`, `future_days = 7`), so an existing install is unaffected until the user changes a setting.
- Keep the fetch bounded: the 64-task cap, the 32-list cap, the 4-pages-per-list cap, and the 24 KB response buffer are unchanged.

## 3. Non-goals

- A free-form filter string or a date-format setting.
- Per-list filtering (the Planned view still spans every list).
- Changing the today/overdue split logic (`isOverdue()` is unchanged).
- Changing the snapshot format, the sleep-screen pre-check, or the auth flow.
- A "hide future tasks" toggle (the forward window already covers this: `future_days` bounds it).
- Plural-form handling beyond English singular/plural (other languages fall back to English per project convention).

## 4. User-facing behavior

### 4.1 Settings rows

**Settings → MS To Do** grows from two rows to four:

| # | Row | Type | Values |
|---|---|---|---|
| 0 | MS To Do sleep screen | checkbox | On / Off |
| 1 | Show overdue | checkbox | On / Off |
| 2 | Future days | cycler | 1, 3, 7, 14, 30 |
| 3 | Sign out | action | — |

- **Show overdue** toggles `show_overdue`. When off, overdue tasks are excluded from the fetch (server-side), so they never reach the activity or the snapshot.
- **Future days** cycles `future_days` through the ladder `1 → 3 → 7 → 14 → 30 → 1`. The value is the inclusive upper bound of the fetch window, measured from today.
- Both rows write through to `mstodo.json` immediately (atomic temp-file-and-rename, same as the existing toggle).
- Both settings **survive sign-out**, like the sleep-screen toggle: signing out clears the session and the snapshot, not the user's display preferences.

### 4.2 Config schema

`/.crosspoint/mstodo.json` gains two fields:

```json
{
  "client_id": "optional-guid-override",
  "access_token": "string",
  "refresh_token": "string",
  "token_expiry_epoch": 1760000000,
  "sleep_screen_enabled": false,
  "show_overdue": true,
  "future_days": 7
}
```

| Field | Type | Default | Allowed values |
|---|---|---|---|
| `show_overdue` | bool | `true` | `true` / `false` |
| `future_days` | number | `7` | 1–30 (clamped on load and on set) |

- Missing fields fall back to the defaults above, so an existing `mstodo.json` written by the shipped firmware loads unchanged.
- `future_days` is **clamped**, not rejected: a hand-edited `0`, `-5`, or `999` becomes `1`, `1`, or `30` respectively. A bad value must never break the fetch.
- Unknown keys are still ignored on load and dropped on the next persist (existing behavior).

### 4.3 Behavior

**Show overdue off.** The fetch's lower date bound moves from `1970-01-01` to today. Overdue tasks are therefore never requested, never parsed, and never counted against the 64-task budget. The activity and the snapshot show only today + the forward window. The `!` overdue glyph can no longer appear.

**Future days.** The fetch's upper date bound moves from `today + 7` to `today + future_days`. A larger window returns more tasks; a smaller window returns fewer. The 64-task cap still applies, so a very large window on a busy account is truncated at 64 tasks rather than growing memory.

**Interaction.** The two settings are independent. `show_overdue = false` with `future_days = 30` fetches today through +30 days, excluding overdue.

## 5. Architecture

### 5.1 Files touched

```
src/integrations/mstodo/MsTodoConfig.{h,cpp}          +2 fields, +2 getters, +2 setters, clamp helper
src/integrations/mstodo/MsTodoClient.{h,cpp}          +FetchOptions struct; $filter rebuilt from options
src/activities/integrations/MsTodoActivity.cpp        build FetchOptions from config at the call site
src/activities/settings/MsTodoSettingsActivity.{h,cpp} +2 rows (kItemCount 2 → 4)
lib/I18n/translations/english.yaml                    +3 keys
docs/mstodo-integration.md                            user guide updated
```

No new files. No new dependency. No change to `SleepActivity`, `MsTodoAuth`, `MsTodoTask`, `main.cpp`, or `SettingsActivity` (the submenu is already wired).

### 5.2 Data flow

```
Settings row (Confirm)
  → MSTODO_CONFIG.setShowOverdue(bool) / setFutureDays(int)
      → clamp (future_days only)
      → value-change check
      → persist()  (atomic tmp + rename)

Activity fetch
  → MSTODO_CONFIG.isShowOverdue() / getFutureDays()
  → MsTodoClient::FetchOptions{showOverdue, futureDays}
  → fetchPlanned(token, options, tasks)
      → lower bound = showOverdue ? "1970-01-01" : today
      → upper bound = today + futureDays
      → percent-encoded $filter
```

The client stays stateless: it takes explicit options and never reads `MsTodoConfig`. The activity is the only place that bridges config to client, matching the existing design.

### 5.3 Client interface

```cpp
struct FetchOptions {
  bool showOverdue = true;
  int futureDays = 7;
};

static FetchResult fetchPlanned(const std::string& accessToken, const FetchOptions& options,
                                std::vector<MsTodoTask>& outTasks);
```

A struct rather than two extra parameters: the call site reads as `FetchOptions{cfg.isShowOverdue(), cfg.getFutureDays()}`, and a future option (e.g. a list picker) extends the struct without another signature change.

## 6. Filter semantics

The `$filter` value is rebuilt per fetch from the options:

```
$filter=status ne 'completed'
        and dueDateTime/dateTime ge '<lower>'
        and dueDateTime/dateTime le '<upper>'
```

| Option | `<lower>` | `<upper>` |
|---|---|---|
| `show_overdue = true` (default) | `1970-01-01` | `today + future_days` |
| `show_overdue = false` | `today` | `today + future_days` |

- `status ne 'completed'` is unchanged.
- The lower bound still excludes undated tasks: a null comparison (`eq null`) is rejected with HTTP 400, so a date literal is used instead. `ge '<today>'` excludes undated tasks for the same reason `ge '1970-01-01'` does.
- The upper bound is inclusive, computed on-device from the local clock via the existing `formatDatePlusDays()` helper.
- The value is percent-encoded with the existing `urlEncode()` helper (the raw value contains spaces and quotes that `http_parser` rejects).

**Unset clock fails open.** `formatDatePlusDays()` writes `9999-12-31` when the clock is not set. With `show_overdue = false`, a `today` lower bound would then equal the upper bound and collapse the window to nothing. The client therefore keeps the `1970-01-01` lower bound whenever the computed "today" is the sentinel — a bad clock shows too much rather than nothing. (In practice the activity syncs the clock before every fetch, so this is a belt-and-braces path.)

**Known limitation (pre-existing, unchanged).** The activity pins `TZ=UTC` for the fetch path, so "today" is UTC. A task due in the first hours of the local day in a positive-offset timezone can be classified as overdue. This already affects `isOverdue()` and the existing upper bound; the new lower bound is consistent with it. Not addressed here.

**Why server-side rather than client-side filtering.** Filtering overdue tasks out after the fetch would still spend the 64-task budget on tasks the user never sees, and would still pay for their bytes in the 24 KB response buffer. Moving the lower bound server-side keeps the budget and the buffer spent on visible tasks, and matches the integration's existing "filter on the server, split on the device" philosophy.

## 7. Memory and bounds impact

| Bound | Before | After |
|---|---|---|
| Response buffer | 24 KB | 24 KB (unchanged) |
| Task cap | 64 | 64 (unchanged) |
| Lists walked | 32 | 32 (unchanged) |
| Pages per list | 4 | 4 (unchanged) |
| `std::vector<MsTodoTask>` reservation | 64 | 64 (unchanged) |

The original future-work note claimed `future_days` "directly affects the response size budget and the vector reservation cap". That is not accurate: both are hard caps that do not scale with the window. What a larger window changes is the **likelihood of hitting** the 64-task cap — a busy account with `future_days = 30` may truncate at 64 tasks. That is bounded and acceptable, and it is the same truncation the shipped firmware already applies to a busy 7-day window.

No new allocation. `FetchOptions` is a two-field POD passed by const reference; it lives on the caller's stack.

## 8. i18n keys

Four additions to `lib/I18n/translations/english.yaml`:

| Key | English |
|---|---|
| `STR_MSTODO_SHOW_OVERDUE` | "Show overdue" |
| `STR_MSTODO_FUTURE_DAYS` | "Future days" |
| `STR_MSTODO_DAY_SINGULAR` | "%d day" |
| `STR_MSTODO_DAY_PLURAL` | "%d days" |

The row label is `STR_MSTODO_FUTURE_DAYS`; the value text is `STR_MSTODO_DAY_SINGULAR` when the count is 1 and `STR_MSTODO_DAY_PLURAL` otherwise. Other languages fall back to English per project convention. Run `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/` after editing the YAML.

## 9. Error handling

| Scenario | Behavior |
|---|---|
| `future_days` out of range in the file | Clamped to 1–30 on load; no error screen |
| `show_overdue` missing | Defaults to `true` (shipped behavior) |
| Persist fails on a settings change | Setter returns false; in-memory value stays consistent with the file (same contract as the existing toggle) |
| `show_overdue = false` and no tasks in the window | Existing `STR_MSTODO_NO_TASKS` empty state; snapshot still captured |
| `future_days = 1` and nothing due today or tomorrow | Empty state; not an error |

No new error paths. The fetch's existing failure handling (auth abort, per-list skip, truncation tolerance) is unchanged.

## 10. Testing

### 10.1 Build-time (verifiable by AI)

- `pio run -e default`, `-e gh_release`, `-e slim` — 0 errors, 0 new warnings.
- `./bin/clang-format-fix -g` — idempotent.
- `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/` — generated headers in sync.
- Manual review: no hardcoded UI strings; all logging via `LOG_*` tag `"MSTD"`; all file I/O via `Storage`; no token values in logs.

### 10.2 Hardware checklist (user runs)

**Settings:**
- [ ] Settings → MS To Do shows four rows in the documented order.
- [ ] Toggle "Show overdue" off → power-cycle → still off (atomic write survived).
- [ ] Cycle "Future days" through 1 → 3 → 7 → 14 → 30 → 1; value text reads "1 day" then "3 days".
- [ ] Sign out → both new settings are kept; next open re-runs sign-in.

**Behavior:**
- [ ] `show_overdue = true` (default) → overdue tasks render with the `!` glyph (unchanged from shipped behavior).
- [ ] `show_overdue = false` → no `!` glyph; overdue tasks absent from the list and from the sleep snapshot.
- [ ] `future_days = 1` → only today's tasks (plus tomorrow's, per the inclusive bound) render.
- [ ] `future_days = 30` → tasks up to +30 days render; a busy account truncates at 64 tasks without a crash.
- [ ] Hand-edit `future_days` to `0` and to `999` on the SD card → clamped to 1 and 30; fetch still succeeds.
- [ ] Hand-edit `show_overdue` to a non-boolean → falls back to `true`.

**Regression:**
- [ ] Existing `mstodo.json` (no new fields) loads and behaves exactly as before.
- [ ] Sleep snapshot reflects the current settings after a refresh.
- [ ] Heap pre-entry vs post-exit within ~2 KB of baseline; open/close 20× with no monotonic leak.

## 11. Rollback

| Level | Action | Reversibility |
|---|---|---|
| Soft | Settings → "Show overdue" on, "Future days" 7 | Instant; returns to shipped behavior |
| Config | Delete the two keys from `mstodo.json` | Defaults apply on next load |
| Code-level | `git revert` the feature commits; rebuild & flash | One build cycle |
| Upstream firmware | Flash upstream `gh_release` | No SPIFFS schema divergence |

## 12. Decisions log

| # | Decision | Rationale |
|---|---|---|
| 1 | On-device settings rows, not SD-only | Matches the firmware's existing settings pattern and the referenced fork's `TasksSettingsActivity`; the SD file stays the persistence layer, not the only interface |
| 2 | Defaults preserve shipped behavior | An existing install must not change until the user opts in |
| 3 | `future_days` clamped, not rejected | A hand-edited file must never break the fetch; clamping is the least surprising recovery |
| 4 | Overdue filtered server-side | Keeps the 64-task budget and the 24 KB buffer spent on visible tasks; matches the existing filter philosophy |
| 5 | `FetchOptions` struct, not extra parameters | Readable call site; extensible without another signature change |
| 6 | Cycler ladder 1/3/7/14/30, config range 1–30 | Common values on-device; SD editors keep the full range |
| 7 | Both settings survive sign-out | Display preferences are not session state; same rule as the sleep-screen toggle |
| 8 | No change to the 64-task / 24 KB caps | A larger window changes the likelihood of truncation, not the memory ceiling |
| 9 | Singular/plural via two keys | English-only plural handling; other languages fall back to English per convention |
