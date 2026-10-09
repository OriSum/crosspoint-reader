#include "TodoistConfig.h"

namespace todoist_stub {
void init() {
  TODOIST_CONFIG.loadFromFile();
}
}  // namespace todoist_stub
