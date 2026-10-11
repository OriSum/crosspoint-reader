#pragma once

#include <string>
#include <vector>

#include "MsTodoTask.h"

namespace mstodo {

enum class FetchResult {
  Ok,
  InvalidToken,  // 401/403
  RateLimited,   // 429
  NetworkError,  // timeout, TLS handshake, transport
  ServerError,   // 5xx
  ParseError,    // bad/unexpected JSON
  ListGone,      // 404 on tasks request for stored list_id
};

class MsTodoClient {
 public:
  // Fetches today + overdue tasks from the default list. accessToken must be
  // fresh. outListId is an in-out parameter:
  //   - If non-empty on entry, it is used as the cached list id (skips resolution).
  //   - If empty on entry, the default list is resolved and outListId is written.
  //   - On FetchResult::ListGone (404 for stored list id), the caller MUST clear
  //     outListId and retry once; the next call will re-resolve.
  // On success, outTasks is filled (reserved to kMaxTasks).
  static FetchResult fetchToday(const std::string& accessToken, std::string& outListId,
                                std::vector<MsTodoTask>& outTasks);
};

}  // namespace mstodo