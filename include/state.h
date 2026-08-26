#ifndef STATE_H
#define STATE_H
#define APP_KEY_LEN 32
typedef enum {
    TAB_HARDWARE = 0,
    TAB_PROCESSES,
    TAB_COUNT
} Tab;

typedef enum {
    PROC_ACTION_NONE = 0,
    PROC_ACTION_EXPAND,
    PROC_ACTION_COLLAPSE
} ProcAction;

typedef struct {
    int running;
    int show_all_processes;
    int rows;
    int cols;
    Tab active_tab;
    int needs_resize;
    int selected_pid;
    char selected_group_name[APP_KEY_LEN];
    ProcAction proc_action;
    int selected_row;   // index into visible_rows
    int process_panel_open;
    int panel_pid;
    int panel_width;
} AppState;

void state_init(AppState *state);

#endif

