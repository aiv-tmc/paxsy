#ifndef PAXSY_COLOR_H
#define PAXSY_COLOR_H

/*
 * color.h - ANSI colour helpers shared by the driver.
 * The globals are empty strings when colour is disabled (NO_COLOR is
 * set or stderr is not a terminal), so call sites can concatenate
 * them unconditionally. color_setup() must run once before printing.
 */

void color_setup(void);

extern const char *c_red;     /* bright red:   errors, fatal text      */
extern const char *c_green;   /* bright green: success, confirmations  */
extern const char *c_yellow;  /* bright yellow: warnings               */
extern const char *c_cyan;    /* bright cyan:  file names, accents     */
extern const char *c_dim;     /* dim/bright-black: gutters, hints      */
extern const char *c_reset;   /* resets all attributes                 */

#endif /* PAXSY_COLOR_H */
