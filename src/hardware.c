#define _POSIX_C_SOURCE 200112L

#include <ncursesw/ncurses.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "hardware.h"
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <ctype.h>
#include <stdlib.h>

#define PAD_HEIGHT 100
#define MEM_ACT_HIST_LEN 25

static unsigned long long prev_pgpgin = 0;
static unsigned long long prev_pgpgout = 0;
static int mem_act_initialized = 0;

static unsigned char mem_act_hist[MEM_ACT_HIST_LEN];
static int mem_act_pos = 0;

static const char *bars[] = {
    "▁","▂","▃","▄","▅","▆","▇","█"
};

static int content_height = 0;
static WINDOW *pad;
static int scroll_offset = 0;
typedef struct {
    unsigned long long total;
    unsigned long long idle;
} CpuSample;
static int visible_height(const AppState *state)
{
    int top = 1;                  // must match pnoutrefresh()
    int bottom = state->rows - 2; // must match pnoutrefresh()

    return bottom - top + 1;
}
static int read_cpu_sample(CpuSample *s)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;

    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &user, &nice, &system, &idle, &iowait,
           &irq, &softirq, &steal);

    s->idle  = idle + iowait;
    s->total = user + nice + system + idle + iowait +
               irq + softirq + steal;

    return 0;
}
static CpuSample prev_cpu = {0};
static int cpu_initialized = 0;
typedef struct {
    unsigned long rx;
    unsigned long tx;
} NetSample;

static NetSample prev_net[16];
static char prev_if[16][32];
static int prev_count = 0;
static void section(int *y, const char *title)
{
    (*y)++;
    mvwprintw(pad, (*y)++, 2, "== %s ==", title);
}

static void load_cpu_details(char *model, size_t msz,
                             int *cores, int *threads,
                             char *mhz, size_t mhzsz,
                             char *cache, size_t csz)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    *cores = *threads = 0;
    mhz[0] = cache[0] = 0;

    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "model name", 10) == 0 && !model[0]) {
            char *p = strchr(line, ':');
            if (p) {
                strncpy(model, p + 2, msz);
                model[strcspn(model, "\n")] = 0;
            }
        } else if (strncmp(line, "processor", 9) == 0) {
            (*threads)++;
        } else if (strncmp(line, "cpu cores", 9) == 0 && *cores == 0) {
            *cores = atoi(strchr(line, ':') + 1);
        } else if (strncmp(line, "cpu MHz", 7) == 0 && !mhz[0]) {
            strncpy(mhz, strchr(line, ':') + 2, mhzsz);
            mhz[strcspn(mhz, "\n")] = 0;
        } else if (strncmp(line, "cache size", 10) == 0 && !cache[0]) {
            strncpy(cache, strchr(line, ':') + 2, csz);
            cache[strcspn(cache, "\n")] = 0;
        }
    }
    if (f) fclose(f);
}
static int read_vmstat(unsigned long long *pgin,
                       unsigned long long *pgout)
{
    FILE *f = fopen("/proc/vmstat", "r");
    if (!f)
        return -1;

    char key[64];
    unsigned long long val;

    *pgin = *pgout = 0;

    while (fscanf(f, "%63s %llu", key, &val) == 2) {
        if (strcmp(key, "pgpgin") == 0)
            *pgin = val;
        else if (strcmp(key, "pgpgout") == 0)
            *pgout = val;

        if (*pgin && *pgout)
            break;
    }

    fclose(f);
    return 0;
}

static void load_mem_ext(long *total, long *avail, long *free,
                         long *cached, long *swap_t, long *swap_f)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char key[64];
    long val;
    char unit[16];

    *total = *avail = *free = *cached = *swap_t = *swap_f = 0;

    while (f && fscanf(f, "%63s %ld %15s\n", key, &val, unit) == 3) {
        if (!strcmp(key, "MemTotal:")) *total = val;
        else if (!strcmp(key, "MemAvailable:")) *avail = val;
        else if (!strcmp(key, "MemFree:")) *free = val;
        else if (!strcmp(key, "Cached:")) *cached = val;
        else if (!strcmp(key, "SwapTotal:")) *swap_t = val;
        else if (!strcmp(key, "SwapFree:")) *swap_f = val;
    }
    if (f) fclose(f);
}


