#include <ncursesw/ncurses.h>
#include "input.h"
#include "process.h"

void handle_input(int ch, AppState *state)
{
    switch (ch) {
        case 'q':
        case 'Q':
            state->running = 0;
            break;
        case 'a':
        case 'A':
            if (state->active_tab == TAB_PROCESSES)
                state->show_all_processes = !state->show_all_processes;
            break;
        
        case '\t':
        case KEY_RIGHT:
            state->active_tab =
                (state->active_tab + 1) % TAB_COUNT;
            break;

        case KEY_LEFT:
            state->active_tab =
                (state->active_tab - 1 + TAB_COUNT) % TAB_COUNT;
            break;

        case KEY_RESIZE:
            state->needs_resize = 1;
            break;
        
        case KEY_DOWN:
            if (state->active_tab == TAB_PROCESSES) {
                int max = process_visible_row_count();
                if (state->selected_row + 1 < max) {
                    state->selected_row++;
                    process_sync_selection_from_row(state);
                }
            }
            break;

        case KEY_UP:
            if (state->active_tab == TAB_PROCESSES) {
                if (state->selected_row > 0) {
                    state->selected_row--;
                    process_sync_selection_from_row(state);
                }
            }
            break;



        case '\n':
        case KEY_ENTER:
            if (state->active_tab != TAB_PROCESSES)
                break;

            if (state->selected_group_name[0] != '\0') {
                process_set_group_expanded_by_name(
                    state->selected_group_name,
                    1
                );
                process_commit_group_state();
            }
            break;


        case 8:
        case KEY_BACKSPACE:
        case 127:
            if (state->active_tab == TAB_PROCESSES)
                process_collapse_selected(state);
            break;
        
        case 'x':
        case 'X':
            if (state->active_tab != TAB_PROCESSES)
                break;

            /* toggle side panel */
            if (!state->process_panel_open) {

                /* only open for a child process */
               if (state->selected_pid != -1 && state->selected_group_name[0] == '\0') {
                    state->process_panel_open = 1;
                    state->panel_pid = state->selected_pid;
                }

            } else {
                /* close panel */
                state->process_panel_open = 0;
                state->panel_pid = -1;
            }
            break;

    }
}

