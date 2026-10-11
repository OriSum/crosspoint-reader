#pragma once

#include <string>

namespace mstodo {

// Outcome of one OAuth call. The activity maps these to user-facing screens;
// the client never renders anything itself.
enum class AuthResult {
  Ok,
  NetworkError,  // timeout, TLS handshake, transport, or a malformed response
  Pending,       // device-code poll: user has not approved yet
  SlowDown,      // device-code poll: poll less often (RFC 8628 §3.5)
  Expired,       // device code expired before approval
  Declined,      // user rejected the sign-in
  Failed,        // endpoint returned an error we cannot classify
  InvalidGrant,  // refresh token rejected; caller must re-run sign-in
};

// Values from the device-code response. The display fields are fixed-size so
// the struct owns its data and stays valid after the response document is
// freed; `deviceCode` is the opaque ~900-char value the poll step needs, so it
// is a std::string rather than a fixed buffer.
struct DeviceCodeInfo {
  char userCode[16];         // "ABCD-1234\0"
  char verificationUri[64];  // "https://microsoft.com/devicelogin\0"
  int expiresInSeconds;
  int intervalSeconds;
  std::string deviceCode;  // opaque; passed back to pollToken()
};

// OAuth 2.0 device authorization grant (RFC 8628) against Microsoft Entra ID,
// plus refresh-token exchange. Stateless: every call is self-contained and the
// caller owns all persisted state. No client secret is used — the app
// registration is a public client, which is mandatory on a device that cannot
// keep a secret.
class MsTodoAuth {
 public:
  // Step 1: request a device code. Fills `out` on Ok.
  static AuthResult requestDeviceCode(const std::string& clientId, DeviceCodeInfo& out);

  // Step 2: one poll tick. On Ok, fills the three out-params. `Pending` and
  // `SlowDown` mean "call again later"; every other result is terminal.
  static AuthResult pollToken(const std::string& clientId, const std::string& deviceCode, std::string& outAccess,
                              std::string& outRefresh, long& outExpiresIn);

  // Exchange a refresh token. On Ok, fills the three out-params. Microsoft
  // rotates refresh tokens, so `outRefresh` may differ from the input; the
  // caller must persist whatever comes back.
  static AuthResult refreshTokens(const std::string& clientId, const std::string& refreshToken, std::string& outAccess,
                                  std::string& outRefresh, long& outExpiresIn);
};

}  // namespace mstodo
