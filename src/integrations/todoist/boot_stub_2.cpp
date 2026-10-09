#include <Arduino.h>
# include "integrations/todoist/TodoistConfig.h"

void setup() {
  // This file is intentionally a no-op placeholder so the project has a compile-safe
  // place to keep the Todoist config boot initialization visible during the minimal phase.
  TODOIST_CONFIG.loadFromFile();
}

void loop() {
  delay(10);
}
