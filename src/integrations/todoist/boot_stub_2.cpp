#include <Arduino.h>

#include "TodoistConfig.h"

void todoist_boot_init_alt() {
  TODOIST_CONFIG.loadFromFile();
}
