#include "TodoistConfig.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <string>

namespace {
constexpr char TODOIST_FILE_JSON[] = "/.crosspoint/todoist.json";
}

bool TodoistConfig::loadFromFile() {
  if (!Storage.exists(TODOIST_FILE_JSON)) {
    return false;
  }

  String json = Storage.readFile(TODOIST_FILE_JSON);
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  const auto error = deserializeJson(doc, json.c_str());
  if (error) {
    LOG_ERR("TODOIST", "JSON parse error while loading config: %s", error.c_str());
    return false;
  }

  apiToken_ = doc["api_token"] | std::string("");
  sleepScreenEnabled_ = doc["sleep_screen_enabled"] | false;

  const auto dateFilterValue = doc["date_filter"] | static_cast<int>(DateFilter::TODAY);
  switch (dateFilterValue) {
    case static_cast<int>(DateFilter::TODAY):
      dateFilter_ = DateFilter::TODAY;
      break;
    case static_cast<int>(DateFilter::WEEK):
      dateFilter_ = DateFilter::WEEK;
      break;
    case static_cast<int>(DateFilter::ALL):
    default:
      dateFilter_ = DateFilter::ALL;
      break;
  }

  const auto overdueFilterValue = doc["show_overdue"] | static_cast<int>(OverdueFilter::SHOW);
  switch (overdueFilterValue) {
    case static_cast<int>(OverdueFilter::HIDE):
      overdueFilter_ = OverdueFilter::HIDE;
      break;
    case static_cast<int>(OverdueFilter::SHOW):
    default:
      overdueFilter_ = OverdueFilter::SHOW;
      break;
  }

  return true;
}

bool TodoistConfig::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["api_token"] = apiToken_.c_str();
  doc["sleep_screen_enabled"] = sleepScreenEnabled_;
  doc["date_filter"] = static_cast<int>(dateFilter_);
  doc["show_overdue"] = static_cast<int>(overdueFilter_);

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(TODOIST_FILE_JSON, json);
}

bool TodoistConfig::clear() {
  apiToken_.clear();
  sleepScreenEnabled_ = false;
  dateFilter_ = DateFilter::TODAY;
  overdueFilter_ = OverdueFilter::SHOW;
  return saveToFile();
}

bool TodoistConfig::setApiToken(const std::string& token) {
  if (apiToken_ == token) {
    return true;
  }

  apiToken_ = token;
  return saveToFile();
}

std::string TodoistConfig::buildFilterQuery() const {
  switch (dateFilter_) {
    case DateFilter::TODAY:
      return "today";
    case DateFilter::WEEK:
      return "due before +7 days";
    case DateFilter::ALL:
    default:
      return "";
  }
}
