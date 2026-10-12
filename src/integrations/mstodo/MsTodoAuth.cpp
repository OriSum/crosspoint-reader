#include "MsTodoAuth.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include <cstring>

namespace mstodo {

namespace {

constexpr const char* kDeviceCodeUrl = "https://login.microsoftonline.com/common/oauth2/v2.0/devicecode";
constexpr const char* kTokenUrl = "https://login.microsoftonline.com/common/oauth2/v2.0/token";
// Space-separated scopes, URL-encoded as '+'. Tasks.Read is the read-only task
// scope; offline_access is what makes Microsoft issue a refresh token. No
// profile scope is needed: the client only calls /me/todo/... endpoints.
constexpr const char* kScopes = "Tasks.Read+offline_access";
constexpr int kHttpTimeoutMs = 15000;
// Keep HTTP rx/tx buffers small: mbedTLS needs ~32 KB of heap on the C3 on top
// of these, so every KB freed here is one mbedTLS can take.
constexpr size_t kHttpBufSize = 2048;
// Token responses are well under 4 KB, but the buffer is allocated lazily
// after the TLS handshake (same pattern as the Todoist client) so the
// allocation cannot starve mbedTLS of a contiguous block. 8 KB leaves headroom
// for a large access + refresh token pair without risking the post-handshake
// largest-free-block budget.
constexpr size_t kMaxResponseBytes = 8 * 1024;

struct ResponseBuffer {
  char* data = nullptr;
  size_t capacity = 0;
  size_t size = 0;
  bool truncated = false;
  bool allocFailed = false;
};

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (!buf || !evt->data || evt->data_len <= 0) return ESP_OK;
  // Lazy allocation: claim the response buffer only after the TLS handshake
  // has run and freed its scratch.
  if (!buf->data) {
    if (buf->allocFailed) return ESP_OK;
    buf->data = static_cast<char*>(malloc(buf->capacity));
    if (!buf->data) {
      buf->allocFailed = true;
      size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      LOG_ERR("MSTD", "OOM allocating %u-byte response buffer (free=%u, largest=%u)",
              static_cast<unsigned>(buf->capacity), static_cast<unsigned>(freeBytes), static_cast<unsigned>(largest));
      return ESP_OK;
    }
  }
  size_t len = static_cast<size_t>(evt->data_len);
  size_t avail = buf->capacity - buf->size;
  if (len > avail) {
    buf->truncated = true;
    len = avail;
    if (len == 0) return ESP_OK;
  }
  memcpy(buf->data + buf->size, evt->data, len);
  buf->size += len;
  return ESP_OK;
}

// Percent-encode for use in a form body value.
std::string urlEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  static const char hex[] = "0123456789ABCDEF";
  for (char c : s) {
    unsigned char uc = static_cast<unsigned char>(c);
    if ((uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9') || uc == '-' || uc == '_' ||
        uc == '.' || uc == '~') {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(hex[uc >> 4]);
      out.push_back(hex[uc & 0x0F]);
    }
  }
  return out;
}

// POST an application/x-www-form-urlencoded body. On success copies the
// response body into `outBody` and returns the HTTP status code; returns -1 on
// a transport or allocation failure. The response buffer is freed before
// returning, so `outBody` is the only surviving copy.
int postForm(const char* url, const std::string& body, std::string& outBody) {
  outBody.clear();
  ResponseBuffer buf;
  buf.capacity = kMaxResponseBytes;

  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = httpEventHandler;
  config.user_data = &buf;
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = kHttpTimeoutMs;
  config.buffer_size = kHttpBufSize;
  config.buffer_size_tx = kHttpBufSize;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("MSTD", "esp_http_client_init failed");
    return -1;
  }

  if (esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded") != ESP_OK ||
      esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) {
    LOG_ERR("MSTD", "Set header failed");
    esp_http_client_cleanup(client);
    return -1;
  }
  if (esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size())) != ESP_OK) {
    LOG_ERR("MSTD", "Set post field failed");
    esp_http_client_cleanup(client);
    return -1;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  LOG_DBG("MSTD", "HTTP %d (err=%d, %u bytes%s)", httpCode, err, static_cast<unsigned>(buf.size),
          buf.truncated ? " [truncated]" : "");

  if (err != ESP_OK || buf.allocFailed) {
    free(buf.data);
    return -1;
  }
  if (buf.truncated) {
    LOG_ERR("MSTD", "Response truncated at cap (%u bytes)", static_cast<unsigned>(buf.capacity));
  }
  if (buf.data) {
    outBody.assign(buf.data, buf.size);
  }
  free(buf.data);
  return httpCode;
}

// Maps the OAuth `error` field to an AuthResult. Unknown values are Failed.
AuthResult errorToResult(const char* error) {
  if (!error) return AuthResult::Failed;
  if (strcmp(error, "authorization_pending") == 0) return AuthResult::Pending;
  if (strcmp(error, "slow_down") == 0) return AuthResult::SlowDown;
  if (strcmp(error, "authorization_expired") == 0 || strcmp(error, "expired_token") == 0) return AuthResult::Expired;
  if (strcmp(error, "declined") == 0 || strcmp(error, "authorization_declined") == 0) return AuthResult::Declined;
  if (strcmp(error, "invalid_grant") == 0) return AuthResult::InvalidGrant;
  return AuthResult::Failed;
}

