# sysdash

A lightweight terminal-based Linux system monitor written in C using **ncursesw**. It provides two interactive views: a hardware/system dashboard and a process monitor with grouped processes, CPU/memory information, keyboard navigation, and an optional process-details side panel.

## Features

### Hardware dashboard

The **Hardware** tab displays live information gathered directly from the Linux system interfaces, including:

- Hostname
- Kernel release
- CPU model, core/thread count, frequency, and cache
- CPU utilization
- RAM and swap usage
- Memory activity history
- Root filesystem capacity and usage
- Per-interface network RX/TX rates
- System uptime
- Scrollable dashboard content

The implementation reads information from Linux interfaces such as `/proc/stat`, `/proc/cpuinfo`, `/proc/meminfo`, `/proc/vmstat`, `/proc/uptime`, and `/sys/class/net`.

### Process monitor

The **Processes** tab provides:

- Process discovery through `/proc`
- CPU and RSS memory usage
- Process grouping by application/process name
- Collapsed and expanded process groups
- Foreground-process filtering by default
- Toggle to show all discovered processes
- CPU and memory activity histories
- Process state, thread count, and file-descriptor count
- Process command line information
- An interactive process-details side panel
- CPU-based process sorting

By default, the process view focuses on processes belonging to the current user and excludes kernel threads. Press `a` to toggle between the default foreground view and all processes.

## Repository Name

Recommended GitHub repository name:

**`sysdash`**

The name matches the executable produced by the supplied Makefile (`sysdash.exe`) and describes the project's purpose as a system dashboard.

## Requirements

This project is currently intended for **Linux**. It directly accesses Linux-specific interfaces such as `/proc` and `/sys`, so it is not a portable POSIX/macOS/Windows system monitor in its current form.

### Required software

- GCC or another C compiler compatible with the supplied Makefile
- GNU Make
- ncursesw development headers and library
- A Linux environment with `/proc` and `/sys` mounted

The source includes `<ncursesw/ncurses.h>` and the Makefile links against `-lncursesw`, so the **development package**, not only the runtime ncurses library, is required.

### Optional software

- `cppcheck` for static analysis

## Installing prerequisites

### Debian / Ubuntu

```bash
sudo apt update
sudo apt install build-essential libncursesw5-dev
```

For optional Cppcheck analysis:

```bash
sudo apt install cppcheck
```

### Fedora / RHEL-compatible distributions

```bash
sudo dnf install gcc make ncurses-devel
```

For optional Cppcheck analysis:

```bash
sudo dnf install cppcheck
```

### Arch Linux

```bash
sudo pacman -S base-devel ncurses
```

For optional Cppcheck analysis:

```bash
sudo pacman -S cppcheck
```

If your distribution uses different package names, search its package manager for the **ncurses development package** and `cppcheck`.

## Project Structure

The supplied Makefile expects the project to be arranged as follows:

```text
sysdash/
├── include/
│   ├── hardware.h
│   ├── input.h
│   ├── process.h
│   ├── state.h
│   ├── tabs.h
│   └── ui.h
├── src/
│   ├── hardware.c
│   ├── input.c
│   ├── main.c
│   ├── process.c
│   ├── state.c
│   └── ui.c
├── cppcheck.suppress
├── Makefile
└── README.md
```

`tabs.h` is currently a placeholder header; the tab definitions used by the application are currently declared in `state.h`.

## Build

Clone the repository and enter its directory:

```bash
git clone <repository-url>
cd sysdash
```

Build the project with:

```bash
make
```

The supplied Makefile:

- Uses `gcc`
- Compiles all `src/*.c` files
- Uses the C11 standard
- Adds a broad set of compiler warnings
- Enables debug information with `-g`
- Includes headers from `include/`
- Links against `ncursesw`
- Removes intermediate object files after a successful link

The resulting executable is named:

```text
sysdash.exe
```

Although the filename ends in `.exe`, the supplied build configuration is Unix/Linux-oriented and links against Linux's ncursesw library. On Linux, this is simply the filename chosen by the Makefile; it is not a Windows executable.

## Run

After a successful build:

```bash
./sysdash.exe
```

