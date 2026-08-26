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
#include <time.h>
#include <wchar.h>

static ProcGroup groups[MAX_GROUPS];
static int group_count = 0;

static VisibleRow visible_rows[1024];
static int visible_row_count = 0;

static PrevGroup prev_groups[MAX_GROUPS];
static int prev_group_count = 0;

static uid_t current_uid = (uid_t)-1;

static unsigned long long prev_proc_times[MAX_PROCS];
static int prev_proc_pids[MAX_PROCS];
static int prev_count = 0;

static unsigned long long last_sys_total = 0;
static int cpu_sys_initialized = 0;
static int cpu_count = 1;

static ProcInfo procs[MAX_PROCS];
static int proc_count = 0;

static unsigned long prev_rss_kb[MAX_PROCS];
static int prev_rss_pids[MAX_PROCS];
static int prev_rss_count = 0;

static int selected = 0;
static int scroll_offset = 0;

static const wchar_t *bars_w[] = {
    L"▁", L"▂", L"▃", L"▄",
    L"▅", L"▆", L"▇", L"█"};

static unsigned long total_mem_kb = 0;

void process_init(void)
{
    proc_count = 0;
    selected = 0;
    scroll_offset = 0;
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

// static int read_proc_stat(int pid, ProcInfo *p)
// {
//     char path[64];
//     snprintf(path, sizeof(path), "/proc/%d/stat", pid);

//     FILE *f = fopen(path, "r");
//     if (!f)
//         return -1;

//     char comm[256];
//     char state;
//     unsigned long long utime, stime, starttime;
//     int tty_nr;
//     int session;

//     if (fscanf(f,
//                "%d (%255[^)]) %c "
//                "%*d %*d %d %d %*d "
//                "%*u %*u %*u %*u %*u "
//                "%llu %llu "
//                "%*d %*d %*d %*d "
//                "%*d %*d %llu",
//                &p->pid,
//                comm,
//                &state,
//                &session,
//                &tty_nr,
//                &utime,
//                &stime,
//                &starttime) != 7)
//     {
//         fclose(f);
//         return -1;
//     }

//     fclose(f);

//     strncpy(p->name, comm, COMM_LEN - 1);
//     p->name[COMM_LEN - 1] = '\0';

//     p->state = state;
//     p->utime = utime;
//     p->stime = stime;
//     p->start_time = starttime;
//     p->tty_nr = tty_nr;
//     p->session = session;

//     return 0;
// }
static int read_proc_stat(int pid, ProcInfo *p)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char comm[256];
    char state;
    unsigned long long utime, stime, starttime;
    int tty_nr;
    int session;
    fscanf(f, "%d %255s %c "
              "%*d %*d %d %d %*d "
              "%*u %*u %*u %*u %*u "
              "%llu %llu "
              "%*d %*d %*d %*d "
              "%*d %*d %llu",
           &p->pid, comm, &state, &session, &tty_nr, &utime, &stime, &starttime);
    fclose(f);
    p->state = state;
    strncpy(p->name, comm, COMM_LEN);
    p->name[COMM_LEN - 1] = '\0';
    p->utime = utime;
    p->stime = stime;
    p->start_time = starttime;
    p->tty_nr = tty_nr;
    p->session = session;
    return 0;
}

static void read_proc_mem(int pid, ProcInfo *p)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[256];

    while (fgets(line, sizeof(line), f))
    {

        if (strncmp(line, "VmRSS:", 6) == 0)
        {
            unsigned long kb = 0;
            sscanf(line + 6, "%lu", &kb);
            p->rss_kb = kb;
        }

        else if (strncmp(line, "Threads:", 8) == 0)
        {
            int t = 0;
            sscanf(line + 8, "%d", &t);
            p->threads = t;
        }
    }

    fclose(f);

    /* ---------- FD COUNT ---------- */
    snprintf(path, sizeof(path), "/proc/%d/fd", pid);

    DIR *d = opendir(path);
    if (d)
    {
        int count = 0;
        struct dirent *e;
        while ((e = readdir(d)))
        {
            if (e->d_name[0] != '.')
                count++;
        }
        closedir(d);
        p->fd_count = count;
    }
}

static unsigned long long find_prev_proc_time(int pid)
{
    for (int i = 0; i < prev_count; i++)
    {
        if (prev_proc_pids[i] == pid)
            return prev_proc_times[i];
    }
    return 0;
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
    fgets(line, sizeof(line), f);
    fclose(f);

    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &user, &nice, &system, &idle,
           &iowait, &irq, &softirq, &steal);

    return user + nice + system + idle +
           iowait + irq + softirq + steal;
}
static void read_proc_cmdline(int pid, ProcInfo *p)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

    FILE *f = fopen(path, "r");
    if (!f)
    {
        p->cmdline[0] = '\0';
        return;
    }

    size_t n = fread(p->cmdline, 1, CMDLINE_LEN - 1, f);
    fclose(f);

    if (n == 0)
    {
        p->cmdline[0] = '\0';
        return;
    }

    p->cmdline[n] = '\0';

    /* convert NULs to spaces */
    for (size_t i = 0; i < n; i++)
    {
        if (p->cmdline[i] == '\0')
            p->cmdline[i] = ' ';
    }
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

static void read_proc_uid(int pid, ProcInfo *p)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE *f = fopen(path, "r");
    if (!f)
    {
        p->uid = (uid_t)-1;
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, "Uid:", 4) == 0)
        {
            unsigned int real_uid;
            sscanf(line, "Uid:\t%u", &real_uid);
            p->uid = (uid_t)real_uid;
            fclose(f);
            return;
        }
    }

    fclose(f);
    p->uid = (uid_t)-1;
}
static int is_kernel_thread(const ProcInfo *p)
{
    /* kernel threads have empty cmdline or names like [kworker] */
    if (p->cmdline[0] == '\0')
        return 1;

    if (p->name[0] == '[' && p->name[strlen(p->name) - 1] == ']')
        return 1;

    return 0;
}
static int is_foreground_candidate(const ProcInfo *p)
{
    /* must belong to current user */
    if (p->uid != current_uid)
        return 0;

    /* remove kernel threads */
    if (is_kernel_thread(p))
        return 0;

    /* interactive if has TTY */
    if (p->tty_nr != 0)
        return 1;

    /* GUI / app processes often have no TTY but have cmdline */
    if (p->cmdline[0] != '\0')
        return 1;

    return 0;
}

