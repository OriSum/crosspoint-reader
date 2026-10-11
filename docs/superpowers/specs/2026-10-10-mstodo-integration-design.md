# Microsoft To Do Integration — Design Spec

**Date:** 2026-10-10
**Project:** CrossPoint Reader (Xteink X4, ESP32-C3)
**Scope:** Personal fork; modular enough that other users can supply their own Azure app registration (client ID).
**Status:** Draft v1 — modeled on the shipped Todoist integration (`docs/superpowers/specs/2026-05-04-todoist-integration-design.md`), with the auth model replaced by OAuth 2.0 device-code flow.

---

## 1. Summary

Add a Microsoft To Do integration that lets the user open a read-only "Today" task list on the e-reader, and optionally use the most recent rendering of that list as the device's sleep screen. All Microsoft To Do state lives on the SD card; the internal flash settings partition (SPIFFS) is **not** modified, so reflashing upstream firmware leaves no persistent residue.

Refresh is **explicitly user-driven** — there is no background polling, no scheduled wake, and no battery cost beyond the WiFi connection used during a manual fetch or sign-in.

The integration reuses the Todoist integration's architecture end to end (config singleton, stateless HTTPS client, activity with snapshot capture, sleep-screen pre-check). The one structural difference is authentication: Microsoft Graph has no personal API token, so the device performs an **OAuth 2.0 device authorization grant** (RFC 8628) — the e-ink screen shows a short code, the user enters it at `microsoft.com/devicelogin` on any phone or PC, and the device polls until Microsoft issues the tokens.

## 2. Goals

- View the user's Microsoft To Do tasks due today, plus overdue ones, from the default task list.
- Persist the most recent rendering as a BMP usable by the existing `SleepActivity` pipeline.
- Let the user toggle the Microsoft To Do sleep screen on/off, falling back to the existing `/.sleep` rotation when off.
- Keep all Microsoft To Do state on the SD card (tokens, toggles, snapshot).
- Portrait only: no orientation settings — the activity and the sleep snapshot always render in portrait.
- Sign in without any on-device keyboard: device-code flow only.
- Reuse the project's existing TLS pattern (`esp_crt_bundle_attach`) — Microsoft Graph chains to DigiCert roots, which are in the ESP-IDF Mozilla CA bundle.

## 3. Non-goals (v1)

- Mark-complete, add, edit, or delete tasks (read-only only).
- Task creation, steps, notes, categories, reminders.
- Multiple list selection (v1 always uses the default task list; `wellknownListName == "defaultList"`).
- Multiple Microsoft accounts.
- Filters other than the fixed "today + overdue" query.
- Weather, geolocation, timezone, date-format, or layout-mode settings.
- Orientation settings — the activity and the sleep snapshot are always portrait (unlike the Todoist integration, which exposes two orientation settings).
- Background / scheduled refresh.
- An OAuth authorization-code flow with a redirect URI (impossible on a headless device).
- Web settings page integration (entirely SD-driven).

## 4. User-facing behavior

### 4.1 First-time setup

1. User creates `/.crosspoint/mstodo.json` on the SD card. The only field most users need is nothing at all — the fork ships a compiled-in default `client_id` for its registered Azure app. Users who prefer their own app registration add `"client_id": "<guid>"`.
2. User selects "MS To Do" from the home screen menu.
3. The activity brings up WiFi and starts the device-code flow:
   - The screen shows a QR code encoding `https://microsoft.com/devicelogin` (rendered via the existing `QrUtils::drawQrCode` — same pattern as `CrossPointWebServerActivity`), with `Go to microsoft.com/devicelogin and enter: ABCD-1234` beside it.
   - The user scans the QR with a phone camera (or types the URL on a PC) and completes sign-in within 15 minutes.
   - The device polls Microsoft's token endpoint every 5 seconds (backing off to 10 on `slow_down`) until the tokens arrive.
4. The device fetches today's tasks, renders them, captures a snapshot, and drops WiFi.

### 4.2 Daily use

