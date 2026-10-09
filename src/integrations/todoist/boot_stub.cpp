#include <Arduino.h>

#include "TodoistConfig.h"

void todoist_boot_init() {
  TODOIST_CONFIG.loadFromFile();
}
