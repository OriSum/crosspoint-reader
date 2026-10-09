#include "TodoistTask.h"

#include <ctime>
#include <cstdio>
#include <cstring>

namespace {

time_t parseIso8601Utc(const std::string& value) {
  if (value.empty()) {
    return 0;
  }

  char buffer[32];
  std::strncpy(buffer, value.c_str(), sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  struct tm timeinfo {
  };
  memset(&timeinfo, 0, sizeof(timeinfo));

  if (std::sscanf(buffer, "%4d-%2d-%2dT%2d:%2d:%2dZ", &timeinfo.tm_year, &timeinfo.tm_mon, &timeinfo.tm_mday,
                  &timeinfo.tm_hour, &timeinfo.tm_min, &timeinfo.tm_sec) == 6) {
    timeinfo.tm_year -= 1900;
    timeinfo.tm_mon -= 1;
    timeinfo.tm_isdst = 0;
    return timegm(&timeinfo);
  }

  if (std::sscanf(buffer, "%4d-%2d-%2d", &timeinfo.tm_year, &timeinfo.tm_mon, &timeinfo.tm_mday) == 3) {
    timeinfo.tm_year -= 1900;
    timeinfo.tm_mon -= 1;
    timeinfo.tm_isdst = 0;
    return timegm(&timeinfo);
  }

  return 0;
}

std::string formatUtcDate(time_t timestamp) {
  if (timestamp == 0) {
    return "";
  }

  struct tm timeinfo;
  gmtime_r(&timestamp, &timeinfo);

  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%a %d", &timeinfo);
  return std::string(buffer);
}

std::string formatUtcTime(time_t timestamp) {
  if (timestamp == 0) {
    return "";
  }

  struct tm timeinfo;
  gmtime_r(&timestamp, &timeinfo);

  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%H:%M", &timeinfo);
  return std::string(buffer);
}

}  // namespace

time_t TodoistTask::getDueTimestamp() const {
  return parseIso8601Utc(due);
}

std::string TodoistTask::getDueDateStr() const {
  if (due.empty()) {
    return "";
  }

  const auto timestamp = getDueTimestamp();
  if (timestamp == 0) {
    return due;
  }

  if (due.find('T') == std::string::npos) {
    return formatUtcDate(timestamp);
  }

  const auto now = time(nullptr);
  const auto midnightToday = now - (now % 86400);
  const auto dueToday = timestamp - (timestamp % 86400);

  if (dueToday == midnightToday) {
    return "Today";
  }

  if (dueToday == midnightToday + 86400) {
    return "Tomorrow";
  }

  return formatUtcDate(timestamp);
}

std::string TodoistTask::getDueTimeStr() const {
  if (due.empty()) {
    return "";
  }

  if (due.find('T') == std::string::npos) {
    return "";
  }

  const auto timestamp = getDueTimestamp();
  if (timestamp == 0) {
    return "";
  }

  return formatUtcTime(timestamp);
}
