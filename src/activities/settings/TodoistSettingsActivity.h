#pragma once

#include <string>

#include "activities/UiListActivity.h"

/**
 * Submenu for Todoist integration settings.
 * Items (in order): design mode, sleep-screen toggle, activity orientation,
 * snapshot orientation, date filter, overdue filter, GMT offset, date format,
 * temperature unit, location, forget.
 */
class TodoistSettingsActivity final : public UiListActivity {
 public:
  static constexpr int kItemCount = 11;

  TodoistSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

 private:
  // Opens the on-screen keyboard for city entry and, on confirm, geocodes
  // via Open-Meteo + persists. Must only be called when WiFi is already
  // connected — the caller (activateIndex case 9) is responsible for
  // launching WifiSelectionActivity first if not.
  void launchCityEntry();
  GfxRenderer::Orientation nextOrientation(GfxRenderer::Orientation current) const;

  // Row storage: kItemCount is a compile-time constant, so fixed-capacity
  // storage avoids any heap allocation for the row list. Labels are set once
  // in the constructor; buildScreen() only refreshes the live value text
  // (rowValues_) by assigning into the existing strings (no array growth).
  std::string rowValues_[kItemCount];
  freeink::ui::ListItem rowItems_[kItemCount]{};

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
};
