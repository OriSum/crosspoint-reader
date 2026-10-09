# Todoist Integration — Minimal Plan with Date/Overdue Filters

**Status:** Draft analysis and reduced scope. This document defines the correct implementation path for a Todoist reader feature that fits the CrossPoint firmware architecture.

**Date:** 2026-10-09
**Branch:** `feat/todoist-integration-minimal`

---

## Executive Summary

The current PR (Feature/todoist-integration) is **oversized and has API mismatches**. This document:

1. **Identifies the actual compile/runtime risks** in the draft
2. **Defines a minimal, mergeable Todoist feature** with date and overdue filtering
3. **Provides a phased, reviewable implementation plan**

The goal is to add a read-only Todoist task list to the device with support for:
- **Today's tasks** (default view)
- **Date filtering** (today, next 7 days, all tasks)
- **Overdue filtering** (show/hide overdue tasks)
- Optional sleep-screen snapshot rendering

All without reimagining the UI layer, adding weather integrations, or overengineering the config model.

---

## Part 1: API Audit — What's Wrong in the Current Draft

### 1.1 UITheme / GUI Mismatch

**Current draft assumes:**
```cpp
GUI.drawHeader(renderer, Rect{...}, title, subtitle);  // with subtitle
GUI.drawList(renderer, Rect{...}, count, index, ...);  // BaseTheme signature
```

**Reality (from BaseTheme.h):**
```cpp
virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                        const char* subtitle = nullptr) const;

virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                      const std::function<std::string(int index)>& rowTitle,
                      const std::function<std::string(int index)>& rowSubtitle = nullptr,
                      const std::function<UIIcon(int index)>& rowIcon = nullptr,
                      const std::function<std::string(int index)>& rowValue = nullptr,
                      bool highlightValue = false) const;
```

**Issue:** `drawList` is **not a method on BaseTheme**—it's a standalone UI component method. The draft assumes a static `GUI` singleton; the actual project uses instance methods on the theme. **BREAKING MISMATCH.**

### 1.2 Missing GUI Definition

**Current draft uses:**
```cpp
#include "components/UITheme.h"
GUI.drawPopup(renderer, message);
GUI.drawButtonHints(renderer, btn1, btn2, btn3, btn4);
```

**Reality:** `GUI` is not defined in the audit. BaseTheme.h has these methods, but they're instance methods, not static.

**Fix required:** Use `UITheme::getInstance()` and call methods on that instance, or use direct renderer calls.

### 1.3 Rendering Entry Point

**Current draft does:**
```cpp
void TodoistActivity::render(RenderLock&&) {
  renderLoading(); // calls GUI.drawPopup(...)
  renderer.displayBuffer();
}
```

**Reality (from KOReaderSyncActivity.cpp:248):**
```cpp
void KOReaderSyncActivity::render(RenderLock&&) {
  // ... rendering code ...
  renderer.displayBuffer();
}
```

Pattern is correct, but `GUI` must be resolved.

### 1.4 ButtonNavigator / Input Handling

**Current draft assumes:**
```cpp
_navigator.onNext([this] { ... });
_navigator.onPrevious([this] { ... });
```

**Reality (from KOReaderSyncActivity.cpp:362–426):**
```cpp
void loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
  // Direct input polling, not push-based navigator
}
```

**Issue:** Input is pull-based, not push-based. No auto-wired navigator.

**Fix required:** Use `mappedInput.wasReleased()` directly.

### 1.5 renderBitmapSleepScreen

**Reality (from SleepActivity.h:16):**
```cpp
void renderBitmapSleepScreen(const Bitmap& bitmap) const;  // Private method
```

**Issue:** Private method. Todoist can't call it directly.

**Fix required:** Handle snapshot independently; SleepActivity checks for it on boot.

### 1.6 UITheme getMetrics()

**Reality:** BaseTheme.h does **not** expose `getMetrics()`. 

**Fix required:** Use BaseTheme's `ThemeMetrics` constant or hard-code dimensions for Xteink X4 (480×800).

### 1.7 Weather Integration (REMOVED from v1)

**Current draft adds:** WeatherClient, WeatherTypes, 400+ lines of weather logic.

**v1 decision:** Remove entirely. Todoist stays focused on tasks only.

### 1.8 Configuration Schema Bloat

**Current draft:** 20+ fields (locations, timezone, weather cache, etc.)

**v1 schema:** Only:
- `api_token`
- `sleep_screen_enabled`
- `date_filter` (today | week | all)
- `show_overdue` (show or hide)

Much simpler, focused on the feature request.

---

## Part 2: Todoist Feature Specification (v1)

### 2.1 Scope Statement

