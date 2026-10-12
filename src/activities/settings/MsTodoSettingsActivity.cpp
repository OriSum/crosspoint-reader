#include "MsTodoSettingsActivity.h"

#include <I18n.h>

#include <cstdio>

#include "components/UITheme.h"
#include "integrations/mstodo/MsTodoConfig.h"

namespace fui = freeink::ui;

// Row labels never change, so they're set once in the constructor; only the
// live value text is refreshed in buildScreen().
namespace {
const StrId rowNames[MsTodoSettingsActivity::kItemCount] = {StrId::STR_MSTODO_SLEEP_SCREEN,
                                                            StrId::STR_MSTODO_SHOW_OVERDUE,
                                                            StrId::STR_MSTODO_FUTURE_DAYS, StrId::STR_MSTODO_SIGNOUT};

// Ladder cycled by the future-days row. Values outside it (hand-edited file)
// fall back to the first entry on the next press.
constexpr int kFutureDaysLadder[] = {1, 3, 7, 14, 30};
constexpr int kFutureDaysLadderCount = sizeof(kFutureDaysLadder) / sizeof(kFutureDaysLadder[0]);

// "1 day" / "N days" for the future-days row value.
void formatFutureDays(int days, std::string& out) {
  char buf[24];
  snprintf(buf, sizeof(buf), I18N.get(days == 1 ? StrId::STR_MSTODO_DAY_SINGULAR : StrId::STR_MSTODO_DAY_PLURAL), days);
  out = buf;
}
}  // namespace

MsTodoSettingsActivity::MsTodoSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("MsTodoSettings", renderer, mappedInput) {
  for (int i = 0; i < kItemCount; i++) {
    rowItems_[i].label = I18N.get(rowNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

int MsTodoSettingsActivity::listCount() const { return kItemCount; }

const char* MsTodoSettingsActivity::headerTitle() const { return tr(STR_MSTODO); }

void MsTodoSettingsActivity::activateIndex(const int index) {
  // Activation repaints a new value; a lingering flash would gray an
  // unrelated row.
  app.clearTapFlash();
  switch (index) {
    case 0:
      MSTODO_CONFIG.setSleepScreenEnabled(!MSTODO_CONFIG.isSleepScreenEnabled());
      return;
    case 1:
      MSTODO_CONFIG.setShowOverdue(!MSTODO_CONFIG.isShowOverdue());
      return;
    case 2: {
      // Cycle the ladder; wrap 30 -> 1.
      const int cur = MSTODO_CONFIG.getFutureDays();
      int next = kFutureDaysLadder[0];
      for (int i = 0; i < kFutureDaysLadderCount; ++i) {
        if (kFutureDaysLadder[i] == cur) {
          next = kFutureDaysLadder[(i + 1) % kFutureDaysLadderCount];
          break;
        }
      }
      MSTODO_CONFIG.setFutureDays(next);
      return;
    }
    case 3:
      // Sign out clears the session and the snapshot but keeps the display
      // preferences, so re-signing-in does not reset them.
      MSTODO_CONFIG.signOut();
      return;
  }
}

void MsTodoSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Labels/actionValue were set once in the constructor; only the live value
  // text needs refreshing here, by assigning into the existing rowValues_
  // strings (no array growth) rather than building a new vector per render.
  for (int i = 0; i < kItemCount; i++) {
    switch (i) {
      case 0:
        rowValues_[i] = std::string(MSTODO_CONFIG.isSleepScreenEnabled() ? "On" : "Off");
        break;
      case 1:
        rowValues_[i] = std::string(MSTODO_CONFIG.isShowOverdue() ? "On" : "Off");
        break;
      case 2:
        formatFutureDays(MSTODO_CONFIG.getFutureDays(), rowValues_[i]);
        break;
      case 3:
        rowValues_[i].clear();
        break;
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }
  GUI.setCheckboxRow(rowItems_[0], MSTODO_CONFIG.isSleepScreenEnabled());
  GUI.setCheckboxRow(rowItems_[1], MSTODO_CONFIG.isShowOverdue());

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(kItemCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Label at the value's font size: both sides of the row read as one unit.
  // maxLines=2 also marks the style caller-owned (see textStyleUnset).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
