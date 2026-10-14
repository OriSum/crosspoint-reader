#include "MsTodoConfig.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <TrustedTime.h>

namespace mstodo {

namespace {

constexpr const char* kConfigPath = "/.crosspoint/mstodo.json";
constexpr const char* kConfigTmpPath = "/.crosspoint/mstodo.json.tmp";
constexpr const char* kSnapshotBmpPath = "/.crosspoint/mstodo_sleep.bmp";
constexpr const char* kSnapshotMetaPath = "/.crosspoint/mstodo_sleep.meta";
constexpr const char* kSnapshotMetaTmpPath = "/.crosspoint/mstodo_sleep.meta.tmp";
constexpr const char* kLogPath = "/.crosspoint/mstodo.log";

// Public-client Azure app registration owned by the fork, with "Allow public
// client flows" enabled. Not a secret; users may override it via "client_id"
// in the config.
constexpr const char* kDefaultClientId = "23db5763-8f56-47b7-83ed-83e808e77c71";

// Refresh this long before the token actually expires, to absorb clock skew
// between the device and Microsoft's token service.
constexpr long kExpirySafetyMarginSeconds = 60;

}  // namespace

MsTodoConfig& MsTodoConfig::getInstance() {
  static MsTodoConfig instance;
  return instance;
}

bool MsTodoConfig::load() {
  accessToken.clear();
  refreshToken.clear();
  tokenExpiryEpoch = 0;
  clientId.clear();
  sleepScreenEnabled = false;
  showOverdue = true;
  futureDays = kDefaultFutureDays;

  if (!Storage.exists(kConfigPath)) {
    LOG_DBG("MSTD", "No config at %s", kConfigPath);
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("MSTD", kConfigPath, file)) {
    LOG_ERR("MSTD", "Cannot open config for read");
    return false;
  }

  // Bounded read: config files are small. Anything past 4 KB is suspect.
  constexpr size_t kMaxConfigBytes = 4096;
  std::string buffer;
  buffer.reserve(kMaxConfigBytes);
  uint8_t chunk[256];
  while (true) {
    int n = file.read(chunk, sizeof(chunk));
    if (n <= 0) break;
    if (buffer.size() + static_cast<size_t>(n) > kMaxConfigBytes) {
      LOG_ERR("MSTD", "Config exceeds %u bytes", static_cast<unsigned>(kMaxConfigBytes));
      return false;
    }
    buffer.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n));
  }

  JsonDocument doc;
  auto err = deserializeJson(doc, buffer);
  if (err) {
    LOG_ERR("MSTD", "JSON parse error: %s", err.c_str());
    return false;
  }

  accessToken = doc["access_token"] | std::string("");
  refreshToken = doc["refresh_token"] | std::string("");
  tokenExpiryEpoch = doc["token_expiry_epoch"] | 0L;
  clientId = doc["client_id"] | std::string("");
  sleepScreenEnabled = doc["sleep_screen_enabled"] | false;
  showOverdue = doc["show_overdue"] | true;
  // Clamp rather than reject: a hand-edited out-of-range value must not break
  // the fetch.
  futureDays = doc["future_days"] | kDefaultFutureDays;
  if (futureDays < kMinFutureDays) futureDays = kMinFutureDays;
  if (futureDays > kMaxFutureDays) futureDays = kMaxFutureDays;

  // Never log token values, not even truncated.
  LOG_DBG("MSTD", "Config loaded (access=%u chars, refresh=%u chars, expiry=%ld, sleep=%d, overdue=%d, days=%d)",
          static_cast<unsigned>(accessToken.size()), static_cast<unsigned>(refreshToken.size()), tokenExpiryEpoch,
          sleepScreenEnabled, showOverdue, futureDays);
  return true;
}

std::string MsTodoConfig::getClientId() const { return clientId.empty() ? std::string(kDefaultClientId) : clientId; }

bool MsTodoConfig::hasTokens() const { return !accessToken.empty() && !refreshToken.empty(); }

bool MsTodoConfig::isAccessTokenExpired() const {
  if (tokenExpiryEpoch <= 0) return true;
  // Fail closed when the clock is not trustworthy: a stale floor would make an
  // expired token look valid, and a refresh is cheaper than a failed fetch.
  const int64_t now = trustedtime::trustedNow();
  if (now == 0) return true;
  return now >= tokenExpiryEpoch - kExpirySafetyMarginSeconds;
}

bool MsTodoConfig::setTokens(const std::string& access, const std::string& refresh, long expiryEpoch) {
  if (access == accessToken && refresh == refreshToken && expiryEpoch == tokenExpiryEpoch) return true;
  accessToken = access;
  refreshToken = refresh;
  tokenExpiryEpoch = expiryEpoch;
  return persist();
}

bool MsTodoConfig::setSleepScreenEnabled(bool enabled) {
  if (enabled == sleepScreenEnabled) return true;
  sleepScreenEnabled = enabled;
  return persist();
}

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

