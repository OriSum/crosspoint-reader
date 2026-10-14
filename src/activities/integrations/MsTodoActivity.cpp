#include "MsTodoActivity.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "Logging.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "integrations/mstodo/MsTodoConfig.h"
#include "util/QrUtils.h"
#include "util/ScreenshotUtil.h"
#include "util/Timezones.h"

namespace {

constexpr const char* kSnapshotBmpPath = "/.crosspoint/mstodo_sleep.bmp";
constexpr const char* kSnapshotMetaPath = "/.crosspoint/mstodo_sleep.meta";
constexpr const char* kSnapshotMetaTmpPath = "/.crosspoint/mstodo_sleep.meta.tmp";

// Fixed verification URI. Microsoft's devicelogin page has no pre-filled-code
// parameter, so the QR encodes the URL only and the user types the code.
constexpr const char* kVerificationUri = "https://microsoft.com/devicelogin";
constexpr int kQrSize = 198;  // matches CrossPointWebServerActivity

// Pin the clock to UTC for the fetch path. There is no user timezone setting
// in the v1 config, so "today" resolves against UTC — the device's own
// timezone setting continues to govern everything outside this activity.
void applyTimezone() {
  setenv("TZ", "UTC", 1);
  tzset();
}

// Try NTP first. If it times out, set the clock to the firmware's build date
// so TLS cert validation can still succeed — mbedTLS rejects certs whose
// notBefore lies in the future relative to the device clock.
void syncTimeWithNTP() {
  if (esp_sntp_enabled()) esp_sntp_stop();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  int retry = 0;
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < 50) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    ++retry;
  }
  if (retry < 50) {
    LOG_DBG("MSTD", "NTP synced");
    return;
  }

  LOG_DBG("MSTD", "NTP timeout, using build date for TLS validation");
  struct tm tm = {};
  if (strptime(__DATE__ " " __TIME__, "%b %d %Y %H:%M:%S", &tm) != nullptr) {
    time_t t = mktime(&tm);
    struct timeval tv = {.tv_sec = t, .tv_usec = 0};
    settimeofday(&tv, nullptr);
  } else {
    LOG_ERR("MSTD", "Build date parse failed; clock unset");
  }
}

void wifiOff() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

}  // namespace

void MsTodoActivity::onEnter() {
  Activity::onEnter();
  _entryOrientation = renderer.getOrientation();
  // Always portrait: the activity and the sleep snapshot share one orientation.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  _state = State::Loading;
  _scrollOffset = 0;
  _selectedIndex = 0;
  _lastVisibleIndex = -1;
  _tasks.clear();

  requestUpdate(true);  // paint Loading screen
  startFetch();
}

void MsTodoActivity::onExit() {
  wifiOff();
  // Restore orientation so HomeActivity (and its cached coverBuffer) are
  // displayed in the orientation they were rendered in.
  renderer.setOrientation(_entryOrientation);
  Activity::onExit();
}

void MsTodoActivity::loop() {
  // Don't call mappedInput.update() here — main loop already calls gpio.update()
  // before dispatching to the activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (_state == State::SignInWaiting) {
    if (static_cast<long>(time(nullptr)) >= _pollDeadlineEpoch) {
      _state = State::ShowingError;
      _errorStrId = StrId::STR_MSTODO_SIGNIN_EXPIRED;
      requestUpdate(true);
      return;
    }
    if (millis() >= _nextPollMs) {
      pollSignIn();
    }
    return;
  }

  // Confirm = Refresh (works in ShowingTasks and ShowingError).
  if ((_state == State::ShowingTasks || _state == State::ShowingError) &&
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    _state = State::Loading;
    _scrollOffset = 0;
    _tasks.clear();
    requestUpdate(true);
    startFetch();
    return;
  }

  // Cursor navigation. The visible window only shifts when the cursor would
  // otherwise leave it.
  _navigator.onNext([this] {
    if (_state != State::ShowingTasks) return;
    if (_selectedIndex + 1 >= static_cast<int>(_tasks.size())) return;
    ++_selectedIndex;
    if (_lastVisibleIndex >= 0 && _selectedIndex > _lastVisibleIndex) {
      ++_scrollOffset;
    }
    requestUpdate();
  });
  _navigator.onPrevious([this] {
    if (_state != State::ShowingTasks) return;
    if (_selectedIndex == 0) return;
    --_selectedIndex;
    if (_selectedIndex < _scrollOffset) {
      _scrollOffset = _selectedIndex;
    }
    requestUpdate();
  });
}

