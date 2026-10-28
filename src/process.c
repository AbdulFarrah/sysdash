#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE_EXTENDED

#include <ncursesw/ncurses.h>
#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "process.h"
#include <pwd.h>
#include <wchar.h>

static ProcGroup groups[MAX_GROUPS];
static int group_count = 0;

static VisibleRow visible_rows[MAX_VISIBLE_ROWS];
static int visible_row_count = 0;

static PrevGroup prev_groups[MAX_GROUPS];
static int prev_group_count = 0;

static uid_t current_uid = (uid_t)-1;

static unsigned long long prev_proc_times[MAX_PROCS];
static int prev_proc_pids[MAX_PROCS];
static unsigned long prev_proc_start_times[MAX_PROCS];
static double prev_proc_cpu[MAX_PROCS];
static unsigned long prev_proc_rss[MAX_PROCS];
static uid_t prev_proc_uid[MAX_PROCS];
static int prev_proc_threads[MAX_PROCS];
static int prev_proc_fds[MAX_PROCS];
static int prev_proc_memory_valid[MAX_PROCS];
static int prev_proc_fd_valid[MAX_PROCS];
static unsigned char prev_cpu_hist[MAX_PROCS][HIST_LEN];
static unsigned char prev_mem_hist[MAX_PROCS][HIST_LEN];
static int prev_hist_pos[MAX_PROCS];
static int prev_count = 0;

static unsigned long long last_sys_total = 0;
static int cpu_sys_initialized = 0;
static int cpu_count = 1;

static ProcInfo procs[MAX_PROCS];
static int proc_count = 0;


static const wchar_t *bars_w[] = {
    L"▁", L"▂", L"▃", L"▄",
    L"▅", L"▆", L"▇", L"█"};

static unsigned long total_mem_kb = 0;
static int total_mem_valid = 0;
static double last_system_uptime = 0.0;
static int have_system_uptime = 0;

void process_init(void)
{
    proc_count = 0;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 0)
        cpu_count = (int)ncpu;
    else
        cpu_count = 1;
    if (cpu_count < 1)
        cpu_count = 1;
    if (current_uid == (uid_t)-1)
        current_uid = getuid();
}
static void format_seconds(long seconds, char *buf, size_t size)
{
    long h = seconds / 3600;
    long m = (seconds % 3600) / 60;
    long s = seconds % 60;

    snprintf(buf, size, "%02ld:%02ld:%02ld", h, m, s);
}

static int read_proc_stat(int pid, ProcInfo *p)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[4096];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    char *open = strchr(line, '(');
    char *close = strrchr(line, ')');
    char *pid_end = NULL;
    long parsed_pid = strtol(line, &pid_end, 10);
    if (pid_end == line || parsed_pid != pid || *pid_end != ' ' ||
        !open || open != pid_end + 1 || !close || close <= open || close[1] != ' ')
        return -1;

    size_t comm_len = (size_t)(close - open - 1);
    if (comm_len >= sizeof(p->name)) comm_len = sizeof(p->name) - 1;
    memcpy(p->name, open + 1, comm_len);
    p->name[comm_len] = '\0';

    unsigned long long utime, stime, starttime;
    int parsed_session, parsed_tty;
    int fields = sscanf(close + 2,
        "%c %*d %*d %d %d %*d %*u %*u %*u %*u %*u %llu %llu "
        "%*d %*d %*d %*d %*d %*d %llu",
        &p->state, &parsed_session, &parsed_tty, &utime, &stime, &starttime);
    if (fields != 6)
        return -1;

    p->pid = pid;
    p->utime = utime;
    p->stime = stime;
    p->start_time = starttime;
    p->tty_nr = parsed_tty;
    p->session = parsed_session;
    return 0;
}

static int read_proc_mem(int pid, ProcInfo *p, int count_fds)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[256];
    int have_rss = 0, have_threads = 0;

    while (fgets(line, sizeof(line), f))
    {

        if (strncmp(line, "VmRSS:", 6) == 0)
        {
            unsigned long kb = 0;
            if (sscanf(line + 6, "%lu", &kb) == 1) {
                p->rss_kb = kb;
                have_rss = 1;
            }
        }

        else if (strncmp(line, "Threads:", 8) == 0)
        {
            int t = 0;
            if (sscanf(line + 8, "%d", &t) == 1) {
                p->threads = t;
                have_threads = 1;
            }
        }
    }

    fclose(f);
    if (!have_rss || !have_threads) return -1;
    p->memory_valid = 1;

    if (count_fds) {
        snprintf(path, sizeof(path), "/proc/%d/fd", pid);

        DIR *d = opendir(path);
        if (d) {
            int count = 0;
            struct dirent *e;
            while ((e = readdir(d))) {
                if (e->d_name[0] != '.') count++;
            }
            closedir(d);
            p->fd_count = count;
            p->fd_valid = 1;
        } else return -1;
    }
    return 0;
}

static int find_previous_process(int pid, unsigned long start_time)
{
    for (int i = 0; i < prev_count; i++)
    {
        if (prev_proc_pids[i] == pid && prev_proc_start_times[i] == start_time)
            return i;
    }
    return -1;
}

static int cmp_cpu_desc(const void *a, const void *b)
{
    const ProcInfo *pa = a;
    const ProcInfo *pb = b;

    if (pb->cpu_percent > pa->cpu_percent)
        return 1;
    if (pb->cpu_percent < pa->cpu_percent)
        return -1;

    return pa->pid - pb->pid; // stable-ish fallback
}

static unsigned long long read_system_cpu_total(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return 0;

    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    unsigned long long user, nice, system, idle, iowait = 0;
    unsigned long long irq = 0, softirq = 0, steal = 0;
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &user, &nice, &system, &idle,
           &iowait, &irq, &softirq, &steal) < 4)
        return 0;

    return user + nice + system + idle +
           iowait + irq + softirq + steal;
}
static void read_proc_executable(int pid, ProcInfo *p)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

    FILE *f = fopen(path, "r");
    if (!f)
    {
        p->executable[0] = '\0';
        return;
    }

    size_t n = fread(p->executable, 1, EXE_NAME_LEN - 1, f);
    fclose(f);

    if (n == 0)
    {
        p->executable[0] = '\0';
        return;
    }

    char *terminator = memchr(p->executable, '\0', n);
    size_t exe_len = terminator ? (size_t)(terminator - p->executable) : n;
    p->executable[exe_len] = '\0';
    const char *base = strrchr(p->executable, '/');
    if (base) memmove(p->executable, base + 1, strlen(base + 1) + 1);
}
void process_sync_selection_from_row(AppState *state)
{
    if (state->selected_row < 0 ||
        state->selected_row >= visible_row_count)
        return;

    VisibleRow *r = &visible_rows[state->selected_row];

    if (r->type == ROW_GROUP)
    {
        strcpy(state->selected_group_name,
               groups[r->group_index].name);
        state->selected_pid = -1;
    }
    else
    {
        strcpy(state->selected_group_name, "");
        state->selected_pid = r->pid;
    }
}

static int read_proc_uid(int pid, ProcInfo *p)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE *f = fopen(path, "r");
    if (!f)
    {
        p->uid = (uid_t)-1;
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, "Uid:", 4) == 0)
        {
            unsigned int real_uid = 0;
            if (sscanf(line, "Uid:\t%u", &real_uid) != 1) {
                fclose(f);
                p->uid = (uid_t)-1;
                return -1;
            }
            p->uid = (uid_t)real_uid;
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    p->uid = (uid_t)-1;
    return -1;
}
static int is_kernel_thread(const ProcInfo *p)
{
    /* Kernel threads use bracketed names such as [kworker]. */
    if (p->name[0] == '[' && p->name[strlen(p->name) - 1] == ']')
        return 1;

    return 0;
}
static int is_current_user_process(const ProcInfo *p)
{
    /* must belong to current user */
    if (p->uid != current_uid)
        return 0;

    /* remove kernel threads */
    if (is_kernel_thread(p))
        return 0;

    return 1;
}

static void build_app_key(ProcInfo *p)
{
    const char *app = p->executable[0] ? p->executable : p->name;
    strncpy(p->app_key, app, APP_KEY_LEN - 1);
    p->app_key[APP_KEY_LEN - 1] = '\0';
}

static void build_groups(void)
{
    /* preserve previous expanded states */
    prev_group_count = group_count;
    for (int i = 0; i < group_count; i++)
    {
        strncpy(prev_groups[i].name, groups[i].name, APP_KEY_LEN - 1);
        prev_groups[i].name[APP_KEY_LEN - 1] = '\0';
        prev_groups[i].expanded = groups[i].expanded;
    }

    group_count = 0;

    for (int i = 0; i < proc_count; i++)
    {
        ProcInfo *p = &procs[i];

        int g;
        for (g = 0; g < group_count; g++)
        {
            if (strcmp(groups[g].name, p->app_key) == 0)
                break;
        }

        if (g == group_count)
        {
            if (group_count >= MAX_GROUPS)
                break;

            memset(&groups[g], 0, sizeof(ProcGroup));
            strncpy(groups[g].name, p->app_key, APP_KEY_LEN - 1);
            groups[g].name[APP_KEY_LEN - 1] = '\0';

            /* restore expanded state by name */
            for (int pg = 0; pg < prev_group_count; pg++)
            {
                if (strcmp(prev_groups[pg].name, groups[g].name) == 0)
                {
                    groups[g].expanded = prev_groups[pg].expanded;
                    break;
                }
            }

            group_count++;
        }

        if (groups[g].pid_count < MAX_PROCS)
            groups[g].pids[groups[g].pid_count++] = p->pid;

        groups[g].cpu_total += p->cpu_percent;
        if (p->memory_valid && p->rss_kb > 0)
            groups[g].rss_total += p->rss_kb;
    }
}

static ProcInfo *find_proc_by_pid(int pid)
{
    for (int i = 0; i < proc_count; i++)
    {
        if (procs[i].pid == pid)
            return &procs[i];
    }
    return NULL;
}

static int update_total_memory(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return 0;

    char key[64];
    unsigned long value;
    char unit[32];

    int found = 0;
    while (fscanf(f, "%63s %lu %31s", key, &value, unit) == 3)
    {
        if (strcmp(key, "MemTotal:") == 0)
        {
            total_mem_kb = value;
            total_mem_valid = 1;
            found = 1;
            break;
        }
    }

    fclose(f);
    return found;
}
void process_update(AppState *state)
{
    int stale = !update_total_memory();
    DIR *d = opendir("/proc");
    if (!d) {
        state->readings_stale = 1;
        return;
    }
    struct dirent *e;
    unsigned long long sys_total = read_system_cpu_total();
    if (sys_total == 0) stale = 1;
    unsigned long long sys_delta = 0;
    if (cpu_sys_initialized && sys_total >= last_sys_total)
        sys_delta = sys_total - last_sys_total;
    proc_count = 0;
    while ((e = readdir(d)) != NULL && proc_count < MAX_PROCS)
    {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        char *pid_end = NULL;
        long parsed_pid = strtol(e->d_name, &pid_end, 10);
        if (!pid_end || *pid_end != '\0' || parsed_pid <= 0 || parsed_pid > 2147483647L)
            continue;
        int pid = (int)parsed_pid;
        ProcInfo *p = &procs[proc_count];
        memset(p, 0, sizeof(*p));
        if (read_proc_stat(pid, p) == 0)
        {
            int previous = find_previous_process(p->pid, p->start_time);
            if (previous >= 0) {
                p->cpu_percent = prev_proc_cpu[previous];
                memcpy(p->cpu_hist, prev_cpu_hist[previous], sizeof(p->cpu_hist));
                memcpy(p->mem_hist, prev_mem_hist[previous], sizeof(p->mem_hist));
                p->hist_pos = prev_hist_pos[previous];
            }
            read_proc_executable(pid, p);
            if (read_proc_uid(pid, p) != 0) stale = 1;
            if (p->uid == (uid_t)-1 && previous >= 0)
                p->uid = prev_proc_uid[previous];
            build_app_key(p);
            if (!state->show_all_processes && !is_current_user_process(p))
                continue;
            if (read_proc_mem(pid, p, pid == state->panel_pid) != 0) {
                stale = 1;
                if (previous >= 0) {
                    p->rss_kb = prev_proc_rss[previous];
                    p->threads = prev_proc_threads[previous];
                    p->fd_count = prev_proc_fds[previous];
                    p->memory_valid = prev_proc_memory_valid[previous];
                    p->fd_valid = prev_proc_fd_valid[previous];
                }
            }
            unsigned long long proc_total = p->utime + p->stime;
            unsigned long long prev_total = previous >= 0 ? prev_proc_times[previous] : 0;
            double new_value = previous >= 0 ? p->cpu_percent : 0.0;
            if (cpu_sys_initialized && sys_delta > 0 && prev_total > 0)
            {
                unsigned long long proc_delta = proc_total - prev_total;
                new_value =
                    (double)proc_delta * 100.0 * cpu_count /
                    (double)sys_delta;
            }
            p->cpu_instant = new_value;
            p->cpu_percent = previous >= 0 ?
                0.7 * p->cpu_percent + 0.3 * new_value : new_value;
            if (p->cpu_percent < 0)
                p->cpu_percent = 0;
            double max_cpu = 100.0 * cpu_count;
            if (p->cpu_percent > max_cpu)
                p->cpu_percent = max_cpu;
            int level;
            /* CPU history */
            level = (int)(p->cpu_instant * 3.5);
            if (level < 0)
                level = 0;
            if (level > 7)
                level = 7;
            p->cpu_hist[p->hist_pos] = (unsigned char)level;
            /* Record absolute RSS movement for the per-process history graph. */
            long prev_rss = previous >= 0 ? (long)prev_proc_rss[previous] : -1;
            long delta_kb = 0;
            if (prev_rss >= 0)
                delta_kb = (long)p->rss_kb - prev_rss;
            /* absolute movement */
            long abs_delta = labs(delta_kb);
            /* scale small movements */
            double scaled = (double)abs_delta * 0.5;
            /* convert to 0–7 bar level */
            level = (int)(scaled / 256.0);
            if (level < 0)
                level = 0;
            if (level > 7)
                level = 7;
            p->mem_hist[p->hist_pos] = (unsigned char)level;
            /* advance ring buffer */
            p->hist_pos = (p->hist_pos + 1) % HIST_LEN;
            proc_count++;
        }
    }
    closedir(d);
    if (sys_total > 0) {
        last_sys_total = sys_total;
        cpu_sys_initialized = 1;
    }
    if (proc_count > 1)
    {
        qsort(procs, (size_t)proc_count, sizeof(ProcInfo), cmp_cpu_desc);
    }
    prev_count = proc_count;
    for (int i = 0; i < proc_count; i++)
    {
        prev_proc_pids[i] = procs[i].pid;
        prev_proc_times[i] = procs[i].utime + procs[i].stime;
        prev_proc_start_times[i] = procs[i].start_time;
        prev_proc_cpu[i] = procs[i].cpu_percent;
        prev_proc_rss[i] = procs[i].rss_kb;
        prev_proc_uid[i] = procs[i].uid;
        prev_proc_threads[i] = procs[i].threads;
        prev_proc_fds[i] = procs[i].fd_count;
        prev_proc_memory_valid[i] = procs[i].memory_valid;
        prev_proc_fd_valid[i] = procs[i].fd_valid;
        memcpy(prev_cpu_hist[i], procs[i].cpu_hist, sizeof(procs[i].cpu_hist));
        memcpy(prev_mem_hist[i], procs[i].mem_hist, sizeof(procs[i].mem_hist));
        prev_hist_pos[i] = procs[i].hist_pos;
    }
    state->readings_stale = stale;
    if (state->selected_pid != -1 &&
        find_proc_index_by_pid(state->selected_pid) == -1)
        state->selected_pid = -1;
    build_groups();
    /* auto-close panel if process no longer valid */
    if (state->process_panel_open)
    {
        if (find_proc_by_pid(state->panel_pid) == NULL)
        {
            state->process_panel_open = 0;
            state->panel_pid = -1;
        }
    }
}

static void build_visible_rows(void)
{
    visible_row_count = 0;

    for (int g = 0; g < group_count; g++)
    {

        if (visible_row_count >= MAX_VISIBLE_ROWS) return;
        visible_rows[visible_row_count++] =
            (VisibleRow){ROW_GROUP, g, -1};

        if (groups[g].expanded)
        {
            for (int i = 0; i < groups[g].pid_count; i++)
            {
                if (visible_row_count >= MAX_VISIBLE_ROWS) return;
                visible_rows[visible_row_count++] =
                    (VisibleRow){ROW_PROC, -1, groups[g].pids[i]};
            }
        }
    }
}

static void draw_table_cell(int row, int col, int width, const char *text)
{
    if (col >= 0 && width > 0)
        mvwaddnstr(stdscr, row, col, text, width);
}

static void draw_history(int row, int col, int width,
                         const unsigned char history[HIST_LEN], int position)
{
    if (width > HIST_LEN) width = HIST_LEN;
    for (int i = 0; i < width; i++) {
        int sample = (position + i * HIST_LEN / width) % HIST_LEN;
        mvwaddwstr(stdscr, row, col + i, bars_w[history[sample]]);
    }
}

void process_draw(AppState *state)
{
    if (state->rows < 4 || state->cols < 12) {
        if (state->rows > 1 && state->cols > 0)
            mvwaddnstr(stdscr, 1, 0, "Terminal too small; resize to view processes", state->cols);
        return;
    }
    int y = 2;

    /* build navigation model */
    build_visible_rows();

    /* --- restore selection from identity --- */
    state->selected_row = -1;

    for (int i = 0; i < visible_row_count; i++)
    {
        if (visible_rows[i].type == ROW_GROUP &&
            strcmp(groups[visible_rows[i].group_index].name, state->selected_group_name) == 0)
        {
            state->selected_row = i;
            break;
        }

        if (visible_rows[i].type == ROW_PROC &&
            visible_rows[i].pid == state->selected_pid)
        {
            state->selected_row = i;
            break;
        }
    }

    if (state->selected_row != -1)
    {
        process_sync_selection_from_row(state);
    }

    if (state->selected_row == -1 && visible_row_count > 0)
    {
        state->selected_row = 0;
        process_sync_selection_from_row(state);
    }

    if (visible_row_count == 0) {
        state->selected_group_name[0] = '\0';
        state->selected_pid = -1;
    }

    int show_cpu = state->cols >= 32;
    int show_rss = state->cols >= 42;
    int show_ram = state->cols >= 60;
    int show_pid = state->cols >= 72;
    int ram_x = show_ram ? state->cols - 10 : -1;
    int rss_x = show_rss ? (show_ram ? ram_x - 11 : state->cols - 12) : -1;
    int cpu_x = show_cpu ? (show_rss ? rss_x - 9 : state->cols - 10) : -1;
    int pid_x = show_pid ? cpu_x - 11 : -1;
    int name_right = show_pid ? pid_x - 1 : show_cpu ? cpu_x - 1 :
        show_rss ? rss_x - 1 : show_ram ? ram_x - 1 : state->cols - 2;
    int name_width = name_right - 2;

    draw_table_cell(1, 2, name_width, "APP / PROCESS");
    if (show_pid) draw_table_cell(1, pid_x, 10, "PID");
    if (show_cpu) draw_table_cell(1, cpu_x, 8, "CPU %");
    if (show_rss) draw_table_cell(1, rss_x, 9, "RSS KB");
    if (show_ram) draw_table_cell(1, ram_x, 8, "RAM %");

    int view_height = state->rows - 3;
    if (view_height < 1) return;
    if (state->selected_row < state->process_scroll_offset)
        state->process_scroll_offset = state->selected_row;
    if (state->selected_row >= state->process_scroll_offset + view_height)
        state->process_scroll_offset = state->selected_row - view_height + 1;
    int max_scroll = visible_row_count - view_height;
    if (max_scroll < 0) max_scroll = 0;
    if (state->process_scroll_offset > max_scroll)
        state->process_scroll_offset = max_scroll;
    if (state->process_scroll_offset < 0)
        state->process_scroll_offset = 0;
    for (int row = state->process_scroll_offset;
         row < visible_row_count && y < state->rows - 1;
         row++, y++) {
        VisibleRow *visible = &visible_rows[row];
        if (state->selected_row == row) attron(A_REVERSE);
        if (visible->type == ROW_GROUP) {
            ProcGroup *grp = &groups[visible->group_index];
            char label[APP_KEY_LEN + 32], cell[32];
            snprintf(label, sizeof(label), "%s %s (%d)",
                     grp->expanded ? "▼" : "▶", grp->name, grp->pid_count);
            draw_table_cell(y, 2, name_width, label);
            if (show_cpu) {
                snprintf(cell, sizeof(cell), "%7.1f%%", grp->cpu_total);
                draw_table_cell(y, cpu_x, 8, cell);
            }
            if (show_rss) {
                snprintf(cell, sizeof(cell), "%10lu", grp->rss_total);
                draw_table_cell(y, rss_x, 10, cell);
            }
            if (show_ram) {
                if (total_mem_valid && total_mem_kb > 0) {
                    double percent = 100.0 * (double)grp->rss_total / (double)total_mem_kb;
                    snprintf(cell, sizeof(cell), "%7.2f%%", percent);
                } else snprintf(cell, sizeof(cell), "     --");
                draw_table_cell(y, ram_x, 8, cell);
            }
        } else {
            ProcInfo *p = find_proc_by_pid(visible->pid);
            if (p) {
                char cell[32];
                draw_table_cell(y, 2, name_width, p->app_key);
                if (show_pid) {
                    snprintf(cell, sizeof(cell), "%10d", p->pid);
                    draw_table_cell(y, pid_x, 10, cell);
                }
                if (show_cpu) {
                    snprintf(cell, sizeof(cell), "%7.1f%%", p->cpu_percent);
                    draw_table_cell(y, cpu_x, 8, cell);
                }
                if (show_rss) {
                    if (p->memory_valid) snprintf(cell, sizeof(cell), "%10lu", p->rss_kb);
                    else snprintf(cell, sizeof(cell), "%10s", "--");
                    draw_table_cell(y, rss_x, 10, cell);
                }
                if (show_ram) {
                    if (p->memory_valid && total_mem_valid && total_mem_kb > 0) {
                        double percent = 100.0 * (double)p->rss_kb / (double)total_mem_kb;
                        snprintf(cell, sizeof(cell), "%7.2f%%", percent);
                    } else snprintf(cell, sizeof(cell), "     --");
                    draw_table_cell(y, ram_x, 8, cell);
                }
            }
        }
        if (state->selected_row == row) attroff(A_REVERSE);
    }
}

int find_proc_index_by_pid(int pid)
{
    for (int i = 0; i < proc_count; i++)
    {
        if (procs[i].pid == pid)
            return i;
    }
    return -1;
}

int process_visible_row_count(void)
{
    return visible_row_count;
}

void process_set_group_expanded_by_name(const char *name, int expanded)
{
    for (int g = 0; g < group_count; g++)
    {
        if (strcmp(groups[g].name, name) == 0)
        {
            groups[g].expanded = expanded;
            return;
        }
    }
}
void process_commit_group_state(void)
{
    prev_group_count = group_count;

    for (int i = 0; i < group_count; i++)
    {
        strncpy(prev_groups[i].name, groups[i].name, APP_KEY_LEN - 1);
        prev_groups[i].name[APP_KEY_LEN - 1] = '\0';
        prev_groups[i].expanded = groups[i].expanded;
    }
}
int process_find_group_by_pid(int pid)
{
    for (int g = 0; g < group_count; g++)
    {
        for (int i = 0; i < groups[g].pid_count; i++)
        {
            if (groups[g].pids[i] == pid)
                return g;
        }
    }
    return -1;
}
void process_collapse_selected(AppState *state)
{
    /* 1. group header selected */
    if (state->selected_group_name[0] != '\0')
    {

        /* collapsing the selected group */
        process_set_group_expanded_by_name(
            state->selected_group_name,
            0);
        process_commit_group_state();

        /* close panel if it belongs to this group */
        if (state->process_panel_open &&
            process_find_group_by_pid(state->panel_pid) != -1 &&
            strcmp(
                groups[process_find_group_by_pid(state->panel_pid)].name,
                state->selected_group_name) == 0)
        {

            state->process_panel_open = 0;
            state->panel_pid = -1;
        }

        return;
    }

    /* 2. child process selected → collapse its parent */
    if (state->selected_pid != -1)
    {

        int g = process_find_group_by_pid(state->selected_pid);

        if (g >= 0)
        {
            process_set_group_expanded_by_name(
                groups[g].name,
                0);
            process_commit_group_state();

            /* close side panel if it was showing this group */
            if (state->process_panel_open &&
                process_find_group_by_pid(state->panel_pid) == g)
            {

                state->process_panel_open = 0;
                state->panel_pid = -1;
            }
        }
    }
}

static void format_memory(unsigned long kb, char *buf, size_t size)
{
    if (kb < 1024)
    {
        snprintf(buf, size, "%lu KB", kb);
    }
    else if (kb < 1024UL * 1024UL)
    {
        snprintf(
            buf,
            size,
            "%.1f MB",
            (double)kb / 1024.0);
    }
    else
    {
        snprintf(
            buf,
            size,
            "%.2f GB",
            (double)kb / (1024.0 * 1024.0));
    }
}

static void draw_panel_line(int row, int x0, int panel_width, const char *text)
{
    int width = panel_width - 4;
    if (width > 0)
        mvwaddnstr(stdscr, row, x0 + 2, text, width);
}

void draw_process_panel(AppState *state)
{
    int x0 = state->cols - state->panel_width;
    int y = 2;

    /* divider */
    for (int i = 1; i < state->rows - 1; i++)
        mvaddch(i, x0 - 1, ACS_VLINE);

    draw_panel_line(1, x0, state->panel_width, "PROCESS DETAILS");
    mvhline(2, x0 + 2, ACS_HLINE, state->panel_width - 4);

    ProcInfo *p = find_proc_by_pid(state->panel_pid);
    if (!p) {
        draw_panel_line(y + 3, x0, state->panel_width, "Process exited");
        return;
    }

    /* user */
    struct passwd *pw = getpwuid(p->uid);
    const char *user = pw ? pw->pw_name : "unknown";

    /* uptime */
    FILE *f = fopen("/proc/uptime", "r");
    double sys_up = 0.0;
    if (f) {
        int valid = fscanf(f, "%lf", &sys_up) == 1;
        fclose(f);
        if (valid) {
            last_system_uptime = sys_up;
            have_system_uptime = 1;
        } else state->readings_stale = 1;
    } else {
        state->readings_stale = 1;
    }
    if (have_system_uptime) sys_up = last_system_uptime;

    long ticks = sysconf(_SC_CLK_TCK);
    double start_sec = (double)p->start_time / (double)ticks;
    long proc_seconds = (long)(sys_up - start_sec);
    if (proc_seconds < 0) proc_seconds = 0;

    char upbuf[64];
    if (have_system_uptime)
        format_seconds(proc_seconds, upbuf, sizeof(upbuf));
    else
        snprintf(upbuf, sizeof(upbuf), "Unavailable");

    char membuf[32];
    if (p->memory_valid)
        format_memory(p->rss_kb, membuf, sizeof(membuf));
    else
        snprintf(membuf, sizeof(membuf), "Unavailable");

    /* ---- BASIC INFO ---- */
    char line[256];
    snprintf(line, sizeof(line), "PID     : %d", p->pid);
    draw_panel_line(y + 3, x0, state->panel_width, line);
    snprintf(line, sizeof(line), "USER    : %s", user);
    draw_panel_line(y + 4, x0, state->panel_width, line);
    snprintf(line, sizeof(line), "STATE   : %c", p->state);
    draw_panel_line(y + 5, x0, state->panel_width, line);
    snprintf(line, sizeof(line), "THREADS : %d", p->threads);
    draw_panel_line(y + 6, x0, state->panel_width, line);
    if (p->fd_valid)
        snprintf(line, sizeof(line), "FDs     : %d", p->fd_count);
    else
        snprintf(line, sizeof(line), "FDs     : Unavailable");
    draw_panel_line(y + 7, x0, state->panel_width, line);

    /* ---- CPU ---- */
    snprintf(line, sizeof(line), "TTY     : %d", p->tty_nr);
    draw_panel_line(y + 8, x0, state->panel_width, line);
    snprintf(line, sizeof(line), "SESSION : %d", p->session);
    draw_panel_line(y + 9, x0, state->panel_width, line);
    snprintf(line, sizeof(line), "CPU     : %5.1f %%", p->cpu_percent);
    draw_panel_line(y + 10, x0, state->panel_width, line);

    int history_width = state->panel_width - 16;
    draw_panel_line(y + 11, x0, state->panel_width, "CPU HIST:");
    draw_history(y + 11, x0 + 13, history_width, p->cpu_hist, p->hist_pos);

    double ram_percent = p->memory_valid && total_mem_valid && total_mem_kb > 0 ?
        100.0 * (double)p->rss_kb / (double)total_mem_kb : 0.0;
    if (p->memory_valid && total_mem_valid)
        snprintf(line, sizeof(line), "RSS: %s (%.2f%% RAM)", membuf, ram_percent);
    else
        snprintf(line, sizeof(line), "RSS: %s (-- RAM)", membuf);
    draw_panel_line(y + 12, x0, state->panel_width, line);

    draw_panel_line(y + 13, x0, state->panel_width, "MEM HIST:");
    draw_history(y + 13, x0 + 13, history_width, p->mem_hist, p->hist_pos);

    /* Relative bar compares this RSS with the largest listed process. */
    int bar_width = state->panel_width - 22;
    if (bar_width > 20) bar_width = 20;
    if (bar_width < 8) bar_width = 8;

    /* compute once per draw */
    unsigned long max_rss = 1;
    for (int i = 0; i < proc_count; i++) {
        if (procs[i].memory_valid && procs[i].rss_kb > max_rss)
            max_rss = procs[i].rss_kb;
    }

    /* per process */
    double ratio = p->memory_valid ? (double)p->rss_kb / (double)max_rss : 0.0;
    int filled = (int)(ratio * bar_width);

    if (filled < 1 && p->rss_kb > 0)
        filled = 1;   // ensure visibility

    if (filled > bar_width)
        filled = bar_width;

    int bar_x = x0 + 13;   // after "MEMORY  : ["
    int bar_y = y + 15;
    const char *block_full = "█";
    const char *block_empty = "░";

    draw_panel_line(bar_y, x0, state->panel_width, "REL RSS: [");
    for (int i = 0; i < bar_width; i++) {
        mvaddstr(
            bar_y,
            bar_x + i,
            (i < filled) ? block_full : block_empty
        );
    }
    if (bar_x + bar_width < x0 + state->panel_width - 1)
        mvprintw(bar_y, bar_x + bar_width, "]");


    /* ---- UPTIME ---- */
    snprintf(line, sizeof(line), "UPTIME  : %s", upbuf);
    draw_panel_line(y + 16, x0, state->panel_width, line);

    /* Keep the executable visible while never retaining or showing arguments. */
    const char *executable = p->executable[0] ? p->executable : p->name;
    snprintf(line, sizeof(line), "EXECUTABLE: %.*s",
             (int)sizeof(line) - 13, executable);
    draw_panel_line(y + 17, x0, state->panel_width, line);

    draw_panel_line(state->rows - 2, x0, state->panel_width, "[X] Close panel");
}
