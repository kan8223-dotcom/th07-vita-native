#pragma once
#include <cstdint>
void th07_vita_init();
bool th07_vita_running();
uint16_t th07_vita_buttons();
// Runtime log ux0:data/th07/vita-runtime.log, started fresh at every launch. After start-up
// a separate thread appends the queued lines, so no file write lands in a game frame.
void th07_vita_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void th07_vita_presented();
// Ends the main loop after an unrecoverable error (logged with the reason).
void th07_vita_stop(const char *reason);
// Writes the queued log lines and stops the log thread (before the process exits).
void th07_vita_shutdown();
// Boot steps before the game starts (vita/Startup.cpp).
void th07_vita_startup();
