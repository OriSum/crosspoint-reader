#include "MsTodoClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace mstodo {

namespace {

constexpr const char* kGraphBase = "https://graph.microsoft.com/v1.0";
constexpr int kHttpTimeoutMs = 15000;
// Keep HTTP rx/tx buffers small. mbedTLS handshake on ESP32-C3 needs ~32 KB
// of heap on top of these — every KB we free here is one mbedTLS can take.
constexpr size_t kHttpBufSize = 2048;
constexpr size_t kMaxTasks = 64;
// Graph JSON is ~1.5x Todoist's even with $select. 64 tasks ≈ 22 KB worst
// case. 24 KB cap trades a higher steady-state footprint for predictability
// (same rationale as Todoist's 16 KB cap). Truncation is handled gracefully.
constexpr size_t kMaxResponseBytes = 24 * 1024;

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

// Percent-encode for use in a URL query value.
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

// Extracts HH:MM from a full ISO datetime ("YYYY-MM-DDTHH:MM:SS...") or
// from a date-only ("YYYY-MM-DD"). Writes to out (size kDueTimeCapacity).
// Empty out means "no time-of-day".
void extractDueTime(const char* due, char* out, size_t outCap) {
  if (outCap == 0) return;
  out[0] = '\0';
  if (!due) return;
  const char* tPos = strchr(due, 'T');
  if (!tPos) return;  // date-only, no time
  // Want 5 chars after T: "HH:MM"
  if (strlen(tPos + 1) < 5) return;
  if (outCap < 6) return;
  snprintf(out, outCap, "%c%c:%c%c", tPos[1], tPos[2], tPos[4], tPos[5]);
}

// Returns true if the due date is strictly before today's date in the
// device's current timezone. We compare via time_t to avoid string parsing
// of timezone offsets. If parsing fails, returns false (not overdue).
bool isOverdue(const char* due) {
  if (!due) return false;
  int y, mo, d;
  if (sscanf(due, "%4d-%2d-%2d", &y, &mo, &d) != 3) return false;

  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // clock not set yet
  struct tm nowTm;
  localtime_r(&now, &nowTm);

  if (y < nowTm.tm_year + 1900) return true;
  if (y > nowTm.tm_year + 1900) return false;
  if (mo < nowTm.tm_mon + 1) return true;
  if (mo > nowTm.tm_mon + 1) return false;
  return d < nowTm.tm_mday;
}

void copyTitle(char* dst, size_t dstCap, const char* src) {
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

FetchResult httpStatusToFetchResult(int code) {
  if (code == 200) return FetchResult::Ok;
  if (code == 401 || code == 403) return FetchResult::InvalidToken;
  if (code == 404) return FetchResult::ListGone;
  if (code == 429) return FetchResult::RateLimited;
  if (code >= 500) return FetchResult::ServerError;
  return FetchResult::NetworkError;
}

// GET request with Bearer token. On success copies the response body into
// `outBody` and returns the HTTP status code; returns -1 on a transport or
// allocation failure. The response buffer is freed before returning.
int getWithToken(const char* url, const std::string& accessToken, std::string& outBody) {
  outBody.clear();
  ResponseBuffer buf;
  buf.capacity = kMaxResponseBytes;

  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = httpEventHandler;
  config.user_data = &buf;
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = kHttpTimeoutMs;
  config.buffer_size = kHttpBufSize;
  config.buffer_size_tx = kHttpBufSize;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("MSTD", "esp_http_client_init failed");
    free(buf.data);
    return -1;
  }

  std::string authHeader = "Bearer " + accessToken;
  if (esp_http_client_set_header(client, "Authorization", authHeader.c_str()) != ESP_OK ||
      esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) {
    LOG_ERR("MSTD", "Set header failed");
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

}  // namespace

FetchResult MsTodoClient::fetchToday(const std::string& accessToken, std::string& outListId,
                                     std::vector<MsTodoTask>& outTasks) {
  outTasks.clear();
  outTasks.reserve(kMaxTasks);

  if (accessToken.empty()) {
    LOG_ERR("MSTD", "Empty token");
    return FetchResult::InvalidToken;
  }

  // Step 1: resolve the default list id if we don't have it cached.
  if (outListId.empty()) {
    std::string url = std::string(kGraphBase) + "/me/todo/lists?$select=id,wellknownListName";
    std::string response;
    const int code = getWithToken(url.c_str(), accessToken, response);
    if (code < 0) return FetchResult::NetworkError;
    if (code != 200) {
      LOG_ERR("MSTD", "List resolution HTTP %d", code);
      return httpStatusToFetchResult(code);
    }

    JsonDocument doc;
    auto parseErr = deserializeJson(doc, response);
    if (parseErr) {
      LOG_ERR("MSTD", "List JSON parse: %s", parseErr.c_str());
      return FetchResult::ParseError;
    }

    JsonArray lists = doc["value"].as<JsonArray>();
    for (JsonObject list : lists) {
      const char* wellknown = list["wellknownListName"] | static_cast<const char*>(nullptr);
      // Graph's wellknownListName enum value for the built-in Tasks list is
      // "defaultList" (not "default").
      if (wellknown && strcmp(wellknown, "defaultList") == 0) {
        const char* id = list["id"] | static_cast<const char*>(nullptr);
        if (id && id[0] != '\0') {
          outListId = id;
          LOG_DBG("MSTD", "Resolved default list id: %s", id);
          break;
        }
      }
    }
    if (outListId.empty()) {
      LOG_ERR("MSTD", "Default list not found");
      return FetchResult::ListGone;
    }
  }

  // Step 2: fetch tasks from the resolved list. The $filter value contains
  // spaces and quotes, which http_parser rejects in a raw URL, so it must be
  // percent-encoded; the $select/$top values are already URL-safe.
  std::string url = std::string(kGraphBase) + "/me/todo/lists/" + outListId +
                    "/tasks?$filter=" + urlEncode("status ne 'completed'") +
                    "&$select=title,dueDateTime,status,importance&$top=64";
  std::string response;
  const int code = getWithToken(url.c_str(), accessToken, response);
  if (code < 0) return FetchResult::NetworkError;
  if (code == 404) {
    // Stored list id is stale (user deleted the list). Caller will clear it
    // and retry once.
    return FetchResult::ListGone;
  }
  if (code != 200) {
    LOG_ERR("MSTD", "Tasks HTTP %d", code);
    return httpStatusToFetchResult(code);
  }

  JsonDocument doc;
  auto parseErr = deserializeJson(doc, response);
  if (parseErr) {
    LOG_ERR("MSTD", "Tasks JSON parse: %s", parseErr.c_str());
    return FetchResult::ParseError;
  }

  JsonArray arr = doc["value"].as<JsonArray>();
  for (JsonObject task : arr) {
    if (outTasks.size() >= kMaxTasks) break;
    MsTodoTask t = {};

    copyTitle(t.title, MsTodoTask::kTitleCapacity, task["title"] | "");

    // importance: "high"=4, "normal"=2, "low"=1, missing=0
    const char* importance = task["importance"] | static_cast<const char*>(nullptr);
    if (importance && strcmp(importance, "high") == 0)
      t.priority = 4;
    else if (importance && strcmp(importance, "normal") == 0)
      t.priority = 2;
    else if (importance && strcmp(importance, "low") == 0)
      t.priority = 1;
    else
      t.priority = 0;

    // dueDateTime: { "dateTime": "2026-10-10T09:30:00.0000000+02:00", "timeZone": "..." }
    const char* dueDateTime = nullptr;
    if (task["dueDateTime"].is<JsonObject>()) {
      JsonObject due = task["dueDateTime"].as<JsonObject>();
      dueDateTime = due["dateTime"] | static_cast<const char*>(nullptr);
    }

    // Extract YYYY-MM-DD prefix and HH:MM after T
    t.dueDate[0] = '\0';
    t.dueTime[0] = '\0';
    if (dueDateTime && strlen(dueDateTime) >= 10) {
      memcpy(t.dueDate, dueDateTime, 10);
      t.dueDate[10] = '\0';
      extractDueTime(dueDateTime, t.dueTime, MsTodoTask::kDueTimeCapacity);
    }

    t.overdue = isOverdue(t.dueDate);

    outTasks.push_back(t);
  }

  // Sort: overdue first (by date asc), then today/undated (by date asc), then future.
  // Same comparator as TodoistClient.
  std::sort(outTasks.begin(), outTasks.end(), [](const MsTodoTask& a, const MsTodoTask& b) {
    if (a.overdue != b.overdue) return a.overdue;  // overdue first
    // Both overdue or both not: sort by dueDate (empty = no due date = last)
    if (a.dueDate[0] != '\0' && b.dueDate[0] != '\0') {
      return strcmp(a.dueDate, b.dueDate) < 0;
    }
    if (a.dueDate[0] != '\0') return true;   // a has date, b doesn't
    if (b.dueDate[0] != '\0') return false;  // b has date, a doesn't
    return false;                            // both undated: stable
  });

  LOG_DBG("MSTD", "Parsed %u tasks", static_cast<unsigned>(outTasks.size()));
  return FetchResult::Ok;
}

}  // namespace mstodo