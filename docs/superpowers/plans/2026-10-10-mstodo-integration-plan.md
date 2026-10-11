# Microsoft To Do Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a read-only Microsoft To Do "Today" view to the CrossPoint Reader, with optional sleep-screen integration backed by an SD-card-only configuration and OAuth 2.0 device-code sign-in.

**Architecture:** All MS To Do state lives on the SD card under `/.crosspoint/`. A `MsTodoConfig` singleton owns load/save with atomic temp-file-and-rename writes. `MsTodoAuth` performs the RFC 8628 device-code flow and token refresh against `login.microsoftonline.com`. A stateless `MsTodoClient` performs HTTPS GETs against `graph.microsoft.com` using `esp_crt_bundle_attach` (the existing Mozilla CA bundle pattern from `TodoistClient`). A `MsTodoActivity` brings WiFi up, signs in or refreshes the token, fetches once, renders, captures a snapshot via `ScreenshotUtil::saveFramebufferAsBmpOriented`, and drops WiFi. `SleepActivity` gains a pre-check that blits the MS To Do snapshot when the toggle is on and the snapshot exists. Everything renders portrait — there are no orientation settings (unlike the Todoist integration). SPIFFS / `CrossPointSettings` is **not** modified.

**Tech Stack:** ESP32-C3 (ESP-IDF + Arduino), C++20 no-RTTI no-exceptions, ArduinoJson 7.4.2, `esp_http_client` + `esp_crt_bundle_attach`, ESP-IDF SNTP, existing UITheme/HalStorage/HalDisplay abstractions.

**Spec:** `docs/superpowers/specs/2026-10-10-mstodo-integration-design.md`
**User guide:** `docs/mstodo-integration.md`

**Base branch:** `mstodo-integration`, cut from `todoist-integration-upgrade`. The Todoist integration is the structural template — every module below has a Todoist sibling to adapt.

**PR strategy:** each phase below is one PR (or two, where noted). Every PR builds clean on `default`, `gh_release`, and `slim`, passes `./bin/clang-format-fix -g`, and is independently revertable. No PR leaves the tree in a state where the new code is reachable from the UI unless its phase is complete.

**Invariants enforced across phases:**
- All user-facing strings via `tr(STR_MSTODO_*)`. No hardcoded UI strings.
- All logging via `LOG_DBG`/`LOG_ERR` with module tag `"MSTD"`.
- All UI rendering via the `GUI` macro (UITheme).
- All file I/O via `Storage` (HalStorage). Never use SdFat directly.
- No SPIFFS schema changes. No additions to `CrossPointSettings`.
- Reserve all `std::vector` capacity before `push_back`. No heap growth in render loops.
- `string_view`s never crossed into C APIs without explicit null-termination.
- Tokens never logged, not even truncated. Log only presence/absence and expiry.

**Verification model (firmware project — no unit test harness):** Each task ends in a `pio run` build. Phase-end smoke tests are run manually on hardware (Xteink X4) and reported by the user. Build environments verified at the end of every phase: `default`, `gh_release`, `slim`.

---

## File Structure

**New files (10 created total across the project):**
```
src/integrations/mstodo/MsTodoConfig.h          Phase 1
src/integrations/mstodo/MsTodoConfig.cpp        Phase 1
src/integrations/mstodo/MsTodoTask.h            Phase 1
src/integrations/mstodo/MsTodoAuth.h            Phase 2
src/integrations/mstodo/MsTodoAuth.cpp          Phase 2
src/integrations/mstodo/MsTodoClient.h          Phase 3
src/integrations/mstodo/MsTodoClient.cpp        Phase 3
src/activities/integrations/MsTodoActivity.h    Phase 4
src/activities/integrations/MsTodoActivity.cpp  Phase 4
src/activities/settings/MsTodoSettingsActivity.{h,cpp}  Phase 5
```

**Modified files (6):**
```
lib/I18n/translations/english.yaml              Phases 1-5 (incremental)
src/main.cpp                                    Phase 1
src/activities/home/HomeActivity.{h,cpp}        Phase 4
src/activities/settings/SettingsActivity.{h,cpp} Phase 5
src/activities/boot_sleep/SleepActivity.cpp     Phase 6
docs/mstodo-integration.md                      Phase 7
```

