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

// Fetch window options, supplied by the caller (the activity reads them from
// MsTodoConfig). The client stays stateless and never reads config itself.
struct FetchOptions {
  bool showOverdue = true;
  int futureDays = 7;
};

class MsTodoClient {
 public:
  // Fetches the "Planned" view: every task with a due date in the
  // overdue (when options.showOverdue) + today + next options.futureDays
  // window, across all of the user's To Do lists. Microsoft To Do's Planned
  // view is a smart view, not a list, and Graph exposes no aggregate endpoint
  // for it, so this walks every list. accessToken must be fresh. On success,
  // outTasks is filled (reserved to kMaxTasks) and sorted overdue-first then
  // by due date; the activity applies its own final ordering.
  static FetchResult fetchPlanned(const std::string& accessToken, const FetchOptions& options,
                                  std::vector<MsTodoTask>& outTasks);
};

}  // namespace mstodo