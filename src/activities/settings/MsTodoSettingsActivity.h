#pragma once

#include <string>

#include "activities/UiListActivity.h"

/**
 * Submenu for the Microsoft To Do integration settings.
 * Items (in order): sleep-screen toggle, show-overdue toggle, future-days
 * cycler, sign out.
 *
 * There are no orientation rows: the activity and the sleep snapshot are
 * always portrait.
 */
class MsTodoSettingsActivity final : public UiListActivity {
 public:
  static constexpr int kItemCount = 4;

  MsTodoSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

 private:
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
