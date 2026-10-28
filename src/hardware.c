#define _POSIX_C_SOURCE 200112L

#include <ncursesw/ncurses.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "hardware.h"
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <time.h>

#define PAD_HEIGHT 256
#define MEM_ACT_HIST_LEN 25

static unsigned long long prev_pgpgin = 0;
static unsigned long long prev_pgpgout = 0;
static int mem_act_initialized = 0;

static unsigned char mem_act_hist[MEM_ACT_HIST_LEN];
static int mem_act_pos = 0;

static const char *bars[] = {
    "▁","▂","▃","▄","▅","▆","▇","█"
};

typedef struct {
    long total, avail, free, cached, swap_total, swap_free;
} MemorySample;

static int content_height = 0;
static WINDOW *pad;
static int scroll_offset = 0;
static MemorySample last_memory;
static int have_memory = 0;
static char last_disk[256];
static int have_disk = 0;
static char last_model[256], last_mhz[64], last_cache[64];
static int last_cores = 0, last_threads = 0, have_cpu_details = 0;
static double last_cpu_usage = 0.0;
static int have_cpu_usage = 0;
static double last_uptime = 0.0;
static int have_uptime = 0;
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

    unsigned long long user, nice, system, idle, iowait = 0;
    unsigned long long irq = 0, softirq = 0, steal = 0;
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &user, &nice, &system, &idle, &iowait,
           &irq, &softirq, &steal) < 4)
        return -1;

    s->idle  = idle + iowait;
    s->total = user + nice + system + idle + iowait +
               irq + softirq + steal;

    return 0;
}
static CpuSample prev_cpu = {0};
static int cpu_initialized = 0;
#define MAX_NET_IFS 64
typedef struct {
    char name[32];
    unsigned long long rx;
    unsigned long long tx;
    double rx_rate;
    double tx_rate;
    struct timespec sampled_at;
    int valid;
} NetSample;

static NetSample prev_net[MAX_NET_IFS];
static int prev_count = 0;
static void section(int *y, const char *title)
{
    (*y)++;
    mvwprintw(pad, (*y)++, 2, "== %s ==", title);
}

static int load_cpu_details(char *model, size_t msz,
                            int *cores, int *threads,
                            char *mhz, size_t mhzsz,
                            char *cache, size_t csz)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    *cores = 0;
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    *threads = online > 0 ? (int)online : 0;
    mhz[0] = cache[0] = 0;
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "model name", 10) == 0 && !model[0]) {
            char *p = strchr(line, ':');
            if (p) {
                snprintf(model, msz, "%s", p + 1);
                model[strcspn(model, "\n")] = 0;
                while (model[0] == ' ' || model[0] == '\t')
                    memmove(model, model + 1, strlen(model));
            }
        } else if (strncmp(line, "cpu MHz", 7) == 0 && !mhz[0]) {
            char *p = strchr(line, ':');
            if (p) snprintf(mhz, mhzsz, "%s", p + 1);
            mhz[strcspn(mhz, "\n")] = 0;
        } else if (strncmp(line, "cache size", 10) == 0 && !cache[0]) {
            char *p = strchr(line, ':');
            if (p) snprintf(cache, csz, "%s", p + 1);
            cache[strcspn(cache, "\n")] = 0;
        }
    }
    fclose(f);

    long configured = sysconf(_SC_NPROCESSORS_CONF);
    int package_ids[8192], core_ids[8192], found = 0;
    if (configured <= 0 || configured > 8192) return 0;
    for (long cpu = 0; cpu < configured; cpu++) {
        char path[160];
        int is_online = 1;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/online", cpu);
        FILE *online_file = fopen(path, "r");
        if (online_file) {
            if (fscanf(online_file, "%d", &is_online) != 1) is_online = 0;
            fclose(online_file);
        }
        if (!is_online) continue;

        int package, core;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/topology/physical_package_id", cpu);
        FILE *topology = fopen(path, "r");
        if (!topology || fscanf(topology, "%d", &package) != 1) {
            if (topology) fclose(topology);
            return 0;
        }
        fclose(topology);
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/topology/core_id", cpu);
        topology = fopen(path, "r");
        if (!topology || fscanf(topology, "%d", &core) != 1) {
            if (topology) fclose(topology);
            return 0;
        }
        fclose(topology);

        int known = 0;
        for (int i = 0; i < found; i++)
            if (package_ids[i] == package && core_ids[i] == core) known = 1;
        if (!known) {
            package_ids[found] = package;
            core_ids[found++] = core;
        }
    }
    *cores = found;
    return found > 0 && model[0] != '\0';
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
    int have_in = 0, have_out = 0;
    while (fscanf(f, "%63s %llu", key, &val) == 2) {
        if (strcmp(key, "pgpgin") == 0) {
            *pgin = val;
            have_in = 1;
        } else if (strcmp(key, "pgpgout") == 0) {
            *pgout = val;
            have_out = 1;
        }
        if (have_in && have_out)
            break;
    }

    fclose(f);
    return have_in && have_out ? 0 : -1;
}

