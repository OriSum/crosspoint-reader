#pragma once

#include <string>

namespace mstodo {

// Configuration and OAuth session for the Microsoft To Do integration,
// persisted to /.crosspoint/mstodo.json on the SD card. The integration is a
// read-only "Planned" task list (all lists, overdue + today + next N days), so
// the state we keep is the OAuth session (access/refresh token + expiry), the
// optional client-id override, and the display preferences (sleep-screen
// toggle, overdue visibility, forward window).
//
// There are no orientation settings: the activity and the sleep snapshot are
// always portrait.
class MsTodoConfig {
 public:
  static MsTodoConfig& getInstance();

  // Bounds for the forward fetch window, in days. The settings row cycles a
  // ladder inside this range; a hand-edited file is clamped to it.
  static constexpr int kMinFutureDays = 1;
  static constexpr int kMaxFutureDays = 30;
  static constexpr int kDefaultFutureDays = 7;

  // Loads /.crosspoint/mstodo.json if present. Safe to call repeatedly.
  // Returns true on successful load (file present, parsed). False means the
  // integration is effectively disabled until the user fixes the file.
  bool load();

  // OAuth session. Empty strings mean "not signed in".
  const std::string& getAccessToken() const { return accessToken; }
  const std::string& getRefreshToken() const { return refreshToken; }

  // Unix time at which the access token expires; 0 means unknown/expired.
  long getTokenExpiryEpoch() const { return tokenExpiryEpoch; }

  // Client id to use for the OAuth flow: the SD override when set, otherwise
  // the compiled-in default app registration.
  std::string getClientId() const;

  // Toggle: render the MS To Do snapshot on sleep when available.
  bool isSleepScreenEnabled() const { return sleepScreenEnabled; }

  // Toggle: include overdue tasks in the fetch. When false the fetch's lower
  // date bound is today, so overdue tasks never reach the list or the snapshot.
  bool isShowOverdue() const { return showOverdue; }

  // Forward window in days, inclusive of today. Always within
  // [kMinFutureDays, kMaxFutureDays].
  int getFutureDays() const { return futureDays; }

  // True when a usable session is stored. Both tokens are required: an access
  // token without a refresh token cannot outlive its one-hour lifetime.
  bool hasTokens() const;

  // True when the access token is missing, within 60 s of expiry, or the clock
  // is not trustworthy. The margin absorbs clock skew between the device and
  // Microsoft's token service; an untrusted clock fails closed so a stale
  // expiry cannot make an expired token look valid.
  bool isAccessTokenExpired() const;

  // Setters. Each performs a value-change check and an atomic write
  // (.tmp + rename) on change. Returns false on persistence failure.
  //
  // setTokens stores the three values as given. Callers must pass the current
  // refresh token when a refresh response omits a new one.
  bool setTokens(const std::string& access, const std::string& refresh, long expiryEpoch);
  bool setSleepScreenEnabled(bool enabled);
  bool setShowOverdue(bool enabled);
  // Clamps `days` to [kMinFutureDays, kMaxFutureDays] before storing.
  bool setFutureDays(int days);

  // Sign out: clears the session, deletes the sleep snapshot, and persists.
  // The display preferences (sleep-screen toggle, overdue visibility, forward
  // window) are deliberately kept so re-signing-in does not silently reset
  // them.
  bool signOut();

  // Forget: deletes /.crosspoint/mstodo.json and the sleep snapshot, and
  // resets in-memory state to defaults.
  bool forget();

  // Log management for debugging fetch failures
  // Appends a timestamped log entry to /.crosspoint/mstodo.log
  void appendLog(const std::string& message);
  // Reads the entire log file content
  std::string readLog();
  // Clears the log file
  void clearLog();

 private:
  MsTodoConfig() = default;
  MsTodoConfig(const MsTodoConfig&) = delete;
  MsTodoConfig& operator=(const MsTodoConfig&) = delete;

  // Serialize current state to /.crosspoint/mstodo.json via temp+rename.
  bool persist();

  std::string accessToken;
  std::string refreshToken;
  long tokenExpiryEpoch = 0;
  std::string clientId;  // empty = use the compiled-in default
  bool sleepScreenEnabled = false;
  bool showOverdue = true;
  int futureDays = kDefaultFutureDays;
};

#define MSTODO_CONFIG mstodo::MsTodoConfig::getInstance()

}  // namespace mstodo
