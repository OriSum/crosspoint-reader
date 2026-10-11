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

// Public-client Azure app registration owned by the fork: delegated
// Tasks.Read + offline_access + User.Read, "Allow public client flows"
// enabled. Not a secret; users may override it via "client_id" in the config.
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
  listId.clear();
  clientId.clear();
  sleepScreenEnabled = false;

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
  listId = doc["list_id"] | std::string("");
  clientId = doc["client_id"] | std::string("");
  sleepScreenEnabled = doc["sleep_screen_enabled"] | false;

  // Never log token values, not even truncated.
  LOG_DBG("MSTD", "Config loaded (access=%u chars, refresh=%u chars, expiry=%ld, sleep=%d)",
          static_cast<unsigned>(accessToken.size()), static_cast<unsigned>(refreshToken.size()), tokenExpiryEpoch,
          sleepScreenEnabled);
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

bool MsTodoConfig::setListId(const std::string& id) {
  if (id == listId) return true;
  listId = id;
  return persist();
}

bool MsTodoConfig::setSleepScreenEnabled(bool enabled) {
  if (enabled == sleepScreenEnabled) return true;
  sleepScreenEnabled = enabled;
  return persist();
}

bool MsTodoConfig::signOut() {
  // Persist the cleared session first: if the write fails, the in-memory
  // session must stay consistent with the file on disk, so the user is not
  // silently signed back in on the next boot.
  const std::string savedAccess = accessToken;
  const std::string savedRefresh = refreshToken;
  const long savedExpiry = tokenExpiryEpoch;
  const std::string savedListId = listId;

  accessToken.clear();
  refreshToken.clear();
  tokenExpiryEpoch = 0;
  listId.clear();

  if (!persist()) {
    accessToken = savedAccess;
    refreshToken = savedRefresh;
    tokenExpiryEpoch = savedExpiry;
    listId = savedListId;
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
  // Best-effort cleanup of orphaned tmp files from any prior failed write.
  Storage.remove(kConfigTmpPath);
  Storage.remove(kSnapshotMetaTmpPath);

  // sleepScreenEnabled is intentionally kept so re-signing-in does not reset it.
  return ok;
}

bool MsTodoConfig::persist() {
  JsonDocument doc;
  doc["access_token"] = accessToken;
  doc["refresh_token"] = refreshToken;
  doc["token_expiry_epoch"] = tokenExpiryEpoch;
  doc["list_id"] = listId;
  doc["client_id"] = clientId;
  doc["sleep_screen_enabled"] = sleepScreenEnabled;

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
  listId.clear();
  clientId.clear();
  sleepScreenEnabled = false;
  return ok;
}

}  // namespace mstodo
