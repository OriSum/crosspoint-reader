# Microsoft To Do Integration

Pull your Microsoft To Do tasks onto the device and (optionally) render them
as the sleep-screen wallpaper, so the last thing you see when you put the
reader down is what you still owe yourself.

The integration is read-only: it fetches tasks over HTTPS and renders them.
Tasks are not created, edited, or completed from the device.

Unlike the Todoist integration, Microsoft To Do has no personal API token.
The device signs in with an **OAuth device code**: the e-ink screen shows a
QR code and a short code — scan the QR with a phone camera (or type
`microsoft.com/devicelogin` on any phone or PC), enter the code, and the
device takes care of the rest.

---

## Prerequisites

- An e-reader running CrossPoint Reader firmware that includes this branch
  (`mstodo-integration` or any release that merged it).
- A Microsoft account with tasks in Microsoft To Do.
- A WiFi network the device can reach (configured via the existing WiFi
  selection flow — Settings → WiFi).
- An SD card with a writable `/.crosspoint/` directory. The firmware
  creates the directory when it first writes settings; nothing else is
  required.

No configuration file is required for a first run — the sign-in flow starts
automatically the first time you open the activity.

The fork ships a compiled-in Azure app registration (a public client with
delegated `Tasks.Read`, `offline_access`, and `User.Read` permissions), so
you do not need to register an app yourself. Fork maintainers who want their
own registration can set `client_id` in `mstodo.json` (see §2).

---

## 1. First run: sign in

1. From the home screen menu, open **MS To Do**.
2. The device connects to WiFi and shows a sign-in screen with a QR code
   and a code:

   ```
   Sign in to Microsoft
   ┌─────────┐  Go to microsoft.com/devicelogin
   │ ▓▓ QR ▓▓│  and enter:
   │ ▓▓▓▓▓▓▓▓│
   └─────────┘       ABCD-1234
        or scan the QR code
   ```

3. On a phone, scan the QR code with the camera app — it opens
   <https://microsoft.com/devicelogin>. On a PC, open that address
   directly. Then enter the code shown on the e-reader and sign in with
   the Microsoft account whose tasks you want.
4. Within a few seconds of your approval, the device fetches your tasks and
   shows the list. The session is stored on the SD card — you won't need to
   sign in again until the session expires (roughly 90 days of not using
   the integration) or you sign out.

The code is valid for 15 minutes. If it expires, the screen says so and you
can start over with **Retry**. Press **Back** at any time to cancel.

---

## 2. Optional: config file on the SD card

Everything works without a config file. `/.crosspoint/mstodo.json` is
created automatically on the SD card the first time the device stores its
sign-in session. You only need to edit it by hand for the optional client
ID override.

### Minimal config (optional)

```json
{}
```

### Full config (every supported field)

```json
{
  "client_id": "00000000-0000-0000-0000-000000000000",
  "sleep_screen_enabled": true
}
```

### Field reference

| Field | Type | Default | Allowed values |
|---|---|---|---|
| `client_id` | string | `23db5763-8f56-47b7-83ed-83e808e77c71` | Azure app registration GUID. Only needed if you register your own app (e.g. corporate tenant policy). |
| `access_token` | string | (managed) | Set by the device after sign-in. Don't edit. |
| `refresh_token` | string | (managed) | Set by the device. Don't edit. |
| `token_expiry_epoch` | number | (managed) | Unix time when the access token expires. Don't edit. |
| `sleep_screen_enabled` | bool | `false` | When `true` and a snapshot exists, the sleep screen renders the cached task list instead of the default rotation. |

The file is bounded at 4 KB — parsing aborts and the integration treats
the file as absent if it is larger, so you are asked to sign in again.
Unknown keys are ignored on load and dropped on the next persist.

**Treat the file like a password.** It contains live session tokens in
plain text. If you lend the SD card to someone, run **Settings → MS To Do →
Sign out** first, or delete `/.crosspoint/mstodo.json` manually.

---

## 3. Build and flash the firmware

```bash
# Build firmware (default environment)
pio run

# Build and upload to the device
pio run -t upload

# Combined upload + serial monitor
pio run -t upload && pio device monitor
```

Filesystem images do not need to be uploaded — the integration only writes
to the SD card, never to SPIFFS.

If you have a serial port set in `platformio.local.ini`, the upload picks
it up automatically. Otherwise pass it explicitly:

```bash
pio run -t upload --upload-port /dev/cu.usbmodem2101  # macOS
pio run -t upload --upload-port COM7                  # Windows
```

For a release build (smaller binary, no debug logging):

```bash
pio run -e gh_release -t upload
```

---

## 4. Daily use

1. From the home screen menu, open **MS To Do**.
2. The activity validates its session (refreshing the token if needed),
   brings up WiFi, syncs the clock (from the hardware RTC when available,
   otherwise NTP), and fetches your tasks. Expect a few seconds for a small
   account; a large account (many lists) takes longer, because the Planned
   view walks every list over one connection.
3. Press **Confirm** to refresh, **Up/Down** to scroll, **Back** to exit.

The list shows the Microsoft To Do **Planned** view: every task with a due
date in the overdue + today + next seven days window, across **all** of your
To Do lists. Completed tasks and tasks with no due date are never shown.

After every successful fetch, the device also writes:

- `/.crosspoint/mstodo_sleep.bmp` — the rendered task list as a bitmap
- `/.crosspoint/mstodo_sleep.meta` — JSON sidecar with the capture time

These are written regardless of the sleep-screen toggle; the toggle only
controls whether the sleep screen uses them. When the device sleeps,
`SleepActivity` checks for the snapshot and, if it exists, renders the
cached BMP as the sleep wallpaper.

---

## 5. Configure on device

**Settings → MS To Do** exposes the persisted settings, each with atomic
write-through to the file:

| Row | What it does |
|---|---|
| MS To Do sleep screen | Toggles `sleep_screen_enabled`. |
| Sign out | Removes the stored session, the snapshot BMP and meta, and any orphaned `.tmp` files. Your sleep-screen toggle is kept. **No confirmation prompt — single press signs you out.** |

The next time you open the activity after signing out, the sign-in flow
starts again.

If both the Todoist and MS To Do sleep screens are enabled, **Todoist
wins** — its snapshot is checked first. Turn one of them off if you want
the other to show.

---

## What the activity shows

A single fixed view: an "MS To Do" header with an `Updated HH:MM` stamp
from the last successful fetch, and the task list below it. The query
fetches overdue tasks, tasks due today, and tasks due within the next
seven days from **every** list — so there are no filter, timezone, or
layout settings to manage. Each row shows a marker glyph (bullet for
today, `!` for overdue, `›` for future-dated), the task title, and a
dd/mm date suffix when the task carries a due date.

---

## Why the requests look the way they do

This section records a Microsoft Graph quirk that shaped the client, so a
future change does not reintroduce it.

**Planned is a smart view, not a list.** Microsoft To Do's Planned view
aggregates every task that has a due date across all of the user's lists.
Graph exposes no aggregate endpoint for it (`GET /me/todo/tasks` returns
HTTP 400 on both v1.0 and beta), so the client walks each list:

1. `GET /me/todo/lists` — collect every list `id`, following
   `@odata.nextLink` if the list itself is paginated.
2. `GET /me/todo/lists/<id>/tasks?$filter=...&$top=8` — fetch the tasks for
   one list, following `@odata.nextLink` until that list is exhausted.

All requests share one `esp_http_client` handle with keep-alive enabled, so
the TLS handshake is paid once per fetch rather than once per list. The
response buffer is allocated once and reused across requests.

**Why `$top=8` and pagination.** Graph task JSON is verbose (~1.7 KB per
task: `recurrence`, `id`, `@odata.etag`, and timestamps), and `$select`
cannot trim it (see below). A single list can therefore exceed the 24 KB
response buffer. Pages of 8 tasks stay around 14–18 KB, and `@odata.nextLink`
walks the rest. `$batch` (one request for all lists) was tried and rejected:
Graph throttles the parallel sub-requests with HTTP 429.

**Bounds and resilience.** The walk is bounded so a large account cannot
stall the device: at most 32 lists, at most 4 pages per list (32 tasks per
list), and 64 tasks overall. A list that fails (deleted, transient 5xx/429,
HTTP 400, or a page too large for the buffer) is skipped and the tasks
already collected from other lists are kept. An authentication failure
(401/403) aborts the whole fetch, as does a transport failure or an
allocation failure on any request.

**`$select` is not used, on purpose.** Microsoft Graph's `$select`
parameter is documented as a way to trim the response, and the To Do
endpoints list it as supported. In practice, on a personal Microsoft
account (MSA), it does not work:

- `$select` with a real field name (for example `title`, or
  `id,wellknownListName`) returns **HTTP 400** with inner error
  `RequestBroker--ParseUri`. The client maps 400 to a generic network
  error, which is what produced the original "Could not fetch tasks".
- `$select` with a field name the broker happens to accept (for example
  `id` or `status`) returns **200 but is silently ignored** — every field
  is still returned, so the payload is not reduced at all.
- `$select` with an unknown field name returns a different 400
  (`BadRequest`, "Parsing OData Select and Expand failed").