- **Open MS To Do activity from home screen** → validates/refreshes the token if needed, fetches once, renders, captures snapshot, drops WiFi.
- **Up / Down** → scroll the list (no re-fetch).
- **Confirm** → manual refresh (re-runs token check → WiFi → fetch → render → snapshot → drop WiFi).
- **Back** → exit (also cancels an in-progress sign-in poll).
- **Sleep** → if the toggle is on and a snapshot exists, blit the snapshot. Otherwise fall through to the existing sleep behavior.

### 4.3 Settings the user controls

| Setting | Stored in | Default |
|---|---|---|
| MS To Do sleep screen on/off | `/.crosspoint/mstodo.json` | `false` |
| Client ID (optional override) | `/.crosspoint/mstodo.json` | compiled-in default |
| Sign out | — | deletes tokens + snapshot, keeps the sleep-screen toggle |

Settings are exposed via an MS To Do submenu in the existing `SettingsActivity`. "Sign out" deletes the token fields from `mstodo.json` and the snapshot BMP/meta, but keeps the user's sleep-screen toggle — re-signing-in should not silently reset preferences.

**Persistence model:** identical to `TodoistConfig` — every change is written to `/.crosspoint/mstodo.json` immediately. Setters compare against the current value (write-throttling) and, on change, perform an atomic write: serialize to `mstodo.json.tmp`, close, rename to `mstodo.json`. Power loss mid-write leaves either the old file or the new file, never a corrupt half.

**Orientation:** unlike the Todoist integration, there are no orientation settings. The activity and the sleep snapshot always render in portrait (`GfxRenderer::Orientation::Portrait`). This removes the render-twice trick from the Todoist design — activity and snapshot orientations can never differ, so `captureSnapshotIfNeeded()` reduces to a single render + save. If the device is physically held in landscape, the portrait view is shown as-is (the user rotates the device to read it).

### 4.4 Active view layout

Identical to the Todoist active view (header with `Updated HH:MM`, bullet list, marker glyphs, dd/mm date suffix, cursor band, scroll bar). Only the header title and the strings differ:

```
  MS To Do — Updated 09:42                      [ 7 tasks ]
  ─────────────────────────────────────────────────────────
  [!]  09:30   Pay landlord
       14:00   Reply to Mateus
               Pick up groceries (no time)
  [!]          Overdue: renew SIM
  ...
```

- Marker glyphs: `!` overdue, `•` today/undated, `›` future-dated — same semantics as Todoist.
- Title wraps via truncation, never line-wraps.
- All coordinates and fonts go through `UITheme` (the GUI macro). No hardcoded geometry.
- Active-view renderer orientation is always portrait, regardless of the device's physical orientation.

## 5. Architecture

### 5.1 New modules

```
src/integrations/mstodo/
├── MsTodoConfig.{h,cpp}     Singleton. Loads/persists /.crosspoint/mstodo.json.
├── MsTodoAuth.{h,cpp}       OAuth 2.0 device-code flow + token refresh. Stateless per call.
├── MsTodoClient.{h,cpp}     Microsoft Graph HTTPS + JSON for the task list. Stateless.
└── MsTodoTask.h             POD struct: title, dueDateTime, importance, overdue.

src/activities/integrations/
└── MsTodoActivity.{h,cpp}   Sign-in / live scrollable view + WiFi management + snapshot capture.
```

`MsTodoAuth` is the only module with no Todoist counterpart. Everything else is a structural sibling of the `todoist` namespace and should be written by adapting the Todoist files, not by inventing new patterns.

### 5.2 Modified files

```
src/activities/boot_sleep/SleepActivity.cpp     Add MS To Do pre-check next to the Todoist one.
src/activities/home/HomeActivity.{cpp}           Add "MS To Do" menu entry.
src/activities/settings/SettingsActivity.{cpp}   Add MS To Do submenu (toggle, Sign out).
src/main.cpp                                     Boot-time MsTodoConfig::getInstance().load();
lib/I18n/translations/english.yaml               Add new STR_MSTODO_* keys.
```

