#include "TodoistSettingsActivity.h"

#include <I18n.h>

#include "components/UITheme.h"
#include "integrations/todoist/TodoistConfig.h"

namespace fui = freeink::ui;

namespace {

const char* orientationLabel(GfxRenderer::Orientation o) {
  switch (o) {
    case GfxRenderer::Orientation::Portrait:
      return "Portrait";
    case GfxRenderer::Orientation::PortraitInverted:
      return "Portrait inverted";
    case GfxRenderer::Orientation::LandscapeClockwise:
      return "Landscape CW";
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      return "Landscape CCW";
  }
  return "Portrait";
}

}  // namespace

// Row labels never change, so they're set once in the constructor; only the
// live value text is refreshed in buildScreen().
namespace {
const StrId rowNames[TodoistSettingsActivity::kItemCount] = {
    StrId::STR_TODOIST_SLEEP_SCREEN, StrId::STR_TODOIST_ACTIVITY_ORIENTATION, StrId::STR_TODOIST_SNAPSHOT_ORIENTATION,
    StrId::STR_TODOIST_FORGET};
}  // namespace

TodoistSettingsActivity::TodoistSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("TodoistSettings", renderer, mappedInput) {
  for (int i = 0; i < kItemCount; i++) {
    rowItems_[i].label = I18N.get(rowNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

int TodoistSettingsActivity::listCount() const { return kItemCount; }

const char* TodoistSettingsActivity::headerTitle() const { return tr(STR_TODOIST); }

void TodoistSettingsActivity::activateIndex(const int index) {
  // Activation repaints a new value; a lingering flash would gray an
  // unrelated row.
  app.clearTapFlash();
  switch (index) {
    case 0:
      TODOIST_CONFIG.setSleepScreenEnabled(!TODOIST_CONFIG.isSleepScreenEnabled());
      return;
    case 1:
      TODOIST_CONFIG.setActivityOrientation(nextOrientation(TODOIST_CONFIG.getActivityOrientation()));
      return;
    case 2:
      TODOIST_CONFIG.setSnapshotOrientation(nextOrientation(TODOIST_CONFIG.getSnapshotOrientation()));
      return;
    case 3:
      TODOIST_CONFIG.forget();
      return;
  }
}

GfxRenderer::Orientation TodoistSettingsActivity::nextOrientation(GfxRenderer::Orientation current) const {
  // Cycle in physical clockwise rotation: each step rotates the device 90° CW.
  switch (current) {
    case GfxRenderer::Orientation::Portrait:
      return GfxRenderer::Orientation::LandscapeClockwise;
    case GfxRenderer::Orientation::LandscapeClockwise:
      return GfxRenderer::Orientation::PortraitInverted;
    case GfxRenderer::Orientation::PortraitInverted:
      return GfxRenderer::Orientation::LandscapeCounterClockwise;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
    default:
      return GfxRenderer::Orientation::Portrait;
  }
}

void TodoistSettingsActivity::buildScreen(UiScreen& screen) {
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
        rowValues_[i] = std::string(TODOIST_CONFIG.isSleepScreenEnabled() ? "On" : "Off");
        break;
      case 1:
        rowValues_[i] = std::string(orientationLabel(TODOIST_CONFIG.getActivityOrientation()));
        break;
      case 2:
        rowValues_[i] = std::string(orientationLabel(TODOIST_CONFIG.getSnapshotOrientation()));
        break;
      case 3:
        rowValues_[i].clear();
        break;
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }
  GUI.setCheckboxRow(rowItems_[0], TODOIST_CONFIG.isSleepScreenEnabled());

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