// Copies a JSON string field into a fixed buffer, always null-terminating.
void copyField(char* dst, size_t dstCap, const char* src) {
  if (dstCap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n >= dstCap) n = dstCap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// Shared parse of a token response. On success fills the three out-params and
// returns Ok; otherwise returns the mapped error. `outRefresh` is only
// overwritten when the response carries a new refresh token, so a refresh
// response that omits it keeps the caller's existing value.
AuthResult parseTokenResponse(const std::string& body, std::string& outAccess, std::string& outRefresh,
                              long& outExpiresIn) {
  JsonDocument doc;
  auto parseErr = deserializeJson(doc, body);
  if (parseErr) {
    LOG_ERR("MSTD", "Token JSON parse: %s", parseErr.c_str());
    return AuthResult::Failed;
  }

  const char* error = doc["error"] | static_cast<const char*>(nullptr);
  if (error) {
    LOG_ERR("MSTD", "Token error: %s", error);
    return errorToResult(error);
  }

  const char* access = doc["access_token"] | static_cast<const char*>(nullptr);
  if (!access || access[0] == '\0') {
    LOG_ERR("MSTD", "Token response missing access_token");
    return AuthResult::Failed;
  }
  outAccess = access;

  const char* refresh = doc["refresh_token"] | static_cast<const char*>(nullptr);
  if (refresh && refresh[0] != '\0') {
    outRefresh = refresh;
  }

  outExpiresIn = doc["expires_in"] | 0L;
  // Never log token values, not even truncated.
  LOG_DBG("MSTD", "Token ok (access=%u chars, refresh=%u chars, expires_in=%ld)",
          static_cast<unsigned>(outAccess.size()), static_cast<unsigned>(outRefresh.size()), outExpiresIn);
  return AuthResult::Ok;
}

}  // namespace

AuthResult MsTodoAuth::requestDeviceCode(const std::string& clientId, DeviceCodeInfo& out) {
  out = {};
  if (clientId.empty()) {
    LOG_ERR("MSTD", "Empty client id");
    return AuthResult::Failed;
  }

  const std::string body = "client_id=" + urlEncode(clientId) + "&scope=" + kScopes;

  std::string response;
  const int code = postForm(kDeviceCodeUrl, body, response);
  if (code < 0) return AuthResult::NetworkError;
  if (code != 200) {
    LOG_ERR("MSTD", "Device code HTTP %d", code);
    return AuthResult::Failed;
  }

  JsonDocument doc;
  auto parseErr = deserializeJson(doc, response);
  if (parseErr) {
    LOG_ERR("MSTD", "Device code JSON parse: %s", parseErr.c_str());
    return AuthResult::Failed;
  }

  const char* userCode = doc["user_code"] | static_cast<const char*>(nullptr);
  const char* verificationUri = doc["verification_uri"] | static_cast<const char*>(nullptr);
  const char* deviceCode = doc["device_code"] | static_cast<const char*>(nullptr);
  if (!userCode || !verificationUri || !deviceCode) {
    LOG_ERR("MSTD", "Device code response missing fields");
    return AuthResult::Failed;
  }
  // A truncated verification URI would render a broken QR/URL, so reject it
  // rather than silently returning Ok.
  if (strlen(verificationUri) >= sizeof(out.verificationUri)) {
    LOG_ERR("MSTD", "verification_uri too long (%u chars)", static_cast<unsigned>(strlen(verificationUri)));
    return AuthResult::Failed;
  }

  copyField(out.userCode, sizeof(out.userCode), userCode);
  copyField(out.verificationUri, sizeof(out.verificationUri), verificationUri);
  out.deviceCode = deviceCode;
  out.expiresInSeconds = doc["expires_in"] | 900;
  out.intervalSeconds = doc["interval"] | 5;
  LOG_DBG("MSTD", "Device code ok (expires_in=%d, interval=%d)", out.expiresInSeconds, out.intervalSeconds);
  return AuthResult::Ok;
}

AuthResult MsTodoAuth::pollToken(const std::string& clientId, const std::string& deviceCode, std::string& outAccess,
                                 std::string& outRefresh, long& outExpiresIn) {
  if (clientId.empty() || deviceCode.empty()) {
    LOG_ERR("MSTD", "Poll with empty client id or device code");
    return AuthResult::Failed;
  }

  const std::string body = "grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=" + urlEncode(clientId) +
                           "&device_code=" + urlEncode(deviceCode);

  std::string response;
  const int code = postForm(kTokenUrl, body, response);
  if (code < 0) return AuthResult::NetworkError;
  // Microsoft returns 400 for the pending/slow_down/expired/declined cases and
  // carries the reason in the JSON `error` field, so parse the body regardless
  // of the status code.
  return parseTokenResponse(response, outAccess, outRefresh, outExpiresIn);
}

AuthResult MsTodoAuth::refreshTokens(const std::string& clientId, const std::string& refreshToken,
                                     std::string& outAccess, std::string& outRefresh, long& outExpiresIn) {
  if (clientId.empty() || refreshToken.empty()) {
    LOG_ERR("MSTD", "Refresh with empty client id or refresh token");
    // A missing client id is a configuration fault, not a rejected token, so
    // it must not send the caller down the re-sign-in path.
    return clientId.empty() ? AuthResult::Failed : AuthResult::InvalidGrant;
  }

  const std::string body = "grant_type=refresh_token&client_id=" + urlEncode(clientId) +
                           "&refresh_token=" + urlEncode(refreshToken) + "&scope=" + kScopes;

  std::string response;
  const int code = postForm(kTokenUrl, body, response);
  if (code < 0) return AuthResult::NetworkError;
  return parseTokenResponse(response, outAccess, outRefresh, outExpiresIn);
}

}  // namespace mstodo