static int load_mem_ext(MemorySample *sample)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char key[64];
    long val;
    char unit[16];
    MemorySample current = {0};
    unsigned int found = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63s %ld %15s", key, &val, unit) < 2) continue;
        if (!strcmp(key, "MemTotal:")) { current.total = val; found |= 1U; }
        else if (!strcmp(key, "MemAvailable:")) { current.avail = val; found |= 2U; }
        else if (!strcmp(key, "MemFree:")) { current.free = val; found |= 4U; }
        else if (!strcmp(key, "Cached:")) { current.cached = val; found |= 8U; }
        else if (!strcmp(key, "SwapTotal:")) { current.swap_total = val; found |= 16U; }
        else if (!strcmp(key, "SwapFree:")) { current.swap_free = val; found |= 32U; }
    }
    fclose(f);
    if (found != 63U) return -1;
    *sample = current;
    return 0;
}


static int load_disk(char *buf, size_t size)
{
    struct statvfs v;
    if (statvfs("/", &v) == 0) {
        long double gib = 1024.0L * 1024.0L * 1024.0L;
        unsigned long long total = (unsigned long long)
            ((long double)v.f_blocks * v.f_frsize / gib);
        unsigned long long avail = (unsigned long long)
            ((long double)v.f_bavail * v.f_frsize / gib);
        unsigned long long free_blocks = (unsigned long long)
            ((long double)v.f_bfree * v.f_frsize / gib);
        unsigned long long used = total > free_blocks ? total - free_blocks : 0;
        snprintf(buf, size, "%llu GB total / %llu GB used / %llu GB available",
                 total, used, avail);
        return 0;
    } else {
        return -1;
    }
}