**Feature Goal:**
- User opens Todoist from the home screen
- App fetches tasks via API (over WiFi)
- Renders a scrollable task list **filtered by date and overdue status**
- Optionally captures a snapshot for the sleep screen

**What v1 includes:**
- API token configuration
- Date filtering: **today** (default) | **next 7 days** | **all tasks**
- Overdue filtering: **show** (default) | **hide**
- Read-only task display (title, due date/time, priority)
- Scroll navigation
- Refresh capability
- Sleep screen snapshot (optional)

**What v1 does NOT include:**
- Weather integration
- Geocoding / automatic timezone detection
- Overdue task grouping (separate section)
- Design mode selection
- Location/timezone auto-detection
- Temperature units

All of these are deferred to v2.

### 2.2 Task Data Model

```cpp
struct TodoistTask {
  std::string id;              // Task ID from API
  std::string content;         // Task title
  int priority = 4;            // 1=highest, 4=lowest
  std::string due;             // ISO 8601: "2024-10-15T14:30:00Z" or "2024-10-15"
  bool isOverdue = false;      // Calculated client-side
  bool isRecurring = false;    // If recurring, show [R] badge
  
  time_t getDueTimestamp() const;  // Parse ISO to Unix timestamp
  std::string getDueDateStr() const;   // "Today", "Mon 15", "Fri", etc.
  std::string getDueTimeStr() const;   // "14:30" or empty if date-only
};
```

### 2.3 File Structure (Minimal)

```
src/integrations/todoist/
├── TodoistConfig.h        
├── TodoistConfig.cpp      
├── TodoistClient.h        
├── TodoistClient.cpp
└── TodoistTask.h          

src/activities/integrations/
├── TodoistActivity.h      
└── TodoistActivity.cpp

src/activities/home/
└── HomeActivity.cpp       (modified: add Todoist menu entry)

src/activities/boot_sleep/
└── SleepActivity.cpp      (modified: add Todoist pre-check)

lib/I18n/translations/
└── english.yaml           (modified: add STR_TODOIST_* strings)

src/main.cpp              (modified: boot-time config load)
```

### 2.4 Config Model (v1)

**File:** `/.crosspoint/todoist.json`

```json
{
  "api_token": "user's 40-char personal token",
  "sleep_screen_enabled": false,
  "date_filter": "today",
  "show_overdue": true
}
```

**Date filter options:**
- `"today"` — Todoist query: `today`
- `"week"` — Todoist query: `due < +7 days`
- `"all"` — Todoist query: (none, fetch all)

**Overdue filter:**
- `true` — Show all tasks including overdue
- `false` — Hide overdue tasks (filter client-side)

**Example config getter:**
```cpp
class TodoistConfig {
  std::string apiToken;
  bool sleepScreenEnabled = false;
  enum DateFilter { TODAY, WEEK, ALL };
  enum OverdueFilter { SHOW, HIDE };
  
  DateFilter getDateFilter() const;
  OverdueFilter getOverdueFilter() const;
  // ... setters
};
```

### 2.5 Activity Flow

```
TodoistActivity::onEnter()
  ├─ Load config (token, date_filter, show_overdue)
  ├─ Is WiFi connected?
  │  ├─ NO  → Launch WifiSelectionActivity
  │  └─ YES → Proceed to fetch
  │
  ├─ Render "Fetching..." popup
  ├─ Sync NTP
  ├─ Build Todoist API query based on date_filter
  │  Example: "filter=today" or "filter=due < +7 days"
  │
  ├─ TodoistClient::fetch(token, filterQuery)
  │  └─ HTTPS GET https://api.todoist.com/rest/v2/tasks?filter=...
  │     (Bearer token auth, TLS)
  │
  ├─ Parse JSON → std::vector<TodoistTask>
  ├─ Calculate isOverdue for each task (client-side)
  ├─ Apply show_overdue filter
  ├─ Sort tasks (by due date, then by time)
  ├─ Cap at 64 tasks (memory limit)
  ├─ Save snapshot BMP if sleep_screen_enabled
  ├─ Render task list to screen
  └─ Enter loop()

loop()
  ├─ mappedInput.wasReleased(Back)     → finish()
  ├─ mappedInput.wasReleased(Confirm)  → open filter menu
  ├─ mappedInput.wasReleased(Up)       → scroll -1
  └─ mappedInput.wasReleased(Down)     → scroll +1

render()
  └─ Draw task list (header + rows + filters)
```

### 2.6 Filtering UI (In-Activity)

**Menu access:** Press Confirm to open filter modal

