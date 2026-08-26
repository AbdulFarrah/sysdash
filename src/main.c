#include <ncursesw/ncurses.h>
#include "state.h"
#include "ui.h"
#include "input.h"
#include "hardware.h"
#include "process.h"

int main(void)
{
    AppState state;
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
        if (state.active_tab == TAB_PROCESSES) 
        {
            process_update(&state);
            process_handle_input(ch, &state);
        }

    }

    ui_shutdown();
    return 0;
}