void MsTodoActivity::startFetch() {
  if (!MSTODO_CONFIG.hasTokens()) {
    LOG_DBG("MSTD", "No session; starting sign-in");
    beginSignIn();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("MSTD", "Already connected to WiFi");
    proceedWithFetch();
    return;
  }

  LOG_DBG("MSTD", "Launching WifiSelectionActivity");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             _state = State::ShowingError;
                             _errorStrId = StrId::STR_MSTODO_OFFLINE;
                             requestUpdate();
                             return;
                           }
                           renderer.setOrientation(GfxRenderer::Orientation::Portrait);
                           proceedWithFetch();
                         });
}

void MsTodoActivity::ensureTimeSynced() {
  // Prefer the hardware RTC: the SDK's Rtc::now() rejects readings with the VL
  // flag set, so a successful read means the RTC holds a real time. This saves
  // the 1-2 s NTP wait on the common path (WifiSelectionActivity already
  // NTP-syncs the RTC on first connect).
  struct tm rtcTm = {};
  if (halClock.isAvailable() && halClock.localTime(rtcTm)) {
    // Reading the RTC does not set the system clock, which is what TLS cert
    // validation and the date math actually use. Push it in, so a device whose
    // NVS time floor was lost still has a usable clock. TZ is UTC here, so
    // mktime() yields the UTC epoch.
    const time_t rtcEpoch = mktime(&rtcTm);
    if (rtcEpoch > 0) {
      struct timeval tv = {.tv_sec = rtcEpoch, .tv_usec = 0};
      settimeofday(&tv, nullptr);
      LOG_DBG("MSTD", "System clock set from hardware RTC");
      return;
    }
  }
  syncTimeWithNTP();
}

void MsTodoActivity::beginSignIn() {
  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                               _state = State::ShowingError;
                               _errorStrId = StrId::STR_MSTODO_OFFLINE;
                               requestUpdate();
                               return;
                             }
                             renderer.setOrientation(GfxRenderer::Orientation::Portrait);
                             beginSignIn();
                           });
    return;
  }

  applyTimezone();
  ensureTimeSynced();

  using mstodo::AuthResult;
  AuthResult r = mstodo::MsTodoAuth::requestDeviceCode(MSTODO_CONFIG.getClientId(), _deviceCode);
  if (r != AuthResult::Ok) {
    _state = State::ShowingError;
    _errorStrId = StrId::STR_MSTODO_SIGNIN_FAILED;
    requestUpdate(true);
    return;
  }

  _pollIntervalSec = _deviceCode.intervalSeconds > 0 ? _deviceCode.intervalSeconds : 5;
  _pollDeadlineEpoch = static_cast<long>(time(nullptr)) + _deviceCode.expiresInSeconds;
  _nextPollMs = millis() + static_cast<unsigned long>(_pollIntervalSec) * 1000UL;
  _state = State::SignInWaiting;
  requestUpdate(true);
}

