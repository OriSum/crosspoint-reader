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
};

class MsTodoClient {
 public:
  // Fetches the "Planned" view: every task with a due date in the
  // overdue + today + next kTaskWindowDays window, across all of the user's
  // To Do lists. Microsoft To Do's Planned view is a smart view, not a list,
  // and Graph exposes no aggregate endpoint for it, so this walks every list.
  // accessToken must be fresh. On success, outTasks is filled (reserved to
  // kMaxTasks) and sorted overdue-first then by due date; the activity applies
  // its own final ordering.
  static FetchResult fetchPlanned(const std::string& accessToken, std::vector<MsTodoTask>& outTasks);
};

}  // namespace mstodo