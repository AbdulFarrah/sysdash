#ifndef PROCESS_H
#define PROCESS_H

#include "state.h"
#include <sys/types.h>

#define MAX_PROCS 1024
#define COMM_LEN  64
#define EXE_NAME_LEN 256
#define MAX_GROUPS 256
#define HIST_LEN 20
#define MAX_VISIBLE_ROWS (MAX_PROCS + MAX_GROUPS)

typedef enum {
    ROW_GROUP,
    ROW_PROC
} RowType;

typedef struct {
    RowType type;
    int group_index;   // valid if ROW_GROUP
    int pid;           // valid if ROW_PROC
} VisibleRow;


typedef struct {
    char name[APP_KEY_LEN];
    int expanded;
} PrevGroup;

typedef struct {
    char name[APP_KEY_LEN];
    int pids[MAX_PROCS];
    int pid_count;
    double cpu_total;
    unsigned long  rss_total;
    int expanded;   // 0 = collapsed, 1 = expanded
} ProcGroup;

typedef struct {
    uid_t uid;
    int tty_nr;
    int session;
    int pid;
    char name[COMM_LEN];
    char executable[EXE_NAME_LEN];
    char app_key[APP_KEY_LEN];
    unsigned long rss_kb;
    double cpu_percent;
    unsigned long long utime;
    unsigned long long stime;
    unsigned long start_time;
    unsigned char cpu_hist[HIST_LEN];
    unsigned char mem_hist[HIST_LEN];
    int hist_pos;
    double cpu_instant;
    char state;
    int threads;
    int fd_count;
    int memory_valid;
    int fd_valid;
} ProcInfo;

void process_init(void);
void process_update(AppState *state);
void process_draw(AppState *state);
/* process list accessors (read-only) */
int find_proc_index_by_pid(int pid);
/* visible row navigation (read-only) */
int process_visible_row_count(void);
void process_sync_selection_from_row(AppState *state);
void process_set_group_expanded_by_name(const char *name, int expanded);
void process_commit_group_state(void);
int process_find_group_by_pid(int pid);
void process_collapse_selected(AppState *state);
void draw_process_panel(AppState *state);

#endif
