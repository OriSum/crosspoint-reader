#include "MsTodoClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "util/TaskWatchdog.h"
#include "MsTodoConfig.h"

namespace mstodo {

namespace {

constexpr const char* kGraphBase = "https://graph.microsoft.com/v1.0";
constexpr int kHttpTimeoutMs = 15000;
// Keep HTTP rx/tx buffers small. mbedTLS handshake on ESP32-C3 needs ~32 KB
// of heap on top of these — every KB we free here is one mbedTLS can take.
constexpr size_t kHttpBufSize = 2048;
constexpr size_t kMaxTasks = 64;
// Upper bound on lists walked per fetch. A user with more lists than this
// loses the tail of the Planned view rather than the whole fetch.
constexpr size_t kMaxLists = 32;
// Safety cap on pages of the /me/todo/lists response. A user with more lists
// than one page still gets the first kMaxLists; the cap bounds the walk.
constexpr int kMaxListPages = 4;
// Graph task JSON is verbose (~1.7 KB/task: recurrence, id, etag, timestamps)
// and $select cannot trim it (see docs/mstodo-integration.md). A page must
// therefore stay well under kMaxResponseBytes; 8 tasks is ~14-18 KB, which
// leaves headroom for the largest list observed. Pages are followed via
// @odata.nextLink until the list is exhausted.
constexpr int kTasksPageSize = 8;
// Safety cap on pages per list. Deliberately smaller than kMaxTasks /
// kTasksPageSize so one very busy list cannot consume the whole task budget
// and starve every other list: a single list contributes at most 32 tasks.
constexpr int kMaxPagesPerList = 4;
// 24 KB cap trades a higher steady-state footprint for predictability (same
// rationale as the removed Todoist client's 16 KB cap). A page that still
// overflows is skipped rather than aborting the whole fetch.
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

// Log detailed error information to mstodo.log for later analysis
void logFetchError(const char* context, const std::string& details) {
  MsTodoConfig::getInstance().appendLog(std::string("FETCH_ERROR: ") + context + " - " + details);
}

// Log detailed error information with HTTP response
void logFetchErrorWithResponse(const char* context, int httpCode, const std::string& response) {
  std::string details = "HTTP " + std::to_string(httpCode);
  if (!response.empty()) {
    // Truncate response to avoid huge log entries
    std::string truncated = response.substr(0, 500);
    details += " Response: " + truncated;
    if (response.size() > 500) details += "...";
  }
  logFetchError(context, details);
}

// Parses an ISO 8601 datetime string (assumed to be in UTC, as returned by
// Microsoft Graph with timeZone="UTC") and converts it to the specified
// timezone. Writes the local date as "YYYY-MM-DD" to outDate (size 11) and
// local time as "HH:MM" to outTime (size 6). Returns true on success.
// If posixTz is nullptr, uses the current process TZ setting.
bool parseUtcToLocal(const char* utcDateTime, char* outDate, size_t dateCap, char* outTime, size_t timeCap,
                     const char* posixTz) {
  if (!utcDateTime || dateCap < 11 || timeCap < 6) return false;

  int y, mo, d, h, mi, s;
  // Parse "YYYY-MM-DDTHH:MM:SS" (ignore fractional seconds and timezone suffix)
  if (sscanf(utcDateTime, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) {
    return false;
  }

  // Build a UTC tm struct
  struct tm utcTm = {};
  utcTm.tm_year = y - 1900;
  utcTm.tm_mon = mo - 1;
  utcTm.tm_mday = d;
  utcTm.tm_hour = h;
  utcTm.tm_min = mi;
  utcTm.tm_sec = s;
  utcTm.tm_isdst = 0;

  // Convert to time_t (UTC epoch). Use mktime after setting TZ to UTC to avoid
  // any local timezone interference.
  const char* tzBefore = getenv("TZ");
  char savedTz[64] = {0};
  if (tzBefore) snprintf(savedTz, sizeof(savedTz), "%s", tzBefore);

  setenv("TZ", "UTC0", 1);
  tzset();
  time_t utcEpoch = mktime(&utcTm);

  // Set the target timezone for localtime_r
  const char* tzToUse = (posixTz && posixTz[0] != '\0') ? posixTz : (savedTz[0] ? savedTz : "UTC0");
  setenv("TZ", tzToUse, 1);
  tzset();

  // Log the timezone being used for debugging (first few calls only)
  static int logCount = 0;
  if (logCount < 3) {
    char logMsg[128];
    snprintf(logMsg, sizeof(logMsg), "TZ_CONV: posixTz=%s savedTz=%s tzToUse=%s utcEpoch=%ld",
             posixTz ? posixTz : "NULL", savedTz[0] ? savedTz : "NULL", tzToUse, static_cast<long>(utcEpoch));
    MsTodoConfig::getInstance().appendLog(logMsg);
    logCount++;
  }

  if (utcEpoch < 0) return false;

  // Convert to local time using the target timezone
  struct tm localTm;
  localtime_r(&utcEpoch, &localTm);

  // Log the conversion result for debugging
  if (logCount <= 3) {
    char logMsg[128];
    snprintf(logMsg, sizeof(logMsg), "TZ_CONV_RESULT: utc=%04d-%02d-%02dT%02d:%02d:%02d local=%04d-%02d-%02dT%02d:%02d",
             y, mo, d, h, mi, s,
             localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday,
             localTm.tm_hour, localTm.tm_min);
    MsTodoConfig::getInstance().appendLog(logMsg);
  }

  strftime(outDate, dateCap, "%Y-%m-%d", &localTm);
  strftime(outTime, timeCap, "%H:%M", &localTm);
  return true;
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

// Writes the local date `daysAhead` days from now as "YYYY-MM-DD" (size 11).
// Used as the inclusive upper bound of the tasks $filter. If the clock is not
// set yet, writes "9999-12-31" so a bad clock never hides tasks.
void formatDatePlusDays(int daysAhead, char* out, size_t outCap) {
  if (outCap < 11) return;
  time_t now = time(nullptr);
  if (now < 1700000000) {  // clock not set yet
    snprintf(out, outCap, "9999-12-31");
    return;
  }
  now += static_cast<time_t>(daysAhead) * 86400;
  struct tm tm;
  localtime_r(&now, &tm);
  strftime(out, outCap, "%Y-%m-%d", &tm);
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
  if (code == 429) return FetchResult::RateLimited;
  if (code >= 500) return FetchResult::ServerError;
  return FetchResult::NetworkError;
}

// One HTTPS session against Graph, reused across every request in a fetch.
// The TLS handshake is the dominant cost on the C3, so the connection is kept
// alive (config.keep_alive_enable) and the response buffer is allocated once
// and reused. All requests target graph.microsoft.com, so keep-alive applies.
class GraphSession {
 public:
  GraphSession() = default;
  ~GraphSession() {
    if (client) esp_http_client_cleanup(client);
    free(buf.data);
  }
  GraphSession(const GraphSession&) = delete;
  GraphSession& operator=(const GraphSession&) = delete;

  // Opens the session and sets the auth header. Returns false on init failure.
  bool begin(const std::string& accessToken) {
    buf.capacity = kMaxResponseBytes;

    esp_http_client_config_t config = {};
    config.url = kGraphBase;  // placeholder; set_url before each request
    config.event_handler = httpEventHandler;
    config.user_data = &buf;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = kHttpBufSize;
    config.buffer_size_tx = kHttpBufSize;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = true;

    client = esp_http_client_init(&config);
    if (!client) {
      LOG_ERR("MSTD", "esp_http_client_init failed");
      return false;
    }

    std::string authHeader = "Bearer " + accessToken;
    if (esp_http_client_set_header(client, "Authorization", authHeader.c_str()) != ESP_OK ||
        esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) {
      LOG_ERR("MSTD", "Set header failed");
      return false;
    }
    return true;
  }

  // GET `url`. On success copies the body into `outBody` and returns the HTTP
  // status code; returns -1 on a transport or allocation failure.
  int get(const std::string& url, std::string& outBody) {
    outBody.clear();
    if (!client) return -1;

    // Reset the shared buffer for this response; keep the allocation.
    buf.size = 0;
    buf.truncated = false;

    if (esp_http_client_set_url(client, url.c_str()) != ESP_OK) {
      LOG_ERR("MSTD", "set_url failed");
      return -1;
    }

    // The fetch issues many sequential blocking requests on the loop task;
    // feed the task watchdog so a slow request cannot trip it.
    resetTaskWatchdogIfSubscribed();
    esp_err_t err = esp_http_client_perform(client);
    resetTaskWatchdogIfSubscribed();
    const int httpCode = esp_http_client_get_status_code(client);

    LOG_DBG("MSTD", "HTTP %d (err=%d, %u bytes%s)", httpCode, err, static_cast<unsigned>(buf.size),
            buf.truncated ? " [truncated]" : "");

    if (err != ESP_OK || buf.allocFailed) return -1;
    if (buf.truncated) {
      LOG_ERR("MSTD", "Response truncated at cap (%u bytes)", static_cast<unsigned>(buf.capacity));
    }
    if (buf.data) outBody.assign(buf.data, buf.size);
    return httpCode;
  }

  // True when the last get() response hit the buffer cap. The body is then
  // incomplete and must not be parsed.
  bool truncated() const { return buf.truncated; }

 private:
  esp_http_client_handle_t client = nullptr;
  ResponseBuffer buf;
};

// Appends the tasks in a Graph "value" array to outTasks, up to kMaxTasks.
void parseTasks(JsonArray arr, std::vector<MsTodoTask>& outTasks, const char* posixTz) {
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

    // Convert UTC datetime to local timezone
    t.dueDate[0] = '\0';
    t.dueTime[0] = '\0';
    if (dueDateTime) {
      parseUtcToLocal(dueDateTime, t.dueDate, MsTodoTask::kDueDateCapacity, t.dueTime, MsTodoTask::kDueTimeCapacity, posixTz);
    }

    t.overdue = isOverdue(t.dueDate);

    outTasks.push_back(t);
  }
}

// Collects list ids from a /me/todo/lists "value" array, up to kMaxLists.
void parseListIds(JsonArray arr, std::vector<std::string>& outIds) {
  for (JsonObject list : arr) {
    if (outIds.size() >= kMaxLists) break;
    const char* id = list["id"] | static_cast<const char*>(nullptr);
    if (id && id[0] != '\0') outIds.emplace_back(id);
  }
}

}  // namespace

FetchResult MsTodoClient::fetchPlanned(const std::string& accessToken, const FetchOptions& options,
                                       std::vector<MsTodoTask>& outTasks, const char* posixTz) {
  outTasks.clear();
  outTasks.reserve(kMaxTasks);

  if (accessToken.empty()) {
    LOG_ERR("MSTD", "Empty token");
    return FetchResult::InvalidToken;
  }

  GraphSession session;
  if (!session.begin(accessToken)) return FetchResult::NetworkError;

  // Step 1: collect every list id. Microsoft To Do's Planned view spans all
  // lists, and Graph has no aggregate endpoint for it, so we walk each list.
  // No $select here: Microsoft Graph rejects $select on the To Do endpoints
  // for personal (MSA) accounts with HTTP 400 RequestBroker--ParseUri, and
  // silently ignores it for the few field names it does accept. See
  // docs/mstodo-integration.md ("Why the requests look the way they do").
  std::vector<std::string> listIds;
  listIds.reserve(kMaxLists);
  {
    std::string url = std::string(kGraphBase) + "/me/todo/lists";
    for (int page = 0; page < kMaxListPages && !url.empty(); ++page) {
      std::string response;
      const int code = session.get(url, response);
      if (code < 0) {
        logFetchError("list_resolution", "Transport error or allocation failure");
        return FetchResult::NetworkError;
      }
      if (code != 200) {
        logFetchErrorWithResponse("list_resolution", code, response);
        return httpStatusToFetchResult(code);
      }
      if (session.truncated()) {
        logFetchError("list_resolution", "Response truncated at cap");
        LOG_ERR("MSTD", "Lists response truncated; stopping list walk");
        break;
      }

      JsonDocument doc;
      auto parseErr = deserializeJson(doc, response);
      if (parseErr) {
        logFetchError("list_resolution", std::string("JSON parse error: ") + parseErr.c_str());
        LOG_ERR("MSTD", "List JSON parse: %s", parseErr.c_str());
        return FetchResult::ParseError;
      }
      parseListIds(doc["value"].as<JsonArray>(), listIds);

      const char* next = doc["@odata.nextLink"] | static_cast<const char*>(nullptr);
      url = next ? next : "";
      if (listIds.size() >= kMaxLists) break;
    }
  }
  if (listIds.empty()) {
    logFetchError("list_resolution", "No lists found");
    LOG_ERR("MSTD", "No lists found");
    return FetchResult::ParseError;
  }

  // Step 2: fetch tasks from every list. The $filter value contains spaces and
  // quotes, which http_parser rejects in a raw URL, so it must be
  // percent-encoded.
  //
  // The date bounds replace the old $select/$top approach. $select is unusable
  // on To Do (see Step 1), and $top alone returned the first N tasks in list
  // order, which hid overdue tasks sitting deeper in a long list. Filtering on
  // dueDateTime server-side keeps each response small and complete:
  //   - the lower bound excludes undated tasks (a null comparison is rejected)
  //     and, when show_overdue is off, excludes overdue tasks too;
  //   - the upper bound bounds the window to today + futureDays.
  // The activity still computes the today/overdue split on-device.
  //
  // formatDatePlusDays() writes "9999-12-31" when the clock is unset, so a
  // "today" lower bound would collapse the window to nothing on an unset
  // clock. Fail open instead: keep the very old literal when the clock is not
  // trustworthy, so a bad clock shows too much rather than nothing.
  char lowerBound[11];
  if (options.showOverdue) {
    snprintf(lowerBound, sizeof(lowerBound), "1970-01-01");
  } else {
    formatDatePlusDays(0, lowerBound, sizeof(lowerBound));
    if (strcmp(lowerBound, "9999-12-31") == 0) {
      snprintf(lowerBound, sizeof(lowerBound), "1970-01-01");
    }
  }
  char upperBound[11];
  formatDatePlusDays(options.futureDays, upperBound, sizeof(upperBound));

  std::string filter = "status ne 'completed' and dueDateTime/dateTime ge '";
  filter += lowerBound;
  filter += "' and dueDateTime/dateTime le '";
  filter += upperBound;
  filter += "'";
  const std::string filterEnc = urlEncode(filter);

  char pageSize[8];
  snprintf(pageSize, sizeof(pageSize), "%d", kTasksPageSize);

  // Track whether any list answered, so an all-lists failure surfaces as an
  // error instead of an empty "no tasks" view.
  bool anyListSucceeded = false;
  FetchResult lastError = FetchResult::Ok;

  for (const auto& listId : listIds) {
    if (outTasks.size() >= kMaxTasks) break;

    std::string url =
        std::string(kGraphBase) + "/me/todo/lists/" + listId + "/tasks?$filter=" + filterEnc + "&$top=" + pageSize;
    for (int page = 0; page < kMaxPagesPerList && !url.empty(); ++page) {
      std::string response;
      const int code = session.get(url, response);
      if (code < 0) {
        logFetchError("task_fetch", "Transport error or allocation failure for list " + listId);
        return FetchResult::NetworkError;
      }
      // A per-list failure (deleted list, transient 5xx/429) skips that list
      // and keeps the tasks already collected from the others. Only an auth
      // failure aborts the whole fetch.
      if (code == 401 || code == 403) {
        logFetchErrorWithResponse("task_fetch", code, response);
        return FetchResult::InvalidToken;
      }
      if (code != 200) {
        logFetchErrorWithResponse("task_fetch", code, response);
        LOG_ERR("MSTD", "Tasks HTTP %d for list; skipping", code);
        lastError = httpStatusToFetchResult(code);
        break;
      }
      anyListSucceeded = true;
      if (session.truncated()) {
        logFetchError("task_fetch", "Response truncated at cap for list " + listId);
        LOG_ERR("MSTD", "Tasks page truncated; skipping rest of list");
        break;
      }

      JsonDocument doc;
      auto parseErr = deserializeJson(doc, response);
      if (parseErr) {
        logFetchError("task_fetch", std::string("JSON parse error: ") + parseErr.c_str() + " for list " + listId);
        LOG_ERR("MSTD", "Tasks JSON parse: %s; skipping list", parseErr.c_str());
        break;
      }

      parseTasks(doc["value"].as<JsonArray>(), outTasks, posixTz);

      const char* next = doc["@odata.nextLink"] | static_cast<const char*>(nullptr);
      url = next ? next : "";
      if (outTasks.size() >= kMaxTasks) break;
    }
  }

  if (!anyListSucceeded && lastError != FetchResult::Ok) {
    logFetchError("task_fetch", "All lists failed");
    LOG_ERR("MSTD", "All lists failed");
    return lastError;
  }

  // Sort: overdue first (by date asc), then today/undated (by date asc), then future.
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

  LOG_DBG("MSTD", "Parsed %u tasks from %u lists (overdue=%d, days=%d)", static_cast<unsigned>(outTasks.size()),
          static_cast<unsigned>(listIds.size()), options.showOverdue, options.futureDays);
  return FetchResult::Ok;
}

}  // namespace mstodo