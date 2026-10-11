#pragma once

#include <string>

namespace mstodo {

// Configuration and OAuth session for the Microsoft To Do integration,
// persisted to /.crosspoint/mstodo.json on the SD card. The integration is a
// read-only "today" task list, so the only state we keep is the OAuth session
// (access/refresh token + expiry), the resolved default list id, the optional
// client-id override, and the sleep-screen toggle.
//
// There are no orientation settings: the activity and the sleep snapshot are
// always portrait.
class MsTodoConfig {
 public:
  static MsTodoConfig& getInstance();

  // Loads /.crosspoint/mstodo.json if present. Safe to call repeatedly.
  // Returns true on successful load (file present, parsed). False means the
  // integration is effectively disabled until the user fixes the file.
  bool load();

  // OAuth session. Empty strings mean "not signed in".
  const std::string& getAccessToken() const { return accessToken; }
  const std::string& getRefreshToken() const { return refreshToken; }

  // Unix time at which the access token expires; 0 means unknown/expired.
  long getTokenExpiryEpoch() const { return tokenExpiryEpoch; }

  // Resolved id of the user's default To Do list; empty until first fetch.
  const std::string& getListId() const { return listId; }

  // Client id to use for the OAuth flow: the SD override when set, otherwise
  // the compiled-in default app registration.
  std::string getClientId() const;

  // Toggle: render the MS To Do snapshot on sleep when available.
  bool isSleepScreenEnabled() const { return sleepScreenEnabled; }

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
  bool setListId(const std::string& id);
  bool setSleepScreenEnabled(bool enabled);

  // Sign out: clears the session and the resolved list id, deletes the sleep
  // snapshot, and persists. The sleep-screen toggle is deliberately kept so
  // re-signing-in does not silently reset the user's preference.
  bool signOut();

  // Forget: deletes /.crosspoint/mstodo.json and the sleep snapshot, and
  // resets in-memory state to defaults.
  bool forget();

 private:
  MsTodoConfig() = default;
  MsTodoConfig(const MsTodoConfig&) = delete;
  MsTodoConfig& operator=(const MsTodoConfig&) = delete;

  // Serialize current state to /.crosspoint/mstodo.json via temp+rename.
  bool persist();

  std::string accessToken;
  std::string refreshToken;
  long tokenExpiryEpoch = 0;
  std::string listId;
  std::string clientId;  // empty = use the compiled-in default
  bool sleepScreenEnabled = false;
};

#define MSTODO_CONFIG mstodo::MsTodoConfig::getInstance()

}  // namespace mstodo
