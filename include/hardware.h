#ifndef HARDWARE_H
#define HARDWARE_H

#include "state.h"

void hardware_init(void);
void hardware_draw(AppState *state);
void hardware_handle_input(int ch, AppState *state);

#endif
