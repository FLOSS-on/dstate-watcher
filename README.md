I don't know English, so the description was translated by an AI.

# Linux D-State Monitor (Kernel Module + Go TUI)

A real-time monitoring tool for tracking processes in `TASK_UNINTERRUPTIBLE` (D-state) on Linux. It consists of a loadable kernel module (C) that hooks into scheduler tracepoints and a terminal UI (Go) that reads and displays the statistics from `/proc`.

## Disclaimer & Project Background

> **Note:** This is an educational project and my very first attempt at writing C and Linux kernel code.
>
> While the Go TUI part was written with AI assistance (a bit of "vibe coding"), the core kernel module is not purely AI-generated—the main goal of this project was to learn C, Linux kernel internals, and kernel APIs rather than Go libraries.
>
> Because this is a learning project, I cannot guarantee 100% stability or completely accurate statistics under extreme loads. The module was originally developed and tested on Linux kernel **6.12.x**, and it has also been verified to build and run on **6.18**.

## Architecture

* **Kernel Module (`C`):** Uses `sched_switch` and `sched_wakeup` tracepoints, lock-free per-CPU ring buffers (`kfifo`), bitmaps for fast PID filtering, and `delayed_work` to expose active D-state tasks via `/proc/my_dir/dstat_info`.
* **Userspace TUI (`Go`):** Reads `/proc/my_dir/dstat_info` and renders a live table with PIDs, process names, and time spent in D-state using `tview`/`tcell`.

## Build & Run

### 1. Build and load the kernel module
```bash
make
sudo insmod your_module_name.ko
```

### 2. Run the TUI monitor
```bash
go run .
```
*(Press `q` to quit)*

### 3. Unload the module
```bash
sudo rmmod your_module_name
```