```
┌─────────────────────────────┐
│ Filters                     │
├─────────────────────────────┤
│ Date: [Today ] Week All     │  ← Toggle between options
│ Show overdue: [Yes] [No]    │  ← Toggle
├─────────────────────────────┤
│ [Apply]  [Cancel]           │
└─────────────────────────────┘
```

After apply → re-fetch with new filter, re-render.

### 2.7 Rendering (Simple List View)

```
  ┌──────────────────────────────┐
  │ Todoist                      │
  │ Today | 5 tasks              │
  │ ──────────────────────────────
  │ [P1] Task 1                  │
  │ [P2] Task 2 (09:30)          │
  │ [P3] Task 3 (Yesterday) [OV] │  ← "OV" = Overdue
  │ [P4] Task 4                  │
  │ [P4] Task 5 (Mon 15)         │
  │ ──────────────────────────────
  │ Back  Filters Scroll Scroll  │
  └──────────────────────────────┘
```

**Components:**
- Header: title + filter summary
- Task rows: priority badge + title + due date/time
- Footer: button hints

---

## Part 3: Implementation Plan (Phased with Filters)

### Phase 1: Config + Models (1 PR)

**Deliverables:**
- `TodoistConfig` singleton (load/save/getters)
  - Include `date_filter` and `overdue_filter` getters/setters
  - JSON serialization for these fields
- `TodoistTask` POD with helper methods
- i18n strings (`STR_TODOIST_*`, `STR_FILTER_*`)
- Boot-time config load and initialization

**Files:** 3 new, 2 modified
**Scope:** ~500 lines
**Verification:** Clean build, config persists correctly

---

### Phase 2: API Client (1 PR)

**Deliverables:**
- `TodoistClient::fetch(token, dateFilterQuery)` 
  - Build filter string based on config
  - Query: `today` | `due < +7 days` | (empty for all)
  - Fetch + parse JSON into `std::vector<TodoistTask>`
- `TodoistTask::calculateOverdue()` (client-side, using system time)
- Error mapping (InvalidToken, RateLimited, NetworkError, ParseError)
- No UI, no WiFi management

**Files:** 2 new
**Scope:** ~400 lines
**Verification:** Clean build, test with real API key (optional)

---

### Phase 3: Activity + Filtering (1 PR)

**Deliverables:**
- `TodoistActivity` 
  - Fetch tasks with current date filter
  - Apply overdue filter (client-side)
  - Sort by due date
  - Render task list with dates/times
  - Handle scroll, back, confirm (filter menu)
- Filter modal UI (simple: toggle date filter, toggle overdue)
- Input handling (back, confirm, up/down)
- Home screen menu entry

**Files:** 2 new, 1 modified (HomeActivity)
**Scope:** ~700 lines
**Verification:** Clean build, manual test on hardware

---

### Phase 4: Sleep Integration (1 PR)

**Deliverables:**
- Snapshot capture (render-to-BMP on successful fetch)
- `SleepActivity` pre-check (blit snapshot if toggle on)
- Snapshot cache management (replace on fetch)

**Files:** 1 modified (SleepActivity)
**Scope:** ~200 lines
**Verification:** Manual test: sleep with and without snapshot

**Total:** 4 focused PRs, ~1800 lines added (excluding comments), no scope creep.

---

## Part 4: Detailed Code Fixes

### Fix 1: Config with Filters

```cpp
// src/integrations/todoist/TodoistConfig.h
#pragma once
#include <string>

class TodoistConfig {
 public:
  enum class DateFilter {
    TODAY = 0,
    WEEK = 1,
    ALL = 2
  };

  enum class OverdueFilter {
    SHOW = 0,
    HIDE = 1
  };

  static TodoistConfig& getInstance();

  bool load();
  bool save();
  bool forget();

  const std::string& getApiToken() const { return apiToken_; }
  bool setApiToken(const std::string& token);

  bool isSleepScreenEnabled() const { return sleepScreenEnabled_; }
  bool setSleepScreenEnabled(bool enabled);

  DateFilter getDateFilter() const { return dateFilter_; }
  void setDateFilter(DateFilter filter) { dateFilter_ = filter; save(); }

  OverdueFilter getOverdueFilter() const { return overdueFilter_; }
  void setOverdueFilter(OverdueFilter filter) { overdueFilter_ = filter; save(); }

  // Helper: build Todoist query string
  std::string buildFilterQuery() const;

 private:
  TodoistConfig() = default;

  std::string apiToken_;
  bool sleepScreenEnabled_ = false;
  DateFilter dateFilter_ = DateFilter::TODAY;
  OverdueFilter overdueFilter_ = OverdueFilter::SHOW;
};

#define TODOIST_CONFIG TodoistConfig::getInstance()
```

