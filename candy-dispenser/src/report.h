#ifndef HALLOWEEN_REPORT_H_
#define HALLOWEEN_REPORT_H_

#include <stdint.h>

/*
 * Reports every released candy to the dispense counter web server over Wi-Fi.
 * Without Wi-Fi support in the build, or without Wi-Fi/server settings, these
 * functions do nothing.
 */

/* Start the background Wi-Fi/HTTP reporter thread. */
void report_init(void);

/* Queue one dispense event for the web server. Never blocks. */
void report_dispense(uint16_t session);

#endif
