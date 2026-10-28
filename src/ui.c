#include <ncursesw/ncurses.h>
#include <locale.h>
#include <stdio.h>
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
    if (state->rows < 1 || state->cols < 3) return;
    attron(A_REVERSE);
    mvhline(state->rows - 1, 0, ' ', state->cols);
    char status[256];
    if (state->active_tab == TAB_HARDWARE) {
        if (state->readings_stale)
            snprintf(status, sizeof(status), "Tab/←→ | q Quit | Hardware | ~ showing last good");
        else
            snprintf(status, sizeof(status), "Tab/←→ Switch | q Quit | Hardware");
    } else {
        const char *mode = state->show_all_processes ? "ALL" : "CURRENT USER";
        if (state->cols >= 128) {
            snprintf(status, sizeof(status),
                     "Tab/←→ Switch | q Quit | Processes | a Toggle (%s) | Enter Expand | Backspace Collapse | x Side Panel%s",
                     mode, state->readings_stale ? " | ~ last good" : "");
        } else if (state->cols >= 80) {
            snprintf(status, sizeof(status),
                     "Tab/←→ Switch | q Quit | Processes | a User/All | Enter | Bksp | x Panel%s",
                     state->readings_stale ? " | ~ last good" : "");
        } else if (state->cols >= 48) {
            if (state->readings_stale)
                snprintf(status, sizeof(status), "Tab | q Quit | Processes | ~ last good");
            else
                snprintf(status, sizeof(status), "Tab | q Quit | Processes | a User/All | x Panel");
        } else if (state->readings_stale) {
            snprintf(status, sizeof(status), "Tab | q Quit | Process | stale");
        } else {
            snprintf(status, sizeof(status), "Tab | q Quit | Process");
        }
    }
    int width = state->cols - 4;
    if (width < 0) width = 0;
    mvwaddnstr(stdscr, state->rows - 1, 2, status, width);
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

    if (state->process_panel_open && state->cols >= 78 && state->rows >= 22) {
        state->panel_width = state->cols / 3;
        if (state->panel_width < 30)
            state->panel_width = 30;
        if (state->panel_width > state->cols - 45)
            state->panel_width = state->cols - 45;
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

        if (state->process_panel_open && state->panel_width > 0 &&
            state->panel_pid != -1) {
            draw_process_panel(state);
        }
    }
    draw_status_bar(state);
    wnoutrefresh(stdscr);
    doupdate();
}