void MsTodoActivity::pollSignIn() {
  using mstodo::AuthResult;
  std::string access;
  std::string refresh;
  long expiresIn = 0;
  AuthResult r =
      mstodo::MsTodoAuth::pollToken(MSTODO_CONFIG.getClientId(), _deviceCode.deviceCode, access, refresh, expiresIn);

  switch (r) {
    case AuthResult::Pending:
      _nextPollMs = millis() + static_cast<unsigned long>(_pollIntervalSec) * 1000UL;
      return;
    case AuthResult::SlowDown:
      _pollIntervalSec += 5;
      _nextPollMs = millis() + static_cast<unsigned long>(_pollIntervalSec) * 1000UL;
      return;
    case AuthResult::Ok: {
      const long expiry = static_cast<long>(time(nullptr)) + expiresIn;
      MSTODO_CONFIG.setTokens(access, refresh, expiry);
      LOG_DBG("MSTD", "Sign-in complete");
      proceedWithFetch();
      return;
    }
    case AuthResult::Expired:
      _state = State::ShowingError;
      _errorStrId = StrId::STR_MSTODO_SIGNIN_EXPIRED;
      requestUpdate(true);
      return;
    case AuthResult::Declined:
      _state = State::ShowingError;
      _errorStrId = StrId::STR_MSTODO_SIGNIN_DECLINED;
      requestUpdate(true);
      return;
    default:
      _state = State::ShowingError;
      _errorStrId = StrId::STR_MSTODO_SIGNIN_FAILED;
      requestUpdate(true);
      return;
  }
}

void MsTodoActivity::proceedWithFetch() {
  applyTimezone();
  ensureTimeSynced();

  using mstodo::AuthResult;
  using mstodo::FetchResult;

  // Refresh the access token if it is expired (or the clock is untrusted).
  if (MSTODO_CONFIG.isAccessTokenExpired()) {
    std::string access;
    std::string refresh;
    long expiresIn = 0;
    AuthResult ar = mstodo::MsTodoAuth::refreshTokens(MSTODO_CONFIG.getClientId(), MSTODO_CONFIG.getRefreshToken(),
                                                      access, refresh, expiresIn);
    if (ar == AuthResult::InvalidGrant) {
      LOG_DBG("MSTD", "Refresh rejected; restarting sign-in");
      MSTODO_CONFIG.signOut();
      beginSignIn();
      return;
    }
    if (ar != AuthResult::Ok) {
      _state = State::ShowingError;
      _errorStrId = StrId::STR_MSTODO_OFFLINE;
      requestUpdate(true);
      return;
    }
    const long expiry = static_cast<long>(time(nullptr)) + expiresIn;
    MSTODO_CONFIG.setTokens(access, refresh, expiry);
  }

  // Get the device's configured timezone for converting UTC datetimes to local time
  const char* posixTz = timezones::table()[timezones::activeIndex()].posixTz;

  mstodo::FetchOptions options;
  options.showOverdue = MSTODO_CONFIG.isShowOverdue();
  options.futureDays = MSTODO_CONFIG.getFutureDays();
  FetchResult r = mstodo::MsTodoClient::fetchPlanned(MSTODO_CONFIG.getAccessToken(), options, _tasks, posixTz);

  if (r == FetchResult::Ok) {
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    _capturedHour = static_cast<uint8_t>(tm_now.tm_hour);
    _capturedMin = static_cast<uint8_t>(tm_now.tm_min);
    strftime(_today, sizeof(_today), "%Y-%m-%d", &tm_now);

    // Chronological sort: oldest overdue first, then today's timed tasks in
    // ascending time, then today's untimed tasks last.
    std::sort(_tasks.begin(), _tasks.end(), [](const mstodo::MsTodoTask& a, const mstodo::MsTodoTask& b) {
      int dateCmp = strcmp(a.dueDate, b.dueDate);
      if (dateCmp != 0) return dateCmp < 0;
      bool aTimed = a.dueTime[0] != '\0';
      bool bTimed = b.dueTime[0] != '\0';
      if (aTimed != bTimed) return aTimed;  // timed first
      if (aTimed) return strcmp(a.dueTime, b.dueTime) < 0;
      return false;  // both untimed: stable order
    });

    _state = State::ShowingTasks;
    _scrollOffset = 0;
    _selectedIndex = 0;
    _lastVisibleIndex = -1;
    captureSnapshotIfNeeded();
  } else if (r == FetchResult::InvalidToken) {
    // Graph rejected the token even though the clock said it was valid
    // (revoked, or a clock skew the refresh could not fix). Clear the session
    // and re-run sign-in rather than dead-ending on an error screen.
    LOG_DBG("MSTD", "Token rejected by Graph; restarting sign-in");
    MSTODO_CONFIG.signOut();
    beginSignIn();
    return;
  } else {
    _state = State::ShowingError;
    _errorStrId = fetchResultToStrId(r);
  }
  requestUpdate(true);
}

