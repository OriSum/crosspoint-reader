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
- An SD card with the `/.crosspoint/` directory writable. The firmware
  creates the directory if it doesn't exist; nothing else is required.

No configuration file is required for a first run — the sign-in flow starts
automatically the first time you open the activity.

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
| `list_id` | string | (managed) | Resolved ID of your default To Do list. Don't edit. |
| `sleep_screen_enabled` | bool | `false` | When `true` and a snapshot exists, the sleep screen renders the cached task list instead of the default rotation. |

The file is bounded at 4 KB — parsing aborts and the integration disables
itself if the file is larger. Unknown keys are ignored on load and dropped
on the next persist.

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
   brings up WiFi, syncs NTP, and fetches your tasks. Expect 3–10 seconds
   before the list appears — a little longer than Todoist because the
   device may also refresh its token.
3. Press **Confirm** to refresh, **Up/Down** to scroll, **Back** to exit.

The list shows tasks due today plus overdue ones from your default To Do
list. Completed tasks are never shown.

After a successful fetch with `sleep_screen_enabled: true`, the device
also writes:

- `/.crosspoint/mstodo_sleep.bmp` — the rendered task list as a bitmap
- `/.crosspoint/mstodo_sleep.meta` — JSON sidecar with capture time and
  orientation

When the device sleeps, `SleepActivity` checks for these files and, if
they exist, renders the cached BMP as the sleep wallpaper.

---

## 5. Configure on device

**Settings → MS To Do** exposes the persisted settings, each with atomic
write-through to the file:

| Row | What it does |
|---|---|
| Sleep screen | Toggles `sleep_screen_enabled`. |
| Sign out | Removes the stored session, the snapshot BMP and meta, and any orphaned `.tmp` files. Your sleep-screen toggle is kept. **No confirmation prompt — single press signs you out.** |

The next time you open the activity after signing out, the sign-in flow
starts again.

If both the Todoist and MS To Do sleep screens are enabled, **Todoist
wins** — its snapshot is checked first. Turn one of them off if you want
the other to show.

---

## What the activity shows

A single fixed view: an "MS To Do" header with an `Updated HH:MM` stamp
from the last successful fetch, and the task list below it. The query is
fixed — today's tasks plus overdue ones from the default list — so there
are no filter, timezone, or layout settings to manage. Each row shows a
marker glyph (bullet for today, `!` for overdue, `›` for future-dated),
the task title, and a dd/mm date suffix when the task carries a due date.

---

## File reference

All MS To Do state lives under `/.crosspoint/` on the SD card.

| Path | Purpose |
|---|---|
| `/.crosspoint/mstodo.json` | Config and sign-in session (tokens). |
| `/.crosspoint/mstodo.json.tmp` | Atomic-write tmp file. Cleaned up on success or on the next sign-out. |
| `/.crosspoint/mstodo_sleep.bmp` | Last successful task-list render (sleep-screen source). |
| `/.crosspoint/mstodo_sleep.meta` | Capture time for the BMP. |
| `/.crosspoint/mstodo_sleep.meta.tmp` | Atomic-write tmp for the meta. |

---

## Troubleshooting

### "Not signed in"

No session is stored yet, or the config file is malformed. Open the
activity and complete the device-code sign-in.

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
by closing other activities first; the integration already releases the
home-screen cover buffer before connecting.

### "Rate limited - try later"

Microsoft Graph answered 429. Wait a few minutes and press **Retry**.

### Tasks from a deleted list

If you deleted your default To Do list in the Microsoft To Do app, the
next fetch re-resolves it automatically. If the error persists, sign out
and sign in again.

### Sleep screen doesn't show my tasks

Check, in order:

1. `sleep_screen_enabled` is `true` in the config.
2. A successful fetch has happened since the last sign-out (the BMP
   exists at `/.crosspoint/mstodo_sleep.bmp`).
3. The Todoist sleep screen isn't also enabled — Todoist takes precedence.

The serial log prints which precondition failed when sleep is entered:

```
MSTD: tryRenderMsTodoSleepScreen: <reason>
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