### Fix 2: Overdue Calculation

```cpp
// src/integrations/todoist/TodoistTask.h
#pragma once
#include <string>
#include <ctime>

struct TodoistTask {
  std::string id;
  std::string content;
  int priority = 4;
  std::string due;  // ISO 8601
  bool isOverdue = false;
  bool isRecurring = false;

  // Parse due string and calculate overdue status
  void calculateOverdue();
  
  time_t getDueTimestamp() const;
  std::string getDueDateStr() const;  // "Today", "Mon 15", etc.
  std::string getDueTimeStr() const;  // "14:30" or empty
};
```

### Fix 3: Activity with Filter Menu

```cpp
// src/activities/integrations/TodoistActivity.h
#pragma once
#include "../Activity.h"
#include "integrations/todoist/TodoistConfig.h"
#include "integrations/todoist/TodoistTask.h"
#include <vector>

class TodoistActivity final : public Activity {
 public:
  explicit TodoistActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Todoist", renderer, mappedInput) {}
  
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum State {
    LOADING,
    FETCHING,
    DISPLAYING,
    FILTER_MENU,
    ERROR,
  };

  State state_ = LOADING;
  std::vector<TodoistTask> tasks_;
  int scrollIndex_ = 0;
  int selectedFilterIdx_ = 0;  // In filter menu: 0=date, 1=overdue
  std::string errorMessage_;

  void performFetch();
  void applyFilters();
  void renderLoading() const;
  void renderFetching() const;
  void renderTaskList() const;
  void renderFilterMenu() const;
  void renderError() const;
  void captureSnapshot();
};
```

### Fix 4: Input Handling

```cpp
// In TodoistActivity::loop()
void TodoistActivity::loop() {
  if (state_ == DISPLAYING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state_ = FILTER_MENU;
      selectedFilterIdx_ = 0;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      scrollIndex_ = std::min(scrollIndex_ + 1, (int)tasks_.size() - 1);
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      scrollIndex_ = std::max(scrollIndex_ - 1, 0);
      requestUpdate();
    }
    return;
  }

  if (state_ == FILTER_MENU) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state_ = DISPLAYING;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Apply and re-fetch
      state_ = FETCHING;
      performFetch();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedFilterIdx_ = (selectedFilterIdx_ + 1) % 2;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedFilterIdx_ = (selectedFilterIdx_ - 1 + 2) % 2;
      requestUpdate();
    }
    return;
  }
}
```

### Fix 5: Simple Rendering

