#include <ncursesw/ncurses.h>
#include <locale.h>
#include "ui.h"
#include "hardware.h"
#include "process.h"

static void draw_tab_bar(AppState *state)
{
    mvhline(0, 0, ' ', state->cols);

    const char *tabs[] = { " Hardware ", " Processes " };

    int x = 2;
    for (Tab i = 0; i < TAB_COUNT; i++) {
        if (state->active_tab == i)
            attron(A_REVERSE);

        mvprintw(0, x, "%s", tabs[i]);

        if (state->active_tab == i)
            attroff(A_REVERSE);

        x += 14;
    }
}

static void draw_status_bar(AppState *state)
{
    attron(A_REVERSE);
    mvhline(state->rows - 1, 0, ' ', state->cols);
  if (state->active_tab == TAB_HARDWARE) {

        mvprintw(
            state->rows - 1,
            2,
            "Tab/←→ Switch | q Quit | Hardware"
        );

    } else { /* TAB_PROCESSES */

        mvprintw(
            state->rows - 1,
            2,
            "Tab/←→ Switch | a Toggle (%s) | Enter Expand | Backspace Collapse | x Side Panel | Processes",
            state->show_all_processes ? "ALL" : "FOREGROUND"
        );
    }
    attroff(A_REVERSE);
}

void ui_init(void)
{
    setlocale(LC_ALL, "");

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    start_color();
    use_default_colors();
    timeout(1000);   // getch() waits max 1000 ms
}

void ui_shutdown(void)
{
    endwin();
}

void ui_draw(AppState *state)
{
    if (state->needs_resize) {
        getmaxyx(stdscr, state->rows, state->cols);
        clear();
        state->needs_resize = 0;
    }

    clear();
    draw_tab_bar(state);
    wnoutrefresh(stdscr);

    int panel_w = 0;

    if (state->process_panel_open) {
        panel_w = state->cols / 3;
        if (panel_w < 30)
            panel_w = 30;
        state->panel_width = panel_w;
    } else {
        state->panel_width = 0;
    }

    if (state->active_tab == TAB_HARDWARE) {
        hardware_draw(state);
    } else {
        int saved_cols = state->cols;

        if (state->process_panel_open)
            state->cols -= state->panel_width;

        process_draw(state);

        state->cols = saved_cols;

        if (state->process_panel_open &&
            state->panel_pid != -1) {
            draw_process_panel(state);
        }
    }
    draw_status_bar(state);
    wnoutrefresh(stdscr);
    doupdate();
}