---

## Phase 1 — Config foundation (PR 1)

**Goal:** `MsTodoConfig` singleton + `MsTodoTask` POD + boot-time load. No network, no UI.

- [ ] 1.1 Create `src/integrations/mstodo/MsTodoTask.h` by adapting `TodoistTask.h`: same fixed-size POD (`char title[96]`, `char dueDate[11]`, `char dueTime[6]`, `uint8_t priority`, `bool overdue`). No changes beyond the namespace (`mstodo`) and header guard.
- [ ] 1.2 Create `src/integrations/mstodo/MsTodoConfig.h` by adapting `TodoistConfig.h`. Differences from Todoist:
  - Config path `/.crosspoint/mstodo.json`, tmp `/.crosspoint/mstodo.json.tmp`, snapshot `/.crosspoint/mstodo_sleep.bmp`, meta `/.crosspoint/mstodo_sleep.meta` (+ `.tmp`).
  - Additional fields: `std::string accessToken`, `std::string refreshToken`, `long tokenExpiryEpoch`, `std::string listId`, `std::string clientId` (empty = compiled-in default).
  - Additional getters/setters: `getAccessToken()`, `getRefreshToken()`, `getTokenExpiryEpoch()`, `getListId()`, `getClientId()` (returns override or `kDefaultClientId`), `setTokens(access, refresh, expiry)`, `setListId()`, `hasTokens()`, `isAccessTokenExpired()` (expiry − 60 s safety margin vs `time(nullptr)`).
  - `signOut()`: clears token fields + `listId`, deletes snapshot BMP/meta/tmp, **keeps** `sleepScreenEnabled`, persists.
  - No orientation fields: unlike `TodoistConfig`, there are no `activityOrientation`/`snapshotOrientation` members, getters, or setters — the activity and snapshot are always portrait.
  - `forget()`: full reset — deletes the config file itself and resets everything to defaults (parity with `TodoistConfig::forget()`).
- [ ] 1.3 Create `src/integrations/mstodo/MsTodoConfig.cpp` by adapting `TodoistConfig.cpp`: same bounded 4 KB read, same `deserializeJson` error handling, same atomic persist (tmp → close → remove-old → rename). Drop the orientation string mapping — no orientation fields exist. Add load/persist of the new fields; `token_expiry_epoch` parsed as `long`, missing → 0 (expired).
- [ ] 1.4 Add `#include "integrations/mstodo/MsTodoConfig.h"` and `MSTODO_CONFIG.load();` next to the existing `TODOIST_CONFIG.load();` in `src/main.cpp` (line ~515).
- [ ] 1.5 Add the first i18n keys to `lib/I18n/translations/english.yaml` (only the ones Phase 1 needs: `STR_MSTODO`, `STR_MSTODO_NO_TOKEN`). Run `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`.
- [ ] 1.6 Build: `pio run`. Environments: `default`, `gh_release`, `slim`. `./bin/clang-format-fix -g`.
- [ ] 1.7 Hardware smoke test: boot with no `mstodo.json` → serial log shows `MSTD: No config at /.crosspoint/mstodo.json`, device boots normally.

**PR 1 contents:** `MsTodoTask.h`, `MsTodoConfig.{h,cpp}`, `main.cpp` (2 lines), `english.yaml` (2 keys). Nothing user-visible yet.

---

## Phase 2 — Auth engine (PR 2)

**Goal:** `MsTodoAuth` — device-code flow and token refresh, testable only via serial logs. No UI yet.