```cpp
// In TodoistActivity::render()
void TodoistActivity::render(RenderLock&&) {
  if (state_ == FETCHING) {
    renderFetching();
    return;
  }
  if (state_ == DISPLAYING) {
    renderTaskList();
    return;
  }
  if (state_ == FILTER_MENU) {
    renderFilterMenu();
    return;
  }
  if (state_ == ERROR) {
    renderError();
    return;
  }
}

void TodoistActivity::renderTaskList() const {
  renderer.clearScreen();
  
  // Header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Todoist", true, EpdFontFamily::BOLD);
  
  // Filter summary
  char summary[64];
  snprintf(summary, sizeof(summary), "%s | %zu tasks",
           TODOIST_CONFIG.getDateFilter() == TodoistConfig::DateFilter::TODAY ? "Today" :
           TODOIST_CONFIG.getDateFilter() == TodoistConfig::DateFilter::WEEK ? "Week" : "All",
           tasks_.size());
  renderer.drawText(UI_10_FONT_ID, 20, 50, summary);
  
  // Task rows
  int y = 80;
  const int taskRowHeight = 35;
  for (size_t i = scrollIndex_; i < tasks_.size() && y < 700; ++i) {
    const auto& task = tasks_[i];
    
    // Priority badge
    char prio[4];
    snprintf(prio, sizeof(prio), "[P%d]", task.priority);
    renderer.drawText(UI_10_FONT_ID, 20, y, prio, false);
    
    // Title
    renderer.drawText(UI_10_FONT_ID, 70, y, task.content.c_str(), false);
    
    // Due date
    char due[32];
    snprintf(due, sizeof(due), "%s", task.getDueDateStr().c_str());
    renderer.drawText(UI_10_FONT_ID, 400, y, due, false);
    
    // Overdue badge
    if (task.isOverdue && TODOIST_CONFIG.getOverdueFilter() == TodoistConfig::OverdueFilter::SHOW) {
      renderer.drawText(UI_10_FONT_ID, 420, y + 15, "[OV]", false);
    }
    
    y += taskRowHeight;
  }
  
  // Button hints
  const auto labels = mappedInput.mapLabels("Back", "Filters", "Scroll", "Scroll");
  renderer.drawText(UI_10_FONT_ID, 20, 750, labels.btn1, false);
  renderer.drawText(UI_10_FONT_ID, 120, 750, labels.btn2, false);
  renderer.drawText(UI_10_FONT_ID, 280, 750, labels.btn3, false);
  renderer.drawText(UI_10_FONT_ID, 380, 750, labels.btn4, false);
  
  renderer.displayBuffer();
}

void TodoistActivity::renderFilterMenu() const {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 20, "Filters", true, EpdFontFamily::BOLD);
  
  int y = 100;
  
  // Date filter options
  renderer.drawText(UI_10_FONT_ID, 40, y, "Date filter:");
  y += 40;
  
  const char* dateOptions[] = {"Today", "Week", "All"};
  for (int i = 0; i < 3; ++i) {
    bool selected = (TODOIST_CONFIG.getDateFilter() == (TodoistConfig::DateFilter)i) && 
                    (selectedFilterIdx_ == 0);
    if (selected) {
      renderer.fillRect(35, y - 2, 410, 30);
    }
    renderer.drawText(UI_10_FONT_ID, 50, y, dateOptions[i], false);
    y += 35;
  }
  
  y += 20;
  
  // Overdue filter options
  renderer.drawText(UI_10_FONT_ID, 40, y, "Show overdue:");
  y += 40;
  
  const char* overdueOptions[] = {"Yes", "No"};
  for (int i = 0; i < 2; ++i) {
    bool selected = (TODOIST_CONFIG.getOverdueFilter() == (TodoistConfig::OverdueFilter)i) && 
                    (selectedFilterIdx_ == 1);
    if (selected) {
      renderer.fillRect(35, y - 2, 410, 30);
    }
    renderer.drawText(UI_10_FONT_ID, 50, y, overdueOptions[i], false);
    y += 35;
  }
  
  // Button hints
  const auto labels = mappedInput.mapLabels("Back", "Apply", "Prev", "Next");
  renderer.drawText(UI_10_FONT_ID, 20, 750, labels.btn1, false);
  renderer.drawText(UI_10_FONT_ID, 120, 750, labels.btn2, false);
  renderer.drawText(UI_10_FONT_ID, 280, 750, labels.btn3, false);
  renderer.drawText(UI_10_FONT_ID, 380, 750, labels.btn4, false);
  
  renderer.displayBuffer();
}
```

---

## Part 5: Testing Strategy

### Build Verification
```bash
pio run                    # Default build
pio check                  # No new linting issues
```

### Hardware Tests
1. **Boot:** Device boots, home screen renders
2. **No config:** Open Todoist → "No token configured"
3. **Invalid token:** → "Invalid token" after fetch
4. **Valid token + WiFi:** → task list renders
5. **Filter: Today** → Show only today's tasks
6. **Filter: Week** → Show next 7 days
7. **Filter: All** → Show all tasks
8. **Overdue: Hide** → Overdue tasks disappear
9. **Overdue: Show** → Overdue tasks visible with [OV] badge
10. **Scroll:** Up/down navigation works
11. **Filter menu:** Open, toggle, apply
12. **Sleep snapshot:** Capture → power down → wake → see Todoist snapshot
13. **Persist config:** Change filters → power down → wake → filters retained

---

## Part 6: Summary of Changes

**Remove from current PR:**
- All weather integration (WeatherClient, WeatherTypes, etc.)
- Geocoding and timezone detection
- Design mode selectors
- Temperature units
- Location auto-detection
- Weather cache
- KeyboardEntryActivity
- 400+ lines of unrelated logic

**Add (v1):**
- TodoistConfig with `date_filter` and `overdue_filter`
- TodoistTask with overdue calculation
- TodoistClient API fetch
- TodoistActivity with filter menu
- i18n strings
- SleepActivity pre-check
- HomeActivity menu entry

**Expected scope:**
- ~1800 lines across 4 focused PRs
- 5 new files, 5 modified files
- Clear, reviewable, testable increments

**Config now supports:**
```json
{
  "api_token": "...",
  "sleep_screen_enabled": false,
  "date_filter": "today",
  "show_overdue": true
}
```

---

## Recommended Approach

1. **Commit this plan** to `feat/todoist-integration-minimal`
2. **Phase 1 PR:** Config + Models
3. **Phase 2 PR:** API Client
4. **Phase 3 PR:** Activity + Filter UI
5. **Phase 4 PR:** Sleep Integration
6. **Merge to master** once all phases pass review and hardware tests

This keeps each PR small, focused, and reviewable while delivering the full feature with date and overdue filtering.
