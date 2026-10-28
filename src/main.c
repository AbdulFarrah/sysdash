#define _POSIX_C_SOURCE 200809L

#include <ncursesw/ncurses.h>
#include "state.h"
#include "ui.h"
#include "input.h"
#include "hardware.h"
#include "process.h"
#include <time.h>

int main(void)
{
    AppState state;
    struct timespec last_process_update = {0, 0};
    int have_process_update = 0;
    state_init(&state);
    ui_init();
    hardware_init();
    process_init();

    while (state.running) {
        ui_draw(&state);
        int ch = getch();
        handle_input(ch, &state);
        if (state.active_tab == TAB_HARDWARE)
            hardware_handle_input(ch, &state);
        if (state.running && state.active_tab == TAB_PROCESSES) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
                double elapsed = (double)(now.tv_sec - last_process_update.tv_sec) +
                    (double)(now.tv_nsec - last_process_update.tv_nsec) / 1000000000.0;
                if (!have_process_update || elapsed >= 1.0) {
                    process_update(&state);
                    last_process_update = now;
                    have_process_update = 1;
                }
            } else {
                state.readings_stale = 1;
            }
        }

    }

    ui_shutdown();
    return 0;
}

