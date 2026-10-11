#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "integrations/mstodo/MsTodoAuth.h"
#include "integrations/mstodo/MsTodoClient.h"
#include "integrations/mstodo/MsTodoTask.h"
#include "util/ButtonNavigator.h"

class MsTodoActivity : public Activity {
 public:
  MsTodoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("MsTodo", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  enum class State {
    Loading,        // initial fetch in progress / awaiting WiFi
    SignInWaiting,  // showing the device code, polling for approval
    ShowingTasks,
    ShowingError,
  };

  // Step 1 of fetch: ensure WiFi. If already connected, calls
  // proceedWithFetch() directly. Otherwise launches WifiSelectionActivity
  // and re-enters proceedWithFetch() on its result.
  void startFetch();

  // Step 2 of fetch: time sync + token refresh + MsTodoClient::fetchToday.
  // Called once WiFi is up.
  void proceedWithFetch();

  // Brings WiFi up and starts the device-code flow. Called when no session
  // is stored, or when a refresh is rejected.
  void beginSignIn();

  // One poll tick of the device-code flow. Called from loop() while in
  // SignInWaiting.
  void pollSignIn();

  // Ensures the clock is trustworthy for TLS and date math. Prefers the
  // hardware RTC; falls back to NTP (with a build-date fallback) when absent.
  void ensureTimeSynced();

  // Maps a FetchResult to the corresponding StrId for an error message.
  StrId fetchResultToStrId(mstodo::FetchResult r) const;

  void renderLoading();
  void renderSignIn(bool waiting);
  void renderError();
  // When drawHints is false, the bottom hint bar is omitted and the list
  // expands into that space. Used by the sleep-screen snapshot, which has
  // no buttons and shouldn't waste pixels on hints.
  void renderTaskList(bool drawHints = true);
  // Shared task-list body. The caller passes the vertical band (top,
  // height) and horizontal band (tileX, tileWidth) already adjusted for the
  // header. Returns nothing — updates _lastVisibleIndex as a side effect.
  void drawTaskRows(int contentTop, int contentHeight, int tileX, int tileWidth, bool drawHints, int pageWidth);

  void captureSnapshotIfNeeded();
  bool writeSnapshotMeta();

  std::vector<mstodo::MsTodoTask> _tasks;
  State _state = State::Loading;
  // First task index in the visible window. Advances only when the cursor
  // crosses out of the window — i.e. window is cursor-driven, not free-scroll.
  int _scrollOffset = 0;
  // Highlighted task. Independent of _scrollOffset so the user can move the
  // cursor through the visible band without scrolling.
  int _selectedIndex = 0;
  // Index of the last task that fully rendered in the previous frame. Used by
  // the navigator to decide whether moving the cursor forward also has to
  // advance _scrollOffset. Updated at the end of each renderTaskList().
  int _lastVisibleIndex = -1;
  StrId _errorStrId = StrId::STR_MSTODO_FETCH_FAILED;
  uint8_t _capturedHour = 0;
  uint8_t _capturedMin = 0;
  // "YYYY-MM-DD" snapshot of today's date at fetch time. Used to classify
  // tasks as future (dueDate strictly greater than this) so they render
  // with a distinct glyph and a date suffix.
  char _today[11] = "";

  // Device-code flow state.
  mstodo::DeviceCodeInfo _deviceCode;
  // Epoch (seconds) at which the device code expires; 0 = no active poll.
  long _pollDeadlineEpoch = 0;
  // Current poll interval in seconds; grows on slow_down.
  int _pollIntervalSec = 5;
  // millis() at which the next poll tick is due.
  unsigned long _nextPollMs = 0;

  // Snapshot of renderer orientation at onEnter() so we can restore it on
  // exit. The activity forces portrait, so this is always the pre-entry value.
  GfxRenderer::Orientation _entryOrientation = GfxRenderer::Orientation::Portrait;
  ButtonNavigator _navigator;
};