static void load_disk(char *buf, size_t size)
{
    struct statvfs v;
    if (statvfs("/", &v) == 0) {
        unsigned long total = (v.f_blocks * v.f_frsize) / (1024*1024*1024);
        unsigned long free  = (v.f_bfree  * v.f_frsize) / (1024*1024*1024);
        unsigned long used  = total - free;
        snprintf(buf, size, "%lu GB total / %lu GB used / %lu GB free",
                 total, used, free);
    } else {
        snprintf(buf, size, "Unavailable");
    }
}


static void load_net(int *y)
{
    DIR *d = opendir("/sys/class/net");
    struct dirent *e;

    if (!d) {
        mvwprintw(pad, (*y)++, 4, "Network unavailable");
        return;
    }

    int idx = 0;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;

        char rxp[512], txp[512];
        unsigned long rx = 0, tx = 0;

        snprintf(rxp, sizeof(rxp),
                 "/sys/class/net/%s/statistics/rx_bytes", e->d_name);
        snprintf(txp, sizeof(txp),
                 "/sys/class/net/%s/statistics/tx_bytes", e->d_name);

        FILE *f = fopen(rxp, "r");
        if (f) { fscanf(f, "%lu", &rx); fclose(f); }
        f = fopen(txp, "r");
        if (f) { fscanf(f, "%lu", &tx); fclose(f); }

        double rx_rate = 0.0, tx_rate = 0.0;

        if (idx < prev_count && strcmp(prev_if[idx], e->d_name) == 0) {
            rx_rate = (double)(rx - prev_net[idx].rx) / 1024.0;
            tx_rate = (double)(tx - prev_net[idx].tx) / 1024.0;
        }

        strncpy(prev_if[idx], e->d_name, sizeof(prev_if[idx]));
        prev_net[idx].rx = rx;
        prev_net[idx].tx = tx;

        mvwprintw(pad, (*y)++, 2,
          "%-20s : RX %4.1f KB/s / TX %4.1f KB/s",
          e->d_name, rx_rate, tx_rate);

        idx++;
    }

    prev_count = idx;
    closedir(d);
}


static void draw_line(int *y, const char *label, const char *value)
{
    mvwprintw(pad, (*y)++, 2, "%-20s : %s", label, value);
}


void hardware_init(void)
{
    pad = NULL;
}
static void format_uptime(double seconds, char *buf, size_t size)
{
    long s = (long)seconds;

    long days = s / 86400;
    s %= 86400;
    long hours = s / 3600;
    s %= 3600;
    long minutes = s / 60;
    long secs = s % 60;

    if (days > 0)
        snprintf(buf, size, "%ld days %02ld:%02ld:%02ld",
                 days, hours, minutes, secs);
    else
        snprintf(buf, size, "%02ld:%02ld:%02ld",
                 hours, minutes, secs);
}


