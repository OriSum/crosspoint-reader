#include "integrations/todoist/TodoistConfig.h"

#include <Arduino.h>

void setup() {
  HalSystem::begin();

  // Minimal boot initialization. We intentionally keep this side-effect free but allow
  // the Todoist config to load at boot time so the app can access the token immediately.
  // The rest of the app boot flow is unchanged.
  TODOIST_CONFIG.loadFromFile();
}

void loop() {
  delay(10);
}
