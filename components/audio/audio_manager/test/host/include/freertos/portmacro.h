#pragma once

typedef int portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock)      do { (void)(lock); } while (0)
#define portEXIT_CRITICAL(lock)       do { (void)(lock); } while (0)