The application starts on the **Hardware** tab.

## Keyboard Controls

### Global controls

| Key | Action |
|---|---|
| `q` / `Q` | Quit |
| `Tab` | Switch to the next tab |
| `Right Arrow` | Switch to the next tab |
| `Left Arrow` | Switch to the previous tab |

### Hardware tab

| Key | Action |
|---|---|
| `Up Arrow` | Scroll up |
| `Down Arrow` | Scroll down |
| `Page Up` | Scroll up by one visible page |
| `Page Down` | Scroll down by one visible page |

### Processes tab

| Key | Action |
|---|---|
| `Up Arrow` | Move selection up |
| `Down Arrow` | Move selection down |
| `a` / `A` | Toggle foreground/all process view |
| `Enter` | Expand the selected process group |
| `Backspace` | Collapse the selected group/process parent |
| `x` / `X` | Open/close the process-details side panel |

The side panel can be opened only when a child process is selected. It is automatically closed if the displayed process disappears.

## How It Works

The application follows a simple update/draw/input loop:

1. Initialize application state.
2. Initialize ncurses.
3. Initialize hardware and process monitoring modules.
4. Draw the active tab.
5. Wait for keyboard input, with ncurses configured for a maximum one-second wait.
6. Process global input.
7. Process hardware or process-tab input as appropriate.
8. Repeat until `q`/`Q` stops the application.
9. Shut down ncurses cleanly.

The main application loop is implemented in `src/main.c`, while UI rendering, input handling, system monitoring, process monitoring, and application state are separated into their respective modules.

## Data Sources

The monitor does not require a separate system-monitoring daemon. It reads Linux kernel-provided interfaces directly.

| Information | Source used by the application |
|---|---|
| CPU usage/details | `/proc/stat`, `/proc/cpuinfo` |
| Memory information | `/proc/meminfo`, `/proc/vmstat` |
| System uptime | `/proc/uptime` |
| Process list | `/proc` |
| Process CPU data | `/proc/<pid>/stat` and `/proc/stat` |
| Process memory/thread data | `/proc/<pid>/status` |
| Process command line | `/proc/<pid>/cmdline` |
| Process file descriptors | `/proc/<pid>/fd` |
| Network statistics | `/sys/class/net/<interface>/statistics/` |
| Root filesystem usage | `statvfs("/", ...)` |
| Host/kernel information | `gethostname()` and `uname()` |

## Process View Details

The process monitor maintains a fixed maximum of **1024 processes** and **256 process groups** according to the constants in `process.h`.

CPU usage is calculated from process CPU time relative to system CPU time and is smoothed over updates. Processes are sorted by CPU usage in descending order.

Process groups preserve their expanded/collapsed state across process-list updates by matching group names.

The process-details panel shows information such as:

- PID
- User
- Process state
- Thread count
- File-descriptor count
- CPU usage
- CPU history
- Memory usage
- Uptime
- Command line

## Static Analysis

The repository includes a `cppcheck.suppress` file containing a suppression for system-header include diagnostics.

If Cppcheck is installed, a suitable analysis command is:

```bash
cppcheck --enable=all --suppressions-list=cppcheck.suppress --std=c11 -I include src
```

## Cleaning the Build

Remove generated object files and the executable with:

```bash
make clean
```

## Terminal Requirements

Because the UI uses ncurses wide-character functionality and Unicode block characters for activity graphs and visual indicators, use a terminal with UTF-8/wide-character support for the intended display.

The application also initializes the locale before starting ncurses, allowing wide-character terminal output to be used.

## Current Limitations

- Linux-specific implementation due to direct use of `/proc`, `/sys`, Linux process metadata, and Linux-specific system interfaces.
- The supplied Makefile names the Linux output `sysdash.exe`; this is only an executable filename and does not make the program Windows-compatible.
- The process monitor has fixed-size limits defined in `process.h`.
- Hardware monitoring is focused on the root filesystem (`/`) rather than presenting every mounted filesystem.
- Network activity is reported per interface using the interfaces exposed through `/sys/class/net`.
- `tabs.h` is currently a placeholder and does not contain the application's tab definitions.

## License

No license is included, as requested.