void MsTodoActivity::captureSnapshotIfNeeded() {
  if (_state != State::ShowingTasks) return;

  // Always portrait, so the snapshot matches the activity orientation — no
  // render-twice trick and no orientation gate.
  renderTaskList(/*drawHints=*/false);
  if (!ScreenshotUtil::saveFramebufferAsBmpOriented(kSnapshotBmpPath, renderer.getFrameBuffer(),
                                                    renderer.getDisplayWidth(), renderer.getDisplayHeight(),
                                                    GfxRenderer::Orientation::Portrait)) {
    LOG_ERR("MSTD", "Snapshot save failed");
  } else {
    LOG_DBG("MSTD", "Snapshot saved to %s", kSnapshotBmpPath);
    writeSnapshotMeta();
  }
}

bool MsTodoActivity::writeSnapshotMeta() {
  char buf[96];
  int len = snprintf(buf, sizeof(buf), "{\"captured_hour\":%u,\"captured_min\":%u}",
                     static_cast<unsigned>(_capturedHour), static_cast<unsigned>(_capturedMin));
  if (len <= 0 || len >= static_cast<int>(sizeof(buf))) {
    LOG_ERR("MSTD", "Meta format failed");
    return false;
  }

  if (Storage.exists(kSnapshotMetaTmpPath)) Storage.remove(kSnapshotMetaTmpPath);

  HalFile file;
  if (!Storage.openFileForWrite("MSTD", kSnapshotMetaTmpPath, file)) {
    LOG_ERR("MSTD", "Cannot open meta tmp");
    return false;
  }
  size_t n = file.write(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(len));
  file.close();
  if (n != static_cast<size_t>(len)) {
    LOG_ERR("MSTD", "Short meta write");
    Storage.remove(kSnapshotMetaTmpPath);
    return false;
  }

  if (Storage.exists(kSnapshotMetaPath)) Storage.remove(kSnapshotMetaPath);
  if (!Storage.rename(kSnapshotMetaTmpPath, kSnapshotMetaPath)) {
    LOG_ERR("MSTD", "Meta rename failed");
    Storage.remove(kSnapshotMetaTmpPath);
    return false;
  }
  return true;
}

void MsTodoActivity::render(RenderLock&&) {
  switch (_state) {
    case State::Loading:
      renderLoading();
      break;
    case State::SignInWaiting:
      renderSignIn(true);
      break;
    case State::ShowingError:
      renderError();
      break;
    case State::ShowingTasks:
      renderTaskList();
      break;
  }
  renderer.displayBuffer();
}

