#pragma once

#include <ctime>
#include <string>

struct TodoistTask {
  std::string id;
  std::string content;
  int priority = 4;
  std::string due;
  bool isOverdue = false;
  bool isRecurring = false;

  void calculateOverdue() {
    const auto ts = getDueTimestamp();
    isOverdue = !due.empty() && ts != 0 && (time(nullptr) > ts);
  }

  time_t getDueTimestamp() const;
  std::string getDueDateStr() const;
  std::string getDueTimeStr() const;
};