### 5.3 Files NOT modified

- `src/CrossPointSettings.{h,cpp}` — no SPIFFS schema change.
- `src/SettingsList.h` — no new SPIFFS-backed settings.
- `partitions.csv` — unchanged.

Same rollback property as the Todoist integration: flash upstream firmware and nothing Microsoft-related survives except removable SD files.

### 5.4 Dependency boundaries

- `MsTodoActivity` depends on `MsTodoConfig` (read), `MsTodoAuth` (sign-in/refresh), `MsTodoClient` (fetch), `HalDisplay` (render), WiFi.
- `MsTodoAuth` depends on `esp_http_client`, ArduinoJson, and `esp_crt_bundle_attach`.
- `MsTodoClient` depends on `esp_http_client`, ArduinoJson, and `esp_crt_bundle_attach`.
- `MsTodoConfig` depends only on `HalStorage` and ArduinoJson.
- `SleepActivity` depends on `MsTodoConfig` (read-only check) and reuses its existing `renderBitmapSleepScreen`.
- `MsTodoAuth` and `MsTodoClient` do not depend on each other; the activity sequences them.

## 6. Authentication design (the new part)

### 6.1 Device authorization grant (RFC 8628)

Microsoft Entra ID supports the device-code flow for public clients (no client secret — mandatory, since nothing on this device can be kept secret).

**Step 1 — request a device code:**

```
POST https://login.microsoftonline.com/common/oauth2/v2.0/devicecode
Content-Type: application/x-www-form-urlencoded

client_id=<CLIENT_ID>&scope=Tasks.Read+offline_access+User.Read
```

Response:

```json
{
  "device_code": "…long opaque string…",
  "user_code": "ABCD1234",
  "verification_uri": "https://microsoft.com/devicelogin",
  "expires_in": 900,
  "interval": 5,
  "message": "To sign in, use a web browser to open …"
}
```

**Step 2 — display and poll:**

- Render the sign-in screen on the e-ink display (via `STR_MSTODO_SIGNIN_*` strings):
  - QR code encoding the fixed `verification_uri` (`https://microsoft.com/devicelogin`) via `QrUtils::drawQrCode(renderer, Rect{...}, "https://microsoft.com/devicelogin")`. The URI is a compile-time constant ~38 bytes, so the QR stays at version 4 (~114-byte capacity) with large, easily scanned modules. The code is NOT embedded in the QR — Microsoft's devicelogin page has no pre-filled-code parameter, so a combined payload would only bloat the QR.
  - The `user_code` rendered large beside the QR for manual entry.
- Poll at `interval` seconds:

```
POST https://login.microsoftonline.com/common/oauth2/v2.0/token
Content-Type: application/x-www-form-urlencoded

grant_type=urn:ietf:params:oauth:grant-type:device_code
&client_id=<CLIENT_ID>
&device_code=<DEVICE_CODE>
```

- `authorization_pending` → keep polling.
- `slow_down` → increase interval by 5 s (RFC 8628 §3.5).
- `authorization_expired` (after `expires_in`) → show `STR_MSTODO_SIGNIN_EXPIRED`, stop.
- `declined` → show `STR_MSTODO_SIGNIN_DECLINED`, stop.
- Success → `access_token`, `refresh_token`, `expires_in` (~3600 s).

**Step 3 — persist and use:**

- Tokens are written to `mstodo.json` via the same atomic temp+rename write as every other config change.
- Every Graph request carries `Authorization: Bearer <access_token>`.

### 6.2 Token refresh

Before any fetch, the activity checks `MsTodoConfig::isAccessTokenExpired()` (stored `token_expiry_epoch` minus a 60 s safety margin against clock skew). When expired:

```
POST https://login.microsoftonline.com/common/oauth2/v2.0/token
grant_type=refresh_token&client_id=<CLIENT_ID>&refresh_token=<REFRESH_TOKEN>
&scope=Tasks.Read+offline_access+User.Read
```