void MsTodoActivity::renderLoading() {
  renderer.clearScreen();
  GUI.drawPopup(renderer, tr(STR_MSTODO_FETCHING));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void MsTodoActivity::renderSignIn(bool waiting) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MSTODO_SIGNIN_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const int sidePadding = metrics.contentSidePadding;

  // QR on the left, instructions + code on the right.
  const Rect qrBounds(sidePadding, contentTop, kQrSize, kQrSize);
  QrUtils::drawQrCode(renderer, qrBounds, kVerificationUri);

  const int textX = sidePadding + kQrSize + metrics.verticalSpacing * 2;
  const int textWidth = pageWidth - sidePadding - textX;
  int y = contentTop;

  auto bodyLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_MSTODO_SIGNIN_BODY), textWidth, 4);
  for (const auto& line : bodyLines) {
    renderer.drawText(UI_10_FONT_ID, textX, y, line.c_str(), true);
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }

  y += metrics.verticalSpacing * 2;
  renderer.drawText(UI_12_FONT_ID, textX, y, _deviceCode.userCode, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing * 2;

  auto scanLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_MSTODO_SIGNIN_SCAN), textWidth, 3);
  for (const auto& line : scanLines) {
    renderer.drawText(UI_10_FONT_ID, textX, y, line.c_str(), true);
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }

  if (waiting) {
    y += metrics.verticalSpacing * 2;
    auto waitLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_MSTODO_SIGNIN_WAITING), textWidth, 3);
    for (const auto& line : waitLines) {
      renderer.drawText(UI_10_FONT_ID, textX, y, line.c_str(), true);
      y += renderer.getLineHeight(UI_10_FONT_ID);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void MsTodoActivity::renderError() {
  renderer.clearScreen();
  GUI.drawPopup(renderer, I18N.get(_errorStrId));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void MsTodoActivity::renderTaskList(bool drawHints) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Portrait only, so the hint strip always lands at the logical bottom.
  const int hintReserve = drawHints ? metrics.buttonHintsHeight + metrics.verticalSpacing * 2 : 0;

  char timestamp[32];
  snprintf(timestamp, sizeof(timestamp), I18N.get(StrId::STR_MSTODO_TODAY_HEADER), _capturedHour, _capturedMin);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MSTODO), timestamp);

  if (!drawHints) {
    // Snapshot mode — paint over the battery icon + percentage text drawn by
    // drawHeader. Battery on a sleep screen is misleading.
    constexpr int kBatteryRegionWidth = 80;
    renderer.fillRect(pageWidth - kBatteryRegionWidth, metrics.topPadding + 5, kBatteryRegionWidth,
                      metrics.batteryHeight + 10, false);
  }

  if (drawHints) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (_tasks.empty()) {
    GUI.drawPopup(renderer, tr(STR_MSTODO_NO_TASKS));
    return;
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - hintReserve;

  const int sidePadding = metrics.contentSidePadding;
  const int tileX = sidePadding;
  const int tileWidth = pageWidth - sidePadding * 2;
  drawTaskRows(contentTop, contentHeight, tileX, tileWidth, drawHints, pageWidth);
}