- [ ] 2.1 Create `src/integrations/mstodo/MsTodoAuth.h`. Public surface:
  ```cpp
  enum class AuthResult { Ok, NetworkError, Pending, SlowDown, Expired, Declined, Failed, InvalidGrant };
  struct DeviceCodeInfo { char userCode[16]; const char* verificationUri; int expiresInSeconds; int intervalSeconds; };
  class MsTodoAuth {
   public:
    // Step 1: request a device code. Fills info; returns Ok or NetworkError/Failed.
    static AuthResult requestDeviceCode(const std::string& clientId, DeviceCodeInfo& out);
    // One poll tick of step 2. On Ok, fills outAccess/outRefresh/outExpiresIn.
    static AuthResult pollToken(const std::string& clientId, const std::string& deviceCode,
                                std::string& outAccess, std::string& outRefresh, long& outExpiresIn);
    // Exchange a refresh token. On Ok, fills the three out-params (refresh may be rotated).
    static AuthResult refreshTokens(const std::string& clientId, const std::string& refreshToken,
                                    std::string& outAccess, std::string& outRefresh, long& outExpiresIn);
  };
  ```
- [ ] 2.2 Create `src/integrations/mstodo/MsTodoAuth.cpp` by adapting `TodoistClient.cpp`'s HTTP skeleton: same `esp_http_client_config_t` shape, same 2 KB rx/tx buffers, same `esp_crt_bundle_attach`, same lazy `ResponseBuffer` allocation pattern (token responses are < 2 KB, but keep the pattern for heap-predictability). Differences:
  - POST, not GET; `application/x-www-form-urlencoded` body built with the same `urlEncode` helper (copy it into this TU or a small shared header — do not make `TodoistClient` depend on mstodo).
  - Endpoints: `https://login.microsoftonline.com/common/oauth2/v2.0/devicecode` and `.../v2.0/token`.
  - Scopes: `Tasks.Read offline_access User.Read` (space-separated, URL-encoded as `+`).
  - Parse token JSON: `access_token`, `refresh_token` (may be absent on refresh — keep the old one), `expires_in`.
  - Parse error JSON: `error` field → map `authorization_pending`→`Pending`, `slow_down`→`SlowDown`, `authorization_expired`/`expired_token`→`Expired`, `declined`/`authorization_declined`→`Declined`, `invalid_grant`→`InvalidGrant`, else `Failed`.
  - **Never log token values.** Log lengths only.
- [ ] 2.3 Add the compiled-in default client ID as `constexpr const char* kDefaultClientId` in `MsTodoConfig.cpp`:
  ```cpp
  constexpr const char* kDefaultClientId = "23db5763-8f56-47b7-83ed-83e808e77c71";
  ```
  (Azure app registration: public client, delegated `Tasks.Read` + `offline_access` + `User.Read`, "Allow public client flows" enabled.)
- [ ] 2.4 Build: `pio run` (all three environments). `./bin/clang-format-fix -g`.
- [ ] 2.5 Hardware smoke test (serial-only): temporary test hook in `loop()` of a scratch activity is **not** allowed — instead verify by code review only this phase; the flow is exercised end-to-end in Phase 4.

**PR 2 contents:** `MsTodoAuth.{h,cpp}`, `MsTodoConfig.cpp` (default client id). Nothing user-visible yet.

---

## Phase 3 — Graph client (PR 3)

**Goal:** `MsTodoClient::fetchToday` — list resolution + task fetch + parse. No UI yet.

- [ ] 3.1 Create `src/integrations/mstodo/MsTodoClient.h`:
  ```cpp
  enum class FetchResult { Ok, InvalidToken, RateLimited, ServerError, NetworkError, ParseError, ListGone };
  class MsTodoClient {
   public:
    // Fetches today + overdue tasks from the default list. accessToken must be fresh.
    static FetchResult fetchToday(const std::string& accessToken, std::string& outListId,
                                  std::vector<mstodo::MsTodoTask>& outTasks);
  };
  ```