The failure is deterministic (it reproduces on every request), and it is
independent of parameter order, comma encoding, request headers, and API
version (v1.0 and beta both fail). Microsoft's own documentation warns
that query parameters can "fail silently" and that the list-tasks method
supports only "some" of the OData query parameters.

**What the client does instead.** It filters server-side on the due date:

```
$filter=status ne 'completed'
        and dueDateTime/dateTime ge '1970-01-01'
        and dueDateTime/dateTime le '<today + 7 days>'
```

- `status ne 'completed'` keeps finished tasks out of the payload.
- `ge '1970-01-01'` excludes tasks with no due date. A null comparison
  (`dueDateTime/dateTime eq null`) is rejected with HTTP 400, so a very
  old lower bound is used instead.
- `le '<today + 7 days>'` bounds the window to overdue + today + the next
  seven days. The upper bound is computed on-device from the local clock.

This keeps each response small without `$select`, and — unlike a plain
`$top=64` — it does not hide overdue tasks that sit deep in a long list. The
today/overdue split is still computed on-device from each task's
`dueDateTime`.

---

## File reference

All MS To Do state lives under `/.crosspoint/` on the SD card.

| Path | Purpose |
|---|---|
| `/.crosspoint/mstodo.json` | Config and sign-in session (tokens). |
| `/.crosspoint/mstodo.json.tmp` | Atomic-write tmp file. Cleaned up on success or on the next sign-out. |
| `/.crosspoint/mstodo_sleep.bmp` | Last successful task-list render (sleep-screen source). |
| `/.crosspoint/mstodo_sleep.meta` | Capture time for the BMP (written for future use; not currently read). |
| `/.crosspoint/mstodo_sleep.meta.tmp` | Atomic-write tmp for the meta. |

---

## Troubleshooting

### Sign-in starts every time

No session is stored, or `mstodo.json` is malformed — the device treats a
broken config as "not signed in" and starts the device-code flow. Complete
the sign-in; the device rewrites the file on success.

### Sign-in code expired

The 15-minute window ran out before you approved the sign-in. Press
**Retry** to get a fresh code.

### "Sign-in declined"

The sign-in was rejected on the phone/PC side (you pressed Cancel, or the
wrong account was used). Press **Retry** and sign in with the intended
account.

### "Session expired - sign in again"

The stored session is no longer valid (revoked, or expired after ~90 days
of not using the integration). The activity restarts the sign-in flow
automatically; just complete it once.

### "WiFi unavailable"

WiFi failed to come up within the activity's timeout. Reconnect via
Settings → WiFi and retry.

### "Could not fetch tasks"

Generic HTTPS or parse failure. Check the serial monitor for the
underlying error:

```bash
pio device monitor
```

The most common one is `MBEDTLS_ERR_SSL_ALLOC_FAILED (-0x7F00)` —
mbedTLS couldn't claim the ~32 KB it needs for the handshake. Free heap
by closing other activities first; the home screen frees its cover buffer
when you leave it, so opening MS To Do from the home screen already
releases that memory.

### "Rate limited - try later"

Microsoft Graph answered 429. Wait a few minutes and press **Retry**.

### Sleep screen doesn't show my tasks

Check, in order:

1. `sleep_screen_enabled` is `true` in the config.
2. A successful fetch has happened since the last sign-out (the BMP
   exists at `/.crosspoint/mstodo_sleep.bmp`).
3. The Todoist sleep screen isn't also enabled — Todoist takes precedence.

The serial log prints which precondition failed when sleep is entered
(debug builds only — release builds compile out debug logging):

```
MSTD: Sleep screen skipped: sleep_screen_enabled=false in config
MSTD: Sleep screen skipped: /.crosspoint/mstodo_sleep.bmp missing
```

### Stale snapshot

The header on the activity reads `Updated HH:MM` so you can spot a stale
capture at a glance. Press **Confirm** to refresh; a fresh fetch
overwrites both the BMP and the meta.

---

## Privacy and data flow

- The device signs in with an OAuth device code. It never sees your
  Microsoft password — only task-scoped tokens (`Tasks.Read`,
  `offline_access`, `User.Read`).
- Task titles and due dates leave Microsoft Graph, hit the device over
  TLS, and are stored unencrypted on the SD card (in the BMP for the
  sleep screen, in heap-only structs while the activity is running).
  Session tokens are also stored unencrypted on the SD card.
- Nothing is uploaded anywhere else.
- **Sign out** removes the session tokens and every MS To Do-owned file
  from the SD card. WiFi credentials and unrelated settings are not
  touched.
- If you use the compiled-in default client ID, sign-ins are attributed
  to the fork's Azure app registration. Register your own app and set
  `client_id` in `mstodo.json` if you prefer full control.
