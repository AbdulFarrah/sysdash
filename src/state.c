#include "state.h"

void state_init(AppState *state)
{
    state->running = 1;
    state->active_tab = TAB_HARDWARE;
    state->needs_resize = 1;
    state->show_all_processes = 0;  // default: foreground only
    state->selected_pid = -1;
    state->proc_action = PROC_ACTION_NONE;
    state->selected_row = 0;
    state->process_panel_open = 0;
    state->panel_pid = -1;
    state->panel_width = 0;
}