void MsTodoActivity::drawTaskRows(int contentTop, int contentHeight, int tileX, int tileWidth, bool drawHints,
                                  int pageWidth) {
  constexpr const char* kBulletNormal = "\xE2\x80\xA2";  // U+2022 BULLET — today / undated
  constexpr const char* kBulletOverdue = "!";            // overdue marker
  constexpr const char* kBulletFuture = "\xE2\x80\xBA";  // U+203A SINGLE RIGHT-POINTING ANGLE QUOTATION MARK
  constexpr int kTilePaddingX = 10;
  constexpr int kTilePaddingY = 6;
  constexpr int kBulletGap = 8;
  constexpr int kRowGap = 2;
  constexpr int kCursorRadius = 6;
  constexpr int kDateGap = 8;
  constexpr int kMaxLines = 2;

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int bulletColWidth = std::max({renderer.getTextWidth(UI_10_FONT_ID, kBulletNormal),
                                       renderer.getTextWidth(UI_10_FONT_ID, kBulletOverdue),
                                       renderer.getTextWidth(UI_10_FONT_ID, kBulletFuture)});
  const int dateColWidth = renderer.getTextWidth(UI_10_FONT_ID, "00/00");

  const int textX = tileX + kTilePaddingX + bulletColWidth + kBulletGap;
  const int textWidth = tileX + tileWidth - kTilePaddingX - textX;

  const int totalTasks = static_cast<int>(_tasks.size());
  if (_scrollOffset > totalTasks - 1) _scrollOffset = std::max(0, totalTasks - 1);
  if (_selectedIndex > totalTasks - 1) _selectedIndex = std::max(0, totalTasks - 1);

  int y = contentTop;
  int rendered = 0;
  int lastFullyVisible = -1;
  for (int taskIdx = _scrollOffset; taskIdx < totalTasks; ++taskIdx) {
    const auto& t = _tasks[taskIdx];

    const bool isFuture = _today[0] != '\0' && t.dueDate[0] != '\0' && strcmp(t.dueDate, _today) > 0;
    const bool showDate = t.dueDate[0] != '\0';

    char fullTitle[128];
    snprintf(fullTitle, sizeof(fullTitle), "%s%s%s", t.dueTime[0] ? t.dueTime : "", t.dueTime[0] ? "  " : "", t.title);

    const int rowTextWidth = showDate ? textWidth - dateColWidth - kDateGap : textWidth;
    auto lines = renderer.wrappedText(UI_10_FONT_ID, fullTitle, rowTextWidth, kMaxLines);
    const int textBlockHeight = static_cast<int>(lines.size()) * lineHeight;
    const int tileHeight = textBlockHeight + kTilePaddingY * 2;
    if (y + tileHeight > contentTop + contentHeight) break;  // would clip

    const bool selected = drawHints && (taskIdx == _selectedIndex);
    if (selected) {
      renderer.fillRoundedRect(tileX, y, tileWidth, tileHeight, kCursorRadius, Color::LightGray);
    }

    const char* marker = t.overdue ? kBulletOverdue : isFuture ? kBulletFuture : kBulletNormal;
    const int markerX = tileX + kTilePaddingX;
    const int firstLineY = y + kTilePaddingY;
    renderer.drawText(UI_10_FONT_ID, markerX, firstLineY, marker, true);

    for (size_t li = 0; li < lines.size(); ++li) {
      renderer.drawText(UI_10_FONT_ID, textX, firstLineY + static_cast<int>(li) * lineHeight, lines[li].c_str(), true);
    }

    if (showDate) {
      const int day = (t.dueDate[8] - '0') * 10 + (t.dueDate[9] - '0');
      const int mon = (t.dueDate[5] - '0') * 10 + (t.dueDate[6] - '0');
      char dateBuf[8];
      snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d", day, mon);
      const int dateWidth = renderer.getTextWidth(UI_10_FONT_ID, dateBuf);
      const int dateX = tileX + tileWidth - kTilePaddingX - dateWidth;
      renderer.drawText(UI_10_FONT_ID, dateX, firstLineY, dateBuf, true);
    }

    y += tileHeight + kRowGap;
    rendered++;
    lastFullyVisible = taskIdx;
  }
  _lastVisibleIndex = lastFullyVisible;

  if (rendered < totalTasks - _scrollOffset || _scrollOffset > 0) {
    const int barX = pageWidth - 6;
    const int barTrackHeight = contentHeight;
    const int barHeight = std::max(8, (barTrackHeight * rendered) / totalTasks);
    const int maxOffset = std::max(1, totalTasks - rendered);
    const int barY = contentTop + ((barTrackHeight - barHeight) * _scrollOffset) / maxOffset;
    renderer.fillRect(barX, barY, 2, barHeight, true);
  }
}

StrId MsTodoActivity::fetchResultToStrId(mstodo::FetchResult r) const {
  switch (r) {
    case mstodo::FetchResult::InvalidToken:
      return StrId::STR_MSTODO_INVALID_TOKEN;
    case mstodo::FetchResult::RateLimited:
      return StrId::STR_MSTODO_RATE_LIMITED;
    case mstodo::FetchResult::Ok:
    case mstodo::FetchResult::ServerError:
    case mstodo::FetchResult::NetworkError:
    case mstodo::FetchResult::ParseError:
    default:
      return StrId::STR_MSTODO_FETCH_FAILED;
  }
}
