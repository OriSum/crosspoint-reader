#pragma once

#include <string>
#include <vector>

#include "TodoistConfig.h"
#include "TodoistTask.h"

namespace todoist {

enum class FetchResult {
  Ok,
  InvalidToken,  // 401/403
  RateLimited,   // 429
  NetworkError,  // timeout, TLS handshake, transport
  ServerError,   // 5xx
  ParseError,    // bad/unexpected JSON
};

class TodoistClient {
 public:
  // Synchronous fetch of today's tasks (plus overdue, per the fixed
  // "today | overdue" query). Caller must guarantee WiFi is up and that the
  // system clock is set (NTP). Reserves out capacity to a sane upper bound
  // and truncates extras silently. Never blocks indefinitely (15s HTTP
  // timeout).
  static FetchResult fetch(const std::string& apiToken, std::vector<TodoistTask>& outTasks);
};

}  // namespace todoist