static void build_app_key(ProcInfo *p)
{
    /* default: use kernel name */
    strncpy(p->app_key, p->name, APP_KEY_LEN - 1);
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
            if (strcmp(groups[g].name, p->name) == 0)
                break;
        }

        if (g == group_count)
        {
            if (group_count >= MAX_GROUPS)
                break;

            memset(&groups[g], 0, sizeof(ProcGroup));
            strncpy(groups[g].name, p->name, APP_KEY_LEN - 1);
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

        if (groups[g].pid_count < 256)
            groups[g].pids[groups[g].pid_count++] = p->pid;

        groups[g].cpu_total += p->cpu_percent;
        if (p->rss_kb > 0)
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

static void update_total_memory(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return;

    char key[64];
    unsigned long value;
    char unit[32];

    while (fscanf(f, "%63s %lu %31s", key, &value, unit) == 3)
    {
        if (strcmp(key, "MemTotal:") == 0)
        {
            total_mem_kb = value;
            break;
        }
    }

    fclose(f);
}
static long find_prev_rss(int pid)
{
    for (int i = 0; i < prev_rss_count; i++)
    {
        if (prev_rss_pids[i] == pid)
            return (long)prev_rss_kb[i];
    }
    return -1;
}

void process_update(AppState *state)
{
    update_total_memory();
    DIR *d = opendir("/proc");
    struct dirent *e;
    unsigned long long sys_total = read_system_cpu_total();
    unsigned long long sys_delta = 0;
    if (cpu_sys_initialized)
        sys_delta = sys_total - last_sys_total;
    proc_count = 0;
    while ((e = readdir(d)) && proc_count < MAX_PROCS)
    {
        if (!isdigit(e->d_name[0]))
            continue;
        int pid = atoi(e->d_name);
        ProcInfo *p = &procs[proc_count];
        if (read_proc_stat(pid, p) == 0)
        {
            read_proc_cmdline(pid, p);
            read_proc_uid(pid, p);
            build_app_key(p);
            if (!state->show_all_processes && !is_foreground_candidate(p))
                continue;
            read_proc_mem(pid, p);
            unsigned long long proc_total = p->utime + p->stime;
            unsigned long long prev_total = find_prev_proc_time(p->pid);
            double new_value = 0.0;
            if (cpu_sys_initialized && sys_delta > 0 && prev_total > 0)
            {
                unsigned long long proc_delta = proc_total - prev_total;
                new_value =
                    (double)proc_delta * 100.0 * cpu_count /
                    (double)sys_delta;
            }
            p->cpu_instant = new_value;
            p->cpu_percent = 0.7 * p->cpu_percent + 0.3 * new_value;
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
            /* ===== MEMORY HISTORY FIX ===== */
            long prev_rss = find_prev_rss(p->pid);
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
    prev_rss_count = proc_count;
    for (int i = 0; i < proc_count; i++)
    {
        prev_rss_pids[i] = procs[i].pid;
        prev_rss_kb[i] = procs[i].rss_kb;
    }
    closedir(d);
    last_sys_total = sys_total;
    cpu_sys_initialized = 1;
    if (proc_count > 1)
    {
        qsort(procs, (size_t)proc_count, sizeof(ProcInfo), cmp_cpu_desc);
    }
    prev_count = proc_count;
    for (int i = 0; i < proc_count; i++)
    {
        prev_proc_pids[i] = procs[i].pid;
        prev_proc_times[i] = procs[i].utime + procs[i].stime;
    }
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

static void build_app_name(const ProcInfo *p, char *out, size_t size)
{
    if (p->cmdline[0] == '\0')
    {
        /* fallback: kernel threads, zombies */
        snprintf(out, size, "%s", p->name);
        return;
    }

    /* take executable */
    const char *cmd = p->cmdline;

    /* copy base executable */
    const char *space = strchr(cmd, ' ');
    size_t len = space ? (size_t)(space - cmd) : strlen(cmd);

    if (len >= size)
        len = size - 1;

    strncpy(out, cmd, len);
    out[len] = '\0';

    /* selectively append meaningful args */
    if (space)
    {
        const char *args = space + 1;

        if (strstr(args, "--app=") ||
            strstr(args, "--type=") ||
            strstr(args, "/"))
        {

            strncat(out, " ", size - strlen(out) - 1);
            strncat(out, args, size - strlen(out) - 1);
        }
    }
}

static void build_visible_rows(void)
{
    visible_row_count = 0;

    for (int g = 0; g < group_count; g++)
    {

        visible_rows[visible_row_count++] =
            (VisibleRow){ROW_GROUP, g, -1};

        if (groups[g].expanded)
        {
            for (int i = 0; i < groups[g].pid_count; i++)
            {
                visible_rows[visible_row_count++] =
                    (VisibleRow){ROW_PROC, -1, groups[g].pids[i]};
            }
        }
    }
}

void process_draw(AppState *state)
{
    int y = 2;
    int row = 0;

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

    /* header */
    mvprintw(
        1, 2,
        " APP / PROCESS      COUNT                           CPU %%       RSS (KB)");

    /* ---- RENDER ---- */
    for (int g = 0; g < group_count && y < state->rows - 1; g++)
    {

        ProcGroup *grp = &groups[g];

        /* group header */
        if (state->selected_row == row)
            attron(A_REVERSE);

        mvprintw(
            y, 2,
            "%s %-17.17s (%d)                      %10.1f%%        %ld",
            grp->expanded ? "▼" : "▶",
            grp->name,
            grp->pid_count,
            grp->cpu_total,
            grp->rss_total);

        if (state->selected_row == row)
            attroff(A_REVERSE);

        y++;
        row++;

        /* child processes */
        if (grp->expanded)
        {
            for (int i = 0;
                 i < grp->pid_count && y < state->rows - 1;
                 i++)
            {

                int pid = grp->pids[i];
                ProcInfo *p = find_proc_by_pid(pid);
                if (!p)
                    continue;

                if (state->selected_row == row)
                    attron(A_REVERSE);

                char appbuf[128];
                build_app_name(p, appbuf, sizeof(appbuf));

                mvprintw(
                    y, 6,
                    "%-40.40s     %6.1f%%    %8lu",
                    appbuf,
                    p->cpu_percent,
                    p->rss_kb);

                if (state->selected_row == row)
                    attroff(A_REVERSE);

                y++;
                row++;
            }
        }
    }
}

void process_handle_input(int ch, AppState *state)
{
    int view_height = state->rows - 3;

    switch (ch)
    {
    case KEY_UP:
        if (selected > 0)
            selected--;
        if (selected < scroll_offset)
            scroll_offset = selected;
        break;

    case KEY_DOWN:
        if (selected < proc_count - 1)
            selected++;
        if (selected >= scroll_offset + view_height)
            scroll_offset = selected - view_height + 1;
        break;
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

// void draw_process_panel(AppState *state)
// {
//     int x0 = state->cols - state->panel_width;
//     int y = 2; /* divider */
//     for (int i = 1; i < state->rows - 1; i++)
//         mvaddch(i, x0 - 1, ACS_VLINE);
//     mvprintw(1, x0 + 2, "PROCESS DETAILS");
//     ProcInfo *p = find_proc_by_pid(state->panel_pid);
//     if (!p)
//     {
//         mvprintw(y + 2, x0 + 2, "Process exited");
//         return;
//     } /* user */
//     struct passwd *pw = getpwuid(p->uid);
//     const char *user = pw ? pw->pw_name : "unknown"; /* uptime */
//     FILE *f = fopen("/proc/uptime", "r");
//     double sys_up = 0.0;
//     if (f)
//     {
//         fscanf(f, "%lf", &sys_up);
//         fclose(f);
//     }
//     long ticks = sysconf(_SC_CLK_TCK);
//     double start_sec = (double)p->start_time / (double)ticks;
//     long proc_seconds = (long)(sys_up - start_sec);
//     if (proc_seconds < 0)
//         proc_seconds = 0;
//     char upbuf[64];
//     format_seconds(proc_seconds, upbuf, sizeof(upbuf));
//     char membuf[32];
//     format_memory(p->rss_kb, membuf, sizeof(membuf));
//     mvprintw(y + 2, x0 + 2, "PID : %d", p->pid);
//     mvprintw(y + 3, x0 + 2, "USER : %s", user);
//     mvprintw(y + 4, x0 + 2, "CPU : %2.1f %%", p->cpu_percent);
//     mvprintw(y + 5, x0 + 2, "MEMORY : %s", membuf);
//     mvprintw(y + 6, x0 + 2, "UPTIME : %s", upbuf); /* ---- CPU HISTORY ---- */
//     mvprintw(y + 8, x0 + 2, "CPU HISTORY");
//     for (int i = 0; i < HIST_LEN; i++)
//     {
//         int idx = (p->hist_pos + i) % HIST_LEN;
//         mvaddwstr(y + 9, x0 + 2 + i, bars_w[p->cpu_hist[idx]]);
//     } /* ---- MEMORY HISTORY ---- */
//     mvprintw(y + 11, x0 + 2, "MEM HISTORY");
//     for (int i = 0; i < HIST_LEN; i++)
//     {
//         int idx = (p->hist_pos + i) % HIST_LEN;
//         mvaddwstr(y + 12, x0 + 2 + i, bars_w[p->mem_hist[idx]]);
//     }
//     mvprintw(y + 14, x0 + 2, "CMDLINE:");
//     const char *cmd = p->cmdline[0] ? p->cmdline : p->name;
//     mvprintw(y + 15, x0 + 2, "%.40s", cmd);
//     mvprintw(state->rows - 2, x0 + 2, "[X] Close panel");
// }

void draw_process_panel(AppState *state)
{
    int x0 = state->cols - state->panel_width;
    int y = 2;

    /* divider */
    for (int i = 1; i < state->rows - 1; i++)
        mvaddch(i, x0 - 1, ACS_VLINE);

    mvprintw(1, x0 + 2, "PROCESS DETAILS");
    mvhline(2, x0 + 2, ACS_HLINE, state->panel_width - 4);

    ProcInfo *p = find_proc_by_pid(state->panel_pid);
    if (!p) {
        mvprintw(y + 3, x0 + 2, "Process exited");
        return;
    }

    /* user */
    struct passwd *pw = getpwuid(p->uid);
    const char *user = pw ? pw->pw_name : "unknown";

    /* uptime */
    FILE *f = fopen("/proc/uptime", "r");
    double sys_up = 0.0;
    if (f) {
        fscanf(f, "%lf", &sys_up);
        fclose(f);
    }

    long ticks = sysconf(_SC_CLK_TCK);
    double start_sec = (double)p->start_time / (double)ticks;
    long proc_seconds = (long)(sys_up - start_sec);
    if (proc_seconds < 0) proc_seconds = 0;

    char upbuf[64];
    format_seconds(proc_seconds, upbuf, sizeof(upbuf));

    char membuf[32];
    format_memory(p->rss_kb, membuf, sizeof(membuf));

    /* ---- BASIC INFO ---- */
    mvprintw(y + 3, x0 + 2, "PID     : %d", p->pid);
    mvprintw(y + 4, x0 + 2, "USER    : %s", user);
    mvprintw(y + 5, x0 + 2, "STATE   : %c", p->state);
    mvprintw(y + 6, x0 + 2, "THREADS : %d", p->threads);
    mvprintw(y + 7, x0 + 2, "FDs     : %d", p->fd_count);

    /* ---- CPU ---- */
    mvprintw(y + 9,  x0 + 2, "CPU     : %5.1f %%", p->cpu_percent);

    mvprintw(y + 10, x0 + 2, "CPU HIST:");
    for (int i = 0; i < HIST_LEN; i++) {
        int idx = (p->hist_pos + i) % HIST_LEN;
        mvaddwstr(y + 10, x0 + 13 + i, bars_w[p->cpu_hist[idx]]);
    }

    /* ---- MEMORY BAR ---- */
    int bar_width = state->panel_width - 20;
    if (bar_width > 24) bar_width = 24;
    if (bar_width < 10) bar_width = 10;

    /* compute once per draw */
    unsigned long max_rss = 1;
    for (int i = 0; i < proc_count; i++) {
        if (procs[i].rss_kb > max_rss)
            max_rss = procs[i].rss_kb;
    }

    /* per process */
    double ratio = (double)p->rss_kb / (double)max_rss;
    int filled = (int)(ratio * bar_width);

    if (filled < 1 && p->rss_kb > 0)
        filled = 1;   // ensure visibility

    if (filled > bar_width)
        filled = bar_width;

    int bar_x = x0 + 13;   // after "MEMORY  : ["
    int bar_y = y + 12;
    const char *block_full = "█";
    const char *block_empty = "░";

    mvprintw(bar_y, x0 + 2, "MEMORY  : [");
    for (int i = 0; i < bar_width; i++) {
        mvaddstr(
            bar_y,
            bar_x + i,
            (i < filled) ? block_full : block_empty
        );
    }
    mvprintw(bar_y, bar_x + bar_width, "] %s", membuf);


    /* ---- UPTIME ---- */
    mvprintw(y + 14, x0 + 2, "UPTIME  : %s", upbuf);

    /* ---- CMDLINE ---- */
    mvprintw(y + 16, x0 + 2, "CMDLINE:");
    const char *cmd = p->cmdline[0] ? p->cmdline : p->name;
    mvprintw(y + 17, x0 + 2, "%.48s", cmd);

    mvprintw(
        state->rows - 2,
        x0 + 2,
        "[X] Close panel"
    );
}