- [ ] 3.2 Create `src/integrations/mstodo/MsTodoClient.cpp` by adapting `TodoistClient.cpp`:
  - Request 1 (skipped when `outListId` is already non-empty): `GET /v1.0/me/todo/lists` → find `wellknownListName == "defaultList"`, store its `id`. No `$select` — Graph rejects it on To Do for MSA accounts (see spec §7.2).
  - Request 2: `GET /v1.0/me/todo/lists/<id>/tasks?$filter=status ne 'completed' and dueDateTime/dateTime ge '1970-01-01' and dueDateTime/dateTime le '<today+7>'` (percent-encode the `$filter` value — it contains spaces and quotes that `http_parser` rejects in a raw URL). No `$select`; the date bounds replace the old `$top=64`, which hid overdue tasks deep in a long list.
  - Response buffer: **24 KB** cap (Graph JSON is verbose and `$select` cannot trim it; see spec §7.3). Same lazy allocation, same truncation tolerance.
  - Parse: `value` array; per task copy `title` → `copyTitle`, `importance` → priority mapping (`high`→4, `normal`→2, `low`→1), `dueDateTime.dateTime` (ISO 8601 with offset, e.g. `2026-10-10T09:30:00.0000000+02:00`) → extract `YYYY-MM-DD` prefix and `HH:MM` after `T` (reuse the `extractDueTime` logic; the offset suffix is ignored — the device clock is UTC-pinned in the activity, matching Todoist's approach).
  - Overdue computation: same `sscanf("%4d-%2d-%2d")` + `localtime_r` comparison as `TodoistClient::isOverdue`.
  - HTTP 404 on the tasks request → `ListGone` (caller re-resolves the list once and retries).
  - `httpStatusToFetchResult` mapping identical to Todoist's, plus `ListGone`.
- [ ] 3.3 Build: `pio run` (all three environments). `./bin/clang-format-fix -g`.
- [ ] 3.4 Hardware smoke test: none yet (no caller). Code review only.

**PR 3 contents:** `MsTodoClient.{h,cpp}`. Nothing user-visible yet.

---

## Phase 4 — Activity (PR 4)

**Goal:** `MsTodoActivity` — sign-in screen, task list, snapshot capture. This is the first user-visible PR.

- [ ] 4.1 Create `src/activities/integrations/MsTodoActivity.h` by adapting `TodoistActivity.h`. State machine gains one state:
  ```cpp
  enum class State { Loading, SignIn, SignInWaiting, ShowingTasks, ShowingError };
  ```
  New members: `DeviceCodeInfo _deviceCode;` `long _pollDeadlineEpoch;` `int _pollIntervalSec;` `long _nextPollMs;`
- [ ] 4.2 Create `src/activities/integrations/MsTodoActivity.cpp` by adapting `TodoistActivity.cpp` (519 lines — expect ~600 with sign-in):
  - `onEnter()`: identical shape — save entry orientation, force portrait (`renderer.setOrientation(GfxRenderer::Orientation::Portrait)`), `State::Loading`, `startFetch()`.
  - `startFetch()`: token check becomes `hasTokens()`; if false → `State::SignIn` and `beginSignIn()`.
  - `beginSignIn()`: WiFi up (reuse the `WifiSelectionActivity` result pattern), `applyTimezone()` + `syncTimeWithNTP()` (copy verbatim from `TodoistActivity.cpp` — the build-date fallback matters for TLS), `MsTodoAuth::requestDeviceCode(MSTODO_CONFIG.getClientId(), _deviceCode)` → render sign-in screen, `State::SignInWaiting`.
  - `loop()` in `SignInWaiting`: Back cancels (finish + `wifiOff()`); otherwise when `millis() >= _nextPollMs`, call `pollToken()`. Map results: `Pending` → keep waiting (check `expires_in` deadline → `Expired` screen), `SlowDown` → `_pollIntervalSec += 5`, `Ok` → `MSTODO_CONFIG.setTokens(...)` → proceed to fetch, others → error screen.
  - `proceedWithFetch()`: time sync first, then `MsTodoAuth::ensureFreshToken` logic inline — if `isAccessTokenExpired()`, call `refreshTokens()`; on `InvalidGrant` → clear tokens, restart sign-in; on success → `setTokens()` (persisting rotated refresh token) → fetch.
  - Time sync (both `beginSignIn()` and `proceedWithFetch()`): check the hardware RTC before waiting on NTP. `halClock.isAvailable()` is true on the X4 Pro/X4 Classic (BM8563 at I²C 0x51) and the SDK's `Rtc::now()` already rejects readings with the VL flag set (oscillator stopped / never set), so a successful `halClock.localTime()` read means the clock is trustworthy. When the RTC holds a valid time, skip the `syncTimeWithNTP()` wait entirely — it saves 1–2 s on the fetch path. Fall back to the Todoist `syncTimeWithNTP()` (with its build-date fallback for TLS) only when the RTC is absent or unreadable. Note `WifiSelectionActivity` already NTP-syncs the RTC on the first WiFi connection, so in practice the RTC path is the common case.
  - Fetch: `MsTodoClient::fetchToday(token, listId, tasks)`; on `ListGone` → clear `listId`, retry once; on success → same chronological sort as Todoist (copy the comparator verbatim), `captureSnapshotIfNeeded()`.
  - `renderSignIn()`: header `STR_MSTODO_SIGNIN_TITLE`, then a two-column body — QR code on one side (via `QrUtils::drawQrCode(renderer, Rect{...}, "https://microsoft.com/devicelogin")`, the same 198×198 pattern as `CrossPointWebServerActivity`; the helper and `ricmoo/QRCode` dependency already ship in the firmware), the user code large on the other side with `STR_MSTODO_SIGNIN_BODY` above and `STR_MSTODO_SIGNIN_SCAN` below it. Hint bar `Back` only. `renderSignInWaiting()`: same plus `STR_MSTODO_SIGNIN_WAITING`.
  - `captureSnapshotIfNeeded()` / `writeSnapshotMeta()`: copy from Todoist with `mstodo_sleep.*` paths, but simplified — no render-twice trick and no orientation branching, since the activity is always portrait and the snapshot matches it. The meta records only the capture time (no orientation field).
  - `onExit()`: `wifiOff()` + orientation restore, identical to Todoist.
- [ ] 4.3 Add the remaining i18n keys (full list in spec §8.2, including the new `STR_MSTODO_SIGNIN_SCAN`). Run `gen_i18n.py`.
- [ ] 4.4 Register the activity in `ActivityManager` and add the home-screen menu entry in `HomeActivity` (adapt the Todoist entry: icon can reuse `checklist.h` until a dedicated icon exists; label `STR_MSTODO`).
- [ ] 4.5 Build: `pio run` (all three environments). `./bin/clang-format-fix -g`.
- [ ] 4.6 Hardware smoke test: full sign-in on device (QR + code shown → phone scan or URL entry → sign-in → task list renders), Back-cancel mid-poll, refresh via Confirm, snapshot written to SD. Verify the QR scans reliably from the e-ink panel at arm's length (no grayscale, high contrast — should be fine, but confirm on hardware).

**PR 4 contents:** `MsTodoActivity.{h,cpp}`, `HomeActivity` menu entry, `ActivityManager` registration, i18n keys. **First user-visible PR.**

---

## Phase 5 — Settings submenu (PR 5)

**Goal:** `MsTodoSettingsActivity` — toggle, sign out.

- [ ] 5.1 Create `src/activities/settings/MsTodoSettingsActivity.{h,cpp}` by adapting `TodoistSettingsActivity.{h,cpp}` (124 lines). Rows: sleep screen toggle, **Sign out** (replaces Todoist's "Forget" — sign-out keeps the sleep-screen toggle per spec §8.1). The two orientation rows from the Todoist submenu are dropped.
- [ ] 5.2 Wire the submenu into `SettingsActivity` next to the Todoist submenu.
- [ ] 5.3 Build: `pio run` (all three environments). `./bin/clang-format-fix -g`.
- [ ] 5.4 Hardware smoke test: toggle persists across power-cycle; sign-out clears tokens + snapshot but keeps the toggle; next open re-runs sign-in.

**PR 5 contents:** `MsTodoSettingsActivity.{h,cpp}`, `SettingsActivity` wiring, i18n keys.

---

## Phase 6 — Sleep-screen integration (PR 6)

**Goal:** `SleepActivity` pre-check for the MS To Do snapshot.

- [ ] 6.1 Add `tryRenderMsTodoSleepScreen()` to `SleepActivity` by adapting `tryRenderTodoistSleepScreen()` (lines ~906-980): same meta parse and `renderBitmapSleepScreen` call, with `mstodo_sleep.*` paths and `MSTD` log tag — but **no orientation-match gate**: the snapshot is always portrait, so the pre-check only tests the toggle and file existence, then `renderer.setOrientation(GfxRenderer::Orientation::Portrait)` before rendering.
- [ ] 6.2 Call it at line ~571 **after** the Todoist pre-check (Todoist keeps precedence per spec decision #11):
  ```cpp
  if (tryRenderTodoistSleepScreen()) return;
  if (tryRenderMsTodoSleepScreen()) return;
  ```
- [ ] 6.3 Build: `pio run` (all three environments). `./bin/clang-format-fix -g`.
- [ ] 6.4 Hardware smoke test: full Todoist-spec sleep matrix (§11.2 of the Todoist spec) with `mstodo_*` paths, plus the both-enabled precedence case.

**PR 6 contents:** `SleepActivity.cpp` only.

---

## Phase 7 — Documentation (PR 7)

**Goal:** user guide + plan/spec cross-links.

- [ ] 7.1 Write `docs/mstodo-integration.md` modeled on `docs/todoist-integration.md`: prerequisites (Azure app registration note for fork maintainers, device-code sign-in instead of API token), config file reference (`mstodo.json` schema), first-run walkthrough (sign-in flow), settings table, file reference, troubleshooting (sign-in expired/declined, `invalid_grant` re-sign-in, `MBEDTLS_ERR_SSL_ALLOC_FAILED` heap note, list-deleted re-resolution), privacy section (tokens on SD in plain text, sign-out semantics).
- [ ] 7.2 Verify all doc paths/commands match the shipped code (file names, settings row labels, serial log tags).
- [ ] 7.3 Build: `pio run` (docs-only PR, but confirm the tree still builds). No firmware change.

**PR 7 contents:** `docs/mstodo-integration.md`.

---

## Future work (v2 candidates)

The following ideas are traced here for future consideration. They are not part of any active phase.

- **Pagination of Graph API results.** The current `$top=64` cap (and the 24 KB response buffer) means the client truncates large task lists silently. A pagination loop using `@odata.nextLink` would fetch all matching tasks across multiple requests, accumulating them into the vector until the full result set is retrieved or a configurable cap is reached. This is especially valuable for users with many tasks due within the window. The response buffer pattern (lazy allocation, truncation tolerance) stays the same; only the fetch loop changes.
- **Overdue tasks visibility toggle.** Currently overdue tasks are always shown. A `show_overdue` setting in `mstodo.json` (default `true`) would let the user hide overdue tasks from the list — useful when the list has accumulated stale items that are no longer actionable. The toggle is persisted alongside `sleep_screen_enabled` and survives sign-out.
- **Configurable future-day window.** The current hard-coded 7-day forward window (`dueDateTime le '<today+7>'`) is a server-side `$filter` bound. A `future_days` setting (default `7`, range 1–30) would let the user control how many days ahead tasks are fetched. The value is stored in `mstodo.json` and used to build the `$filter` date bound at fetch time. This directly affects the response size budget and the vector reservation cap, so the config change must flow through `MsTodoConfig` and the fetch filter must be rebuilt on each call.

## Phase-end verification (every phase)

- `pio run -t clean && pio run` — 0 errors, 0 new warnings.
- `pio run -e gh_release && pio run -e slim` — pass.
- `./bin/clang-format-fix -g` — idempotent.
- `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/` — generated headers in sync (phases that touch YAML).
- Manual review: portrait-only rendering enforced; no hardcoded geometry; all UI strings via `tr()`; all logging via `LOG_*` tag `"MSTD"`; all file I/O via `Storage`; no token values in logs.

## Final hardware checklist (before the last PR merges)

See spec §12.2 for the full list. Minimum gate: sign-in end-to-end, token refresh across a power cycle, task render with mixed overdue/today/future, snapshot + sleep-screen display, sign-out semantics, 20× open/close heap stability, and a reflash of upstream `gh_release` confirming no SPIFFS residue.