Microsoft rotates refresh tokens: whenever a refresh response contains a new `refresh_token`, it replaces the stored one. If the refresh fails with `invalid_grant` (revoked, expired after ~90 days of inactivity), the integration falls back to the sign-in flow — the user simply signs in again; no error dead-end.

### 6.3 Client ID

- The fork ships a compiled-in default `client_id`: `23db5763-8f56-47b7-83ed-83e808e77c71` (a public-client Azure app registration with delegated `Tasks.Read`, `offline_access`, `User.Read` and "Allow public client flows" enabled). This is the fork maintainer's responsibility to register once.
- A user may override it via `"client_id"` in `mstodo.json` (e.g. corporate tenant policies). Validation: non-empty, 36-char GUID shape.
- The client ID is not a secret; storing it in plain text on the SD card is acceptable. Access and refresh tokens are also plain text on the SD card — same trade-off as the Todoist token, documented in the user guide.

### 6.4 Auth error mapping

| Scenario | Behavior |
|---|---|
| No tokens stored | Sign-in flow (device code screen) |
| `devicecode` endpoint unreachable / 4xx | `STR_MSTODO_SIGNIN_FAILED` |
| Poll `authorization_pending` past `expires_in` | `STR_MSTODO_SIGNIN_EXPIRED` |
| Poll `declined` | `STR_MSTODO_SIGNIN_DECLINED` |
| Refresh `invalid_grant` | Restart sign-in flow |
| Refresh network failure | `STR_MSTODO_OFFLINE` |
| Back pressed during sign-in | Cancel polling, exit activity |

## 7. Data flow

### 7.1 Opening the activity (happy path, already signed in)

```
User selects "MS To Do" on home screen
  → MsTodoActivity::onEnter()
    → renderer.setOrientation(GfxRenderer::Orientation::Portrait)
    → drawCenteredText(STR_MSTODO_FETCHING) + displayBuffer()
    → MsTodoConfig.hasTokens()?  [no → sign-in flow, §6.1]
    → bringUpWifi(timeoutMs=15000)  [may render error & exit]
    → syncTimeWithNTP()             [matches TodoistActivity pattern]
    → MsTodoAuth::ensureFreshToken()  [refresh if expired; may re-enter sign-in]
    → MsTodoClient::fetchToday(accessToken, &tasks)
        - GET https://graph.microsoft.com/v1.0/me/todo/lists  → resolve default list id
        - GET https://graph.microsoft.com/v1.0/me/todo/lists/<id>/tasks
              ?$filter=status%20ne%20%27completed%27
              %20and%20dueDateTime/dateTime%20ge%20%271970-01-01%27
              %20and%20dueDateTime/dateTime%20le%20%27<today%2B7>%27
              &$top=64
              [percent-encoded; raw spaces/quotes are rejected by http_parser;
               $top=64 is a safety cap to keep the response under the 24 KB buffer]
              [no $select — see §7.2]
        - Streamed deserializeJson into reserved std::vector<MsTodoTask> (cap 64)
        - Client-side filter: due date == today OR (due date < today AND not completed)
    → captureSnapshotIfNeeded()     [identical render-twice trick as Todoist]
    → WiFi.disconnect()
```

Two Graph requests per fetch (list resolution + tasks) instead of Todoist's one. The resolved default-list id is cached in `mstodo.json` (`list_id`) so subsequent fetches skip the first request; it is re-resolved when a fetch returns 404 for the stored id (user deleted the list).

### 7.2 Why no `$select`, and why the date filter

Microsoft Graph documents `$select` as supported on the To Do endpoints, but on a personal Microsoft account (MSA) it does not work:

- A real field name (`title`, `id,wellknownListName`) → **HTTP 400** `RequestBroker--ParseUri`. The client maps 400 to a generic network error, which is what produced the original "Could not fetch tasks".
- A field name the broker accepts (`id`, `status`) → **200 but silently ignored**; every field is still returned, so the payload is not reduced.
- An unknown field name → a different 400 (`BadRequest`, "Parsing OData Select and Expand failed").

