#pragma once

#include <string>

class TodoistConfig {
 public:
  enum class DateFilter : uint8_t { TODAY = 0, WEEK = 1, ALL = 2 };
  enum class OverdueFilter : uint8_t { SHOW = 0, HIDE = 1 };

  static TodoistConfig& getInstance() {
    static TodoistConfig instance;
    return instance;
  }

  TodoistConfig(const TodoistConfig&) = delete;
  TodoistConfig& operator=(const TodoistConfig&) = delete;

  bool loadFromFile();
  bool saveToFile() const;
  bool clear();

  const std::string& getApiToken() const { return apiToken_; }
  bool setApiToken(const std::string& token);

  bool isSleepScreenEnabled() const { return sleepScreenEnabled_; }
  void setSleepScreenEnabled(bool enabled) {
    if (sleepScreenEnabled_ != enabled) {
      sleepScreenEnabled_ = enabled;
      saveToFile();
    }
  }

  DateFilter getDateFilter() const { return dateFilter_; }
  void setDateFilter(DateFilter value) {
    if (dateFilter_ != value) {
      dateFilter_ = value;
      saveToFile();
    }
  }

  OverdueFilter getOverdueFilter() const { return overdueFilter_; }
  void setOverdueFilter(OverdueFilter value) {
    if (overdueFilter_ != value) {
      overdueFilter_ = value;
      saveToFile();
    }
  }

  std::string buildFilterQuery() const;

 private:
  TodoistConfig() = default;

  std::string apiToken_;
  bool sleepScreenEnabled_ = false;
  DateFilter dateFilter_ = DateFilter::TODAY;
  OverdueFilter overdueFilter_ = OverdueFilter::SHOW;
};

#define TODOIST_CONFIG TodoistConfig::getInstance()