bool MsTodoConfig::signOut() {
  // Persist the cleared session first: if the write fails, the in-memory
  // session must stay consistent with the file on disk, so the user is not
  // silently signed back in on the next boot.
  const std::string savedAccess = accessToken;
  const std::string savedRefresh = refreshToken;
  const long savedExpiry = tokenExpiryEpoch;

  accessToken.clear();
  refreshToken.clear();
  tokenExpiryEpoch = 0;

  if (!persist()) {
    accessToken = savedAccess;
    refreshToken = savedRefresh;
    tokenExpiryEpoch = savedExpiry;
    return false;
  }

  bool ok = true;
  if (Storage.exists(kSnapshotBmpPath) && !Storage.remove(kSnapshotBmpPath)) {
    LOG_ERR("MSTD", "Could not remove %s", kSnapshotBmpPath);
    ok = false;
  }
  if (Storage.exists(kSnapshotMetaPath) && !Storage.remove(kSnapshotMetaPath)) {
    LOG_ERR("MSTD", "Could not remove %s", kSnapshotMetaPath);
    ok = false;
  }
  // Also clear the log file on sign out
  clearLog();
  // Best-effort cleanup of orphaned tmp files from any prior failed write.
  Storage.remove(kConfigTmpPath);
  Storage.remove(kSnapshotMetaTmpPath);

  // Display preferences are intentionally kept so re-signing-in does not reset
  // them: sleepScreenEnabled, showOverdue, futureDays.
  return ok;
}

bool MsTodoConfig::persist() {
  JsonDocument doc;
  doc["access_token"] = accessToken;
  doc["refresh_token"] = refreshToken;
  doc["token_expiry_epoch"] = tokenExpiryEpoch;
  doc["client_id"] = clientId;
  doc["sleep_screen_enabled"] = sleepScreenEnabled;
  doc["show_overdue"] = showOverdue;
  doc["future_days"] = futureDays;

  // Atomic write: serialize to .tmp, close, rename to final path.
  if (Storage.exists(kConfigTmpPath)) {
    Storage.remove(kConfigTmpPath);
  }

  std::string out;
  serializeJson(doc, out);

  // Scoped: HalFile destructor closes the underlying SdFat file before the
  // rename below — SdFat behaviour against an open handle is implementation-
  // defined, so we don't rely on it.
  size_t written = 0;
  {
    HalFile file;
    if (!Storage.openFileForWrite("MSTD", kConfigTmpPath, file)) {
      LOG_ERR("MSTD", "Cannot open tmp for write");
      return false;
    }
    written = file.write(reinterpret_cast<const uint8_t*>(out.data()), out.size());
  }

  if (written != out.size()) {
    LOG_ERR("MSTD", "Short write: %u/%u", static_cast<unsigned>(written), static_cast<unsigned>(out.size()));
    Storage.remove(kConfigTmpPath);
    return false;
  }

  if (!Storage.replaceFile(kConfigTmpPath, kConfigPath)) {
    LOG_ERR("MSTD", "Replace %s -> %s failed", kConfigTmpPath, kConfigPath);
    Storage.remove(kConfigTmpPath);
    return false;
  }

  LOG_DBG("MSTD", "Config persisted");
  return true;
}

bool MsTodoConfig::forget() {
  bool ok = true;
  if (Storage.exists(kConfigPath) && !Storage.remove(kConfigPath)) {
    LOG_ERR("MSTD", "Could not remove %s", kConfigPath);
    ok = false;
  }
  if (Storage.exists(kSnapshotBmpPath) && !Storage.remove(kSnapshotBmpPath)) {
    LOG_ERR("MSTD", "Could not remove %s", kSnapshotBmpPath);
    ok = false;
  }
  if (Storage.exists(kSnapshotMetaPath) && !Storage.remove(kSnapshotMetaPath)) {
    LOG_ERR("MSTD", "Could not remove %s", kSnapshotMetaPath);
    ok = false;
  }
  // Best-effort cleanup of orphaned tmp files from any prior failed write.
  Storage.remove(kConfigTmpPath);
  Storage.remove(kSnapshotMetaTmpPath);

  // Reset in-memory state unconditionally: the config file is gone, so keeping
  // a live session in RAM would only diverge from disk until the next boot.
  accessToken.clear();
  refreshToken.clear();
  tokenExpiryEpoch = 0;
  clientId.clear();
  sleepScreenEnabled = false;
  showOverdue = true;
  futureDays = kDefaultFutureDays;
  return ok;
}

// Append a log entry to /.crosspoint/mstodo.log with timestamp and details.
// This is used to capture detailed error information for later analysis.
void MsTodoConfig::appendLog(const std::string& message) {
  // Ensure the directory exists
  Storage.ensureDirectoryExists("/.crosspoint");

  HalFile file;
  if (!Storage.openFileForWrite("MSTD", kLogPath, file)) {
    LOG_ERR("MSTD", "Cannot open log file for write");
    return;
  }

  // Get current time for timestamp
  time_t now = time(nullptr);
  struct tm tm_now;
  localtime_r(&now, &tm_now);
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_now);

  // Format log entry
  char logEntry[512];
  int len = snprintf(logEntry, sizeof(logEntry), "[%s] %s\n", timestamp, message.c_str());
  if (len > 0) {
    file.write(reinterpret_cast<const uint8_t*>(logEntry), static_cast<size_t>(len));
  }
}

// Read the log file content for debugging
std::string MsTodoConfig::readLog() {
  if (!Storage.exists(kLogPath)) {
    return "";
  }

  HalFile file;
  if (!Storage.openFileForRead("MSTD", kLogPath, file)) {
    return "";
  }

  std::string buffer;
  uint8_t chunk[256];
  while (true) {
    int n = file.read(chunk, sizeof(chunk));
    if (n <= 0) break;
    buffer.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n));
  }
  return buffer;
}

// Clear the log file
void MsTodoConfig::clearLog() {
  if (Storage.exists(kLogPath)) {
    Storage.remove(kLogPath);
  }
}

}  // namespace mstodo