The failure is deterministic and independent of parameter order, comma encoding, headers, and API version (v1.0 and beta both fail). Microsoft's own docs warn that query parameters can "fail silently" and that list-tasks supports only "some" of the OData query parameters. `$select` is therefore removed from both requests.

The tasks request instead filters server-side on the due date:

```
$filter=status ne 'completed'
        and dueDateTime/dateTime ge '1970-01-01'
        and dueDateTime/dateTime le '<today + 7 days>'
```

- `ge '1970-01-01'` excludes undated tasks; a null comparison (`eq null`) is rejected with 400, so an old lower bound is used.
- `le '<today + 7 days>'` bounds the window to overdue + today + the next seven days, computed on-device from the local clock.

This replaces the previous `$top=64`, which returned the first 64 tasks in list order and hid overdue tasks sitting deeper in a long list. The today/overdue split is still computed on-device from `dueDateTime.dateTime`, exactly like `TodoistClient::isOverdue` does with `sscanf` + `localtime_r`.

### 7.3 Response size budget

Graph task JSON is verbose (~250–350 bytes/task vs Todoist's ~180) and `$select` cannot trim it (§7.2). The date `$filter` keeps the window small in practice (a few KB for a typical account), but a user with many tasks due in the next week can still produce a large body. The response buffer is therefore **24 KB**, allocated lazily after the TLS handshake exactly as `TodoistClient` does (the 16 KB Todoist cap is too small here; 32 KB is not reliably available as one contiguous block on the C3 after WiFi+NTP+TLS). Truncation is handled gracefully — parse what arrived, log `MSTD truncated`.

### 7.4 Refresh (Confirm pressed)

Same as opening, minus loading text: token check → WiFi → fetch → render → captureSnapshot → drop WiFi.

### 7.5 Sleep screen integration

Identical to `SleepActivity::tryRenderTodoistSleepScreen()`, with MS To Do paths:

```
SleepActivity::onEnter():
  - Todoist pre-check (unchanged, runs first)
  - MS To Do pre-check (new):
      if MsTodoConfig::isSleepScreenEnabled()
         AND Storage.exists("/.crosspoint/mstodo_sleep.bmp"):
           renderer.setOrientation(GfxRenderer::Orientation::Portrait)
           open + parse BMP
           call existing renderBitmapSleepScreen(bitmap)
           return
  - Otherwise: fall through to existing switch on SETTINGS.sleepScreen.
```

No orientation-match gate is needed: the snapshot is always portrait, so the meta file only records the capture time. (The Todoist pre-check keeps its orientation gate — its behavior is untouched.)

Precedence when both integrations are enabled: Todoist first (existing behavior is untouched), MS To Do second. Documented in the user guide so the interaction is not a surprise.

The snapshot is **not** dropped into `/.sleep/` — same rationale as Todoist: that folder is a randomized rotation pool, and the MS To Do snapshot must display deterministically when toggled on.

### 7.6 Exit

```
MsTodoActivity::onExit():
  - Free task vector (per-task strings are fixed-size char arrays, no heap to free).
  - wifiOff() (stops SNTP, disconnects, powers radio down — TodoistActivity pattern).
  - Restore entry orientation so HomeActivity's cached coverBuffer stays valid (the activity forced portrait on entry).
  - Activity::onExit().
```

No FreeRTOS tasks, no malloc'd buffers held across the activity boundary. The sign-in poll loop is a synchronous bounded loop inside `onEnter` (5 s `vTaskDelay` ticks, cancellable via Back checked between ticks), not a background task.

## 8. Settings & configuration

### 8.1 `/.crosspoint/mstodo.json` schema

```json
{
  "client_id": "optional-guid-override",
  "access_token": "string",
  "refresh_token": "string",
  "token_expiry_epoch": 1760000000,
  "list_id": "resolved-default-list-id",
  "sleep_screen_enabled": false
}
```

- Only the toggle field is user-editable on device; tokens and `list_id` are written by the auth/fetch paths.
- Unknown keys are ignored. Missing fields fall back to defaults. `activity_orientation`/`snapshot_orientation` keys from a hand-edited file are ignored (portrait is fixed).
- File bounded at 4 KB on load, like `TodoistConfig` — larger files are rejected and the integration disables itself.
- `Sign out` removes `access_token`, `refresh_token`, `token_expiry_epoch`, `list_id`, the snapshot BMP and meta, and any orphaned `.tmp` files — but keeps `sleep_screen_enabled`.

### 8.2 i18n keys (additions to English YAML)

| Key | English |
|---|---|
| `STR_MSTODO` | "MS To Do" |
| `STR_MSTODO_FETCHING` | "Fetching tasks…" |
| `STR_MSTODO_SIGNIN_TITLE` | "Sign in to Microsoft" |
| `STR_MSTODO_SIGNIN_BODY` | "Go to microsoft.com/devicelogin and enter:" |
| `STR_MSTODO_SIGNIN_SCAN` | "or scan the QR code" |
| `STR_MSTODO_SIGNIN_WAITING` | "Waiting for sign-in… (Back to cancel)" |
| `STR_MSTODO_SIGNIN_EXPIRED` | "Sign-in code expired - try again" |
| `STR_MSTODO_SIGNIN_DECLINED` | "Sign-in declined" |
| `STR_MSTODO_SIGNIN_FAILED` | "Sign-in failed" |
| `STR_MSTODO_NO_TOKEN` | "Not signed in" |
| `STR_MSTODO_OFFLINE` | "WiFi unavailable" |
| `STR_MSTODO_FETCH_FAILED` | "Could not fetch tasks" |
| `STR_MSTODO_NO_TASKS` | "All clear today" |
| `STR_MSTODO_SLEEP_SCREEN` | "MS To Do sleep screen" |
| `STR_MSTODO_SIGNOUT` | "Sign out" |
| `STR_MSTODO_TODAY_HEADER` | "Updated %02d:%02d" |
| `STR_MSTODO_INVALID_TOKEN` | "Session expired - sign in again" |
| `STR_MSTODO_RATE_LIMITED` | "Rate limited - try later" |

Other languages fall back to English per existing project convention. Run `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/` after editing the YAML.

### 8.3 TLS certificate handling

Same pattern as `TodoistClient`: `esp_crt_bundle_attach` with the ESP-IDF Mozilla CA bundle. `login.microsoftonline.com` and `graph.microsoft.com` chain to DigiCert roots (G1/G2), all present in the bundle. No PEM file shipped, no SD setup, no first-run copy logic.

## 9. Error handling

Following `LOG_ERR + fallback` (same philosophy as the Todoist integration). Active view errors render a static screen and wait for Back. Sleep-screen errors silently fall through to the existing pipeline.

| Scenario | Behavior |
|---|---|
| `mstodo.json` malformed / > 4 KB | `STR_MSTODO_NO_TOKEN` (config treated as absent) |
| No tokens stored | Sign-in flow |
| SD unreachable | `STR_MSTODO_FETCH_FAILED` "storage unavailable" |
| WiFi credentials missing or AP unreachable (15s) | `STR_MSTODO_OFFLINE` |
| Graph 401 after a successful refresh | `STR_MSTODO_INVALID_TOKEN` → sign-in flow |
| Graph 429 | `STR_MSTODO_RATE_LIMITED` |
| Graph 5xx / timeout / TLS handshake failure | `STR_MSTODO_FETCH_FAILED` |
| Graph 404 for stored `list_id` | Re-resolve default list once, retry fetch once |
| Empty task list after filtering | `STR_MSTODO_NO_TASKS`, snapshot still captured |
| JSON parse error | `STR_MSTODO_FETCH_FAILED` |
| `malloc` for response/snapshot buffer fails | LOG_ERR; skip snapshot / report fetch failure; active view unaffected |
| Snapshot write fails (SD full, etc.) | LOG_ERR; skip; previous snapshot remains valid |
| Snapshot exists but toggle off | Ignore snapshot; existing sleep modes run |
| User pulls SD mid-fetch | `STR_MSTODO_FETCH_FAILED` "storage unavailable" |

**Failed refresh never overwrites a valid snapshot.** Stale tasks > blank tasks for the sleep screen — same invariant as Todoist.

## 10. Memory budget

| Buffer | Size | Lifetime |
|---|---|---|
| Framebuffer (existing) | 48 KB | Owned by renderer |
| `std::vector<MsTodoTask>` reserved 64 | ~7 KB | `onEnter` to `onExit` |
| ArduinoJson document (streaming) | ~10 KB | Fetch only (Graph envelope is heavier than Todoist's) |
| Per-task title `char[96]` | (already in vector) | — |
| HTTPS + TLS client transient (esp_http_client + mbedTLS handshake) | ~30 KB | Fetch only |
| Device-code poll responses | < 1 KB | Sign-in only |

Peak transient pressure during fetch: ~47 KB on top of baseline (vs ~45 KB for Todoist — the extra 2 KB is the larger JSON document). Same mitigations apply: lazy response-buffer allocation after the TLS handshake, `free()` before task-vector growth, `ESP.getFreeHeap()` logged before and after fetch for hardware verification.

Stack: rendering uses no large local variables. Fixed-size `char[96]` per-task lives in heap-vector storage, not stack. No `std::function`, no recursive rendering. The sign-in poll loop uses a fixed-size `char[64]` for the user code and no dynamic allocation per tick.

## 11. Rollback

| Level | Action | Reversibility |
|---|---|---|
| Soft | Settings → "MS To Do sleep screen" off | Instant; sleep returns to existing behavior |
| Sign out | Settings → "Sign out" | Deletes tokens + snapshot; sleep-screen toggle kept |
| Forget | Delete `/.crosspoint/mstodo.json` on SD | Full reset |
| Code-level | `git revert` the MS To Do commits; rebuild & flash | One build cycle |
| Upstream firmware | Flash upstream `gh_release` | No SPIFFS schema divergence to worry about |
| Nuclear | `esptool.py erase_flash` + reflash upstream | Full factory state |

## 12. Testing

### 12.1 Build-time (verifiable by AI)

- `pio run -t clean && pio run` — 0 errors, 0 new warnings.
- `pio run -e gh_release && pio run -e slim` — all pass.
- `./bin/clang-format-fix -g` — idempotent across new files.
- `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/` — generated headers in sync with source YAMLs.
- Manual code review: portrait-only rendering enforced; no hardcoded geometry; all UI strings via `tr()`; all logging via `LOG_*` with tag `"MSTD"`; all file I/O via `Storage` (HalStorage).

### 12.2 Hardware checklist (user runs)

**Configuration:**
- [ ] No `mstodo.json` → sign-in flow starts (not an error screen — tokens are optional until first sign-in)
- [ ] Malformed `mstodo.json` → integration disabled, log shows parse error
- [ ] No SD card → "storage unavailable"

**Sign-in:**
- [ ] First run → device-code screen renders code and URI
- [ ] Complete sign-in on phone within timeout → tokens persisted, task list renders
- [ ] Let the code expire without signing in → "code expired" screen
- [ ] Decline on the phone → "sign-in declined" screen
- [ ] Press Back mid-poll → activity exits, no orphaned WiFi state
- [ ] Power-cycle after sign-in → no re-sign-in needed (tokens loaded from SD)

**Token lifecycle:**
- [ ] Fetch with fresh token → no refresh request in serial log
- [ ] Manually set `token_expiry_epoch` to the past → fetch triggers refresh, then succeeds
- [ ] Corrupt `refresh_token` → refresh fails with `invalid_grant` → sign-in flow restarts cleanly

**Network / API:**
- [ ] No WiFi credentials → "WiFi unavailable"
- [ ] Working WiFi + valid session → tasks render
- [ ] Mid-fetch WiFi drop → "fetch failed"
- [ ] Empty today → "all clear today" + snapshot saved
- [ ] Overdue + today + future mix → correct glyphs and ordering
- [ ] Delete the default list in To Do, fetch → list re-resolved, fetch succeeds

**Snapshot & sleep:**
- [ ] Snapshot + sleep cases from the Todoist checklist (§11.2 of the Todoist spec) that don't involve orientation settings, with `mstodo_*` paths
- [ ] Both Todoist and MS To Do sleep screens enabled → Todoist wins; documented behavior
- [ ] Settings persistence: change a toggle, power-cycle → preserved (atomic write survived)
- [ ] "Sign out" → tokens + snapshot gone; sleep-screen toggle kept; next open re-runs sign-in

**Memory:**
- [ ] Heap pre-entry vs post-exit: within ~2 KB of baseline
- [ ] Open / close 20× → no monotonic leak
- [ ] Sign-in poll loop: heap stable across 15 minutes of polling
- [ ] Stack high-water mark > 512 bytes

**Battery sanity:**
- [ ] Standby with toggle on → drain matches normal sleep baseline
- [ ] One activity open per day for a week → battery roughly matches reading-only baseline

## 13. Future work (v2 candidates)

- Mark-complete from device (PATCH `/me/todo/tasks/{id}`; introduces write semantics and retry logic).
- List picker (enumerate `/me/todo/lists`, side-button-cycled).
- User-configurable filter string.
- On-device client_id entry via `KeyboardEntryActivity`.
- Web settings page integration.
- Shared task-list rendering: extract the common `TodoistActivity`/`MsTodoActivity` row renderer into a shared component once both integrations are stable.

## 14. Decisions log

| # | Decision | Rationale |
|---|---|---|
| 1 | v1 read-only | Same rationale as Todoist: consumption, not editing; halves scope |
| 2 | Device-code flow for auth | Only OAuth flow that works on a headless device with no keyboard and no browser |
| 3 | Compiled-in default client_id + SD override | Zero-setup for fork users; escape hatch for tenant restrictions |
| 4 | Tokens on SD, plain text | Same trade-off as the Todoist token; SD is removable and inspectable; SPIFFS stays untouched |
| 5 | Refresh token rotation persisted | Microsoft rotates refresh tokens; dropping the new one would break the session within weeks |
| 6 | Server-side date `$filter`, client-side today/overdue split | `$filter` on `dueDateTime` works and bounds the window to overdue + today + 7 days; the today/overdue split is still computed on-device |
| 7 | No `$select` on either request | `$select` returns HTTP 400 `RequestBroker--ParseUri` for real field names on MSA accounts, and is silently ignored for the rest — it cannot trim the payload (§7.2) |
| 8 | Default list only, cached `list_id` | One fewer HTTPS round-trip per fetch; re-resolve on 404 covers list deletion |
| 9 | Portrait only, no orientation settings | Simpler config and render path; the snapshot always matches the activity orientation, so no render-twice trick and no orientation gate at sleep |
| 10 | Snapshot does not auto-expire | User has full control via Refresh; staleness is user's responsibility |
| 11 | Todoist sleep-screen pre-check keeps precedence | Existing shipped behavior must not change when this integration lands |
| 12 | TLS via existing `esp_crt_bundle_attach` | DigiCert roots already in the bundle; zero new cert plumbing |
| 13 | Synchronous sign-in poll inside `onEnter` | No FreeRTOS task lifecycle to manage; Back-cancellable between ticks; matches the activity's existing synchronous fetch model |
| 15 | QR code on the sign-in screen encoding the URL only | Removes browser-and-URL typing on phones; `QrUtils::drawQrCode` + `ricmoo/QRCode` are already shipped and used by `CrossPointWebServerActivity`; fixed short URI keeps the QR at version 4 with large modules |
| 14 | Microsoft Graph v1.0 | Stable surface; beta endpoints change without notice |