static void copy_interface_name(char *dst, size_t dst_size, const char *src)
{
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int load_net(int *y)
{
    DIR *d = opendir("/sys/class/net");
    struct dirent *e;
    if (!d) {
        for (int i = 0; i < prev_count; i++)
            mvwprintw(pad, (*y)++, 2, "%-16s : RX %5.1f / TX %5.1f KB/s (stale)",
                      prev_net[i].name, prev_net[i].rx_rate, prev_net[i].tx_rate);
        if (prev_count == 0) mvwprintw(pad, (*y)++, 4, "Network unavailable");
        return -1;
    }

    NetSample current[MAX_NET_IFS] = {0};
    int count = 0, stale = 0, omitted = 0;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        closedir(d);
        return -1;
    }
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (count >= MAX_NET_IFS) { omitted = 1; continue; }
        NetSample *sample = &current[count];
        copy_interface_name(sample->name, sizeof(sample->name), e->d_name);
        char rxp[512], txp[512];
        snprintf(rxp, sizeof(rxp),
                 "/sys/class/net/%s/statistics/rx_bytes", e->d_name);
        snprintf(txp, sizeof(txp),
                 "/sys/class/net/%s/statistics/tx_bytes", e->d_name);

        unsigned long long rx = 0, tx = 0;
        FILE *f = fopen(rxp, "r");
        int rx_ok = f && fscanf(f, "%llu", &rx) == 1;
        if (f) fclose(f);
        f = fopen(txp, "r");
        int tx_ok = f && fscanf(f, "%llu", &tx) == 1;
        if (f) fclose(f);

        int previous = -1;
        for (int i = 0; i < prev_count; i++)
            if (strcmp(prev_net[i].name, sample->name) == 0) previous = i;
        if (!rx_ok || !tx_ok) {
            stale = 1;
            if (previous >= 0) *sample = prev_net[previous];
            copy_interface_name(sample->name, sizeof(sample->name), e->d_name);
        } else {
            sample->rx = rx;
            sample->tx = tx;
            sample->sampled_at = now;
            sample->valid = 1;
            if (previous >= 0 && prev_net[previous].valid &&
                rx >= prev_net[previous].rx && tx >= prev_net[previous].tx) {
                double elapsed = (double)(now.tv_sec - prev_net[previous].sampled_at.tv_sec) +
                    (double)(now.tv_nsec - prev_net[previous].sampled_at.tv_nsec) / 1000000000.0;
                if (elapsed > 0.0) {
                    sample->rx_rate = (double)(rx - prev_net[previous].rx) / 1024.0 / elapsed;
                    sample->tx_rate = (double)(tx - prev_net[previous].tx) / 1024.0 / elapsed;
                }
            }
        }
        if (sample->valid)
            mvwprintw(pad, (*y)++, 2, "%-16s : RX %5.1f / TX %5.1f KB/s",
                      sample->name, sample->rx_rate, sample->tx_rate);
        else
            mvwprintw(pad, (*y)++, 2, "%-16s : unavailable", sample->name);
        count++;
    }
    closedir(d);
    if (count == 0) mvwprintw(pad, (*y)++, 4, "No network interfaces");
    if (omitted) {
        mvwprintw(pad, (*y)++, 4, "Additional interfaces omitted (limit %d)", MAX_NET_IFS);
        stale = 1;
    }
    memcpy(prev_net, current, sizeof(current));
    prev_count = count;
    return stale ? -1 : 0;
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
    static char last_hostname[256];
    static struct utsname last_uts;
    static int have_hostname = 0, have_uts = 0;

    if (!pad || state->cols != last_cols) {
        if (pad) {
            delwin(pad);
            pad = NULL;
        }
        if (state->cols < 1) {
            state->readings_stale = 1;
            return;
        }
        pad = newpad(PAD_HEIGHT, state->cols);
        if (!pad) {
            state->readings_stale = 1;
            return;
        }
        last_cols = state->cols;
    }
    if (state->rows < 3) return;

    werase(pad);
    int y = 1;
    char buf[256];
    int vheight = visible_height(state);
    if (vheight < 1) vheight = 1;
    int stale = 0;

    section(&y, "System");

    if (gethostname(buf, sizeof(buf) - 1) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        snprintf(last_hostname, sizeof(last_hostname), "%s", buf);
        have_hostname = 1;
    } else stale = 1;
    draw_line(&y, "Hostname", have_hostname ? last_hostname : "Unavailable");

    struct utsname uts;
    if (uname(&uts) == 0) {
        last_uts = uts;
        have_uts = 1;
    } else stale = 1;
    draw_line(&y, "Kernel", have_uts ? last_uts.release : "Unavailable");
    draw_line(&y, "Architecture", have_uts ? last_uts.machine : "Unavailable");

    section(&y, "CPU");

    char model[256]={0}, mhz[64]={0}, cache[64]={0};
    int cores=0, threads=0;
    if (load_cpu_details(model, sizeof(model), &cores, &threads, mhz, sizeof(mhz), cache, sizeof(cache))) {
        if (model[0]) snprintf(last_model, sizeof(last_model), "%s", model);
        if (mhz[0]) snprintf(last_mhz, sizeof(last_mhz), "%s", mhz);
        if (cache[0]) snprintf(last_cache, sizeof(last_cache), "%s", cache);
        last_cores = cores;
        last_threads = threads;
        have_cpu_details = 1;
    } else stale = 1;
    draw_line(&y, "Model", have_cpu_details ? last_model : "Unavailable");
    if (have_cpu_details)
        snprintf(buf, sizeof(buf), "%d cores / %d threads", last_cores, last_threads);
    else
        snprintf(buf, sizeof(buf), "Unavailable");
    draw_line(&y, "Topology", buf);
    draw_line(&y, "Frequency", have_cpu_details && last_mhz[0] ? last_mhz : "Unavailable");
    draw_line(&y, "Cache", have_cpu_details && last_cache[0] ? last_cache : "Unavailable");
    CpuSample cur = {0};
    if (read_cpu_sample(&cur) == 0) {
        if (cpu_initialized) {
            if (cur.total >= prev_cpu.total && cur.idle >= prev_cpu.idle) {
                unsigned long long dt = cur.total - prev_cpu.total;
                unsigned long long di = cur.idle - prev_cpu.idle;
                if (dt > 0 && di <= dt) {
                    last_cpu_usage = ((double)(dt - di) * 100.0) / (double)dt;
                    have_cpu_usage = 1;
                }
            } else stale = 1;
        }
        prev_cpu = cur;
        cpu_initialized = 1;
    } else stale = 1;
    if (have_cpu_usage) snprintf(buf, sizeof(buf), "%.1f %%", last_cpu_usage);
    else snprintf(buf, sizeof(buf), "Unavailable");
    draw_line(&y, "Usage", buf);

    section(&y, "Memory");
    unsigned long long pgin = 0, pgout = 0;
    if (read_vmstat(&pgin, &pgout) == 0) {
        if (mem_act_initialized && pgin >= prev_pgpgin && pgout >= prev_pgpgout) {
            unsigned long long delta = (pgin - prev_pgpgin) + (pgout - prev_pgpgout);
            int level = delta == 0 ? 0 : delta < 5 ? 1 : delta < 20 ? 2 :
                delta < 50 ? 3 : delta < 100 ? 4 : delta < 300 ? 5 : delta < 700 ? 6 : 7;
            mem_act_hist[mem_act_pos] = (unsigned char)level;
            mem_act_pos = (mem_act_pos + 1) % MEM_ACT_HIST_LEN;
        } else if (mem_act_initialized) stale = 1;
        prev_pgpgin = pgin;
        prev_pgpgout = pgout;
        mem_act_initialized = 1;
    } else stale = 1;

    MemorySample memory;
    if (load_mem_ext(&memory) == 0) {
        last_memory = memory;
        have_memory = 1;
    } else stale = 1;
    if (have_memory) {
        snprintf(buf, sizeof(buf), "%ld MB total / %ld MB available",
                 last_memory.total/1024, last_memory.avail/1024);
        draw_line(&y, "RAM", buf);
        snprintf(buf, sizeof(buf), "Free %ld MB  Cached %ld MB",
                 last_memory.free/1024, last_memory.cached/1024);
        draw_line(&y, "Details", buf);
    } else {
        draw_line(&y, "RAM", "Unavailable");
        draw_line(&y, "Details", "Unavailable");
    }
    if (have_memory)
        snprintf(buf, sizeof(buf), "%ld MB total / %ld MB free",
                 last_memory.swap_total/1024, last_memory.swap_free/1024);
    else
        snprintf(buf, sizeof(buf), "Unavailable");
    draw_line(&y, "Swap", buf);
    mvwprintw(pad, y, 2, "Activity             :");
    for (int i = 0; i < MEM_ACT_HIST_LEN; i++) {
        int idx = (mem_act_pos + i) % MEM_ACT_HIST_LEN;
        mvwprintw(pad, y, 26 + i, "%s", bars[mem_act_hist[idx]]);
    }
    y++;
    

    section(&y, "Disk");
    if (load_disk(buf, sizeof(buf)) == 0) {
        snprintf(last_disk, sizeof(last_disk), "%s", buf);
        have_disk = 1;
    } else stale = 1;
    draw_line(&y, "Root (/) ", have_disk ? last_disk : "Unavailable");

    section(&y, "Network");
    if (load_net(&y) != 0) stale = 1;

    section(&y, "RUNTIME");
    /* Uptime */
    FILE *f = fopen("/proc/uptime", "r");
    double up = 0.0;
    if (f) {
        int valid = fscanf(f, "%lf", &up) == 1;
        fclose(f);
        if (valid) {
            last_uptime = up;
            have_uptime = 1;
        } else stale = 1;
    } else stale = 1;
    if (have_uptime) format_uptime(last_uptime, buf, sizeof(buf));
    else snprintf(buf, sizeof(buf), "Unavailable");
    draw_line(&y, "Uptime", buf);
    
    content_height = y;
    int max_scroll = content_height - vheight;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
    if (scroll_offset < 0) scroll_offset = 0;
    state->readings_stale = stale;
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