void hardware_draw(AppState *state)
{
    static int last_cols = 0;

    if (!pad || state->cols != last_cols) {
        if (pad)
            delwin(pad);

        pad = newpad(PAD_HEIGHT, state->cols);
        last_cols = state->cols;
    }

    werase(pad);
    int y = 1;
    char buf[256];
    int vheight = visible_height(state);
    int max_scroll = content_height - vheight;

    if (max_scroll < 0)
        max_scroll = 0;

    if (scroll_offset > max_scroll)
        scroll_offset = max_scroll;

    if (scroll_offset < 0)
        scroll_offset = 0;

    section(&y, "System");

    gethostname(buf, sizeof(buf));
    draw_line(&y, "Hostname", buf);

    struct utsname uts;
    uname(&uts);
    draw_line(&y, "Kernel", uts.release);
    draw_line(&y, "Architecture", uts.machine);

    section(&y, "CPU");

    char model[256]={0}, mhz[64]={0}, cache[64]={0};
    int cores=0, threads=0;
    load_cpu_details(model, sizeof(model), &cores, &threads, mhz, sizeof(mhz), cache, sizeof(cache));
    draw_line(&y, "Model", model);
    snprintf(buf, sizeof(buf), "%d cores / %d threads", cores, threads);
    draw_line(&y, "Topology", buf);
    draw_line(&y, "Frequency", mhz);
    draw_line(&y, "Cache", cache);
    CpuSample cur;
    if (read_cpu_sample(&cur) == 0) {
        if (cpu_initialized) {
            unsigned long long dt = cur.total - prev_cpu.total;
            unsigned long long di = cur.idle  - prev_cpu.idle;

            double usage = 0.0;
            if (dt > 0)
                usage = ((double)(dt - di) * 100.0) / (double)dt;

            snprintf(buf, sizeof(buf), "%.1f %%", usage);
            draw_line(&y, "Usage", buf);
        }
        prev_cpu = cur;
        cpu_initialized = 1;
    }

    section(&y, "Memory");
    unsigned long long pgin, pgout;

if (read_vmstat(&pgin, &pgout) == 0) {

    if (mem_act_initialized) {
        unsigned long long delta_in  = pgin  - prev_pgpgin;
        unsigned long long delta_out = pgout - prev_pgpgout;
        unsigned long long delta = delta_in + delta_out;
        int level;

        if (delta == 0) level = 0;
        else if (delta < 5) level = 1;
        else if (delta < 20) level = 2;
        else if (delta < 50) level = 3;
        else if (delta < 100) level = 4;
        else if (delta < 300) level = 5;
        else if (delta < 700) level = 6;
        else level = 7;
        mem_act_hist[mem_act_pos] = (unsigned char)level;
        mem_act_pos = (mem_act_pos + 1) % MEM_ACT_HIST_LEN;
    }
    prev_pgpgin = pgin;
    prev_pgpgout = pgout;
    mem_act_initialized = 1;
}


    long mt, ma, mf, mc, st, sf;
    load_mem_ext(&mt, &ma, &mf, &mc, &st, &sf);
    snprintf(buf, sizeof(buf), "%ld MB total / %ld MB available", mt/1024, ma/1024);
    draw_line(&y, "RAM", buf);
    snprintf(buf, sizeof(buf), "Free %ld MB  Cached %ld MB", mf/1024, mc/1024);
    draw_line(&y, "Details", buf);
    snprintf(buf, sizeof(buf), "%ld MB total / %ld MB free", st/1024, sf/1024);
    draw_line(&y, "Swap", buf);
    mvwprintw(pad, y, 2, "Activity             :");
    for (int i = 0; i < MEM_ACT_HIST_LEN; i++) {
        int idx = (mem_act_pos + i) % MEM_ACT_HIST_LEN;
        mvwprintw(pad, y, 26 + i, "%s", bars[mem_act_hist[idx]]);
    }
    y++;
    

    section(&y, "Disk");
    load_disk(buf, sizeof(buf));
    draw_line(&y, "Root (/)", buf);

    section(&y, "Network");
    load_net(&y);

    section(&y, "RUNTIME");
    /* Uptime */
    FILE *f = fopen("/proc/uptime", "r");
    double up = 0.0;
    if (f) {
        fscanf(f, "%lf", &up);
        fclose(f);
    }
    format_uptime(up, buf, sizeof(buf));
    draw_line(&y, "Uptime", buf);
    
    content_height = y;
    pnoutrefresh(pad,
             scroll_offset, 0,
             1, 0,
             state->rows - 2, state->cols - 1);
}


void hardware_handle_input(int ch, AppState *state)
{
    int vheight = visible_height(state);

    switch (ch) {
        case KEY_UP:
            scroll_offset--;
            break;

        case KEY_DOWN:
            scroll_offset++;
            break;

        case KEY_PPAGE:
            scroll_offset -= vheight;
            break;

        case KEY_NPAGE:
            scroll_offset += vheight;
            break;
    }
}
