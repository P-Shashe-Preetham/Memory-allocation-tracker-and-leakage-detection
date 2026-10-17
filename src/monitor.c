#define _POSIX_C_SOURCE 200809L

#include "monitor.h"
#include "report.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop_monitoring = 0;

static void monitor_sig_handler(int signo) {
    (void)signo;
    g_stop_monitoring = 1;
}

void mw_growth_config_defaults(mw_growth_config_t *cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->min_samples         = 4;
    cfg->abs_threshold_bytes = 64ULL * 1024ULL; /* 64 KiB default threshold */
    cfg->pct_threshold       = 5.0;             /* 5.0% growth default */
    cfg->min_duration_ms     = 0;
}

mw_status_t mw_tracker_init(mw_tracker_t *tracker, size_t capacity, const mw_growth_config_t *config) {
    if (tracker == NULL || capacity == 0) {
        return MW_ERR_INVALID_ARG;
    }
    memset(tracker, 0, sizeof(*tracker));
    if (capacity > MW_MAX_TRACKED_PROCS) {
        capacity = MW_MAX_TRACKED_PROCS;
    }
    tracker->entries = calloc(capacity, sizeof(mw_proc_history_t));
    if (tracker->entries == NULL) {
        return MW_ERR_NOMEM;
    }
    tracker->capacity = capacity;
    tracker->count = 0;
    if (config != NULL) {
        tracker->config = *config;
    } else {
        mw_growth_config_defaults(&tracker->config);
    }
    return MW_OK;
}

void mw_tracker_free(mw_tracker_t *tracker) {
    if (tracker == NULL) {
        return;
    }
    free(tracker->entries);
    tracker->entries = NULL;
    tracker->capacity = 0;
    tracker->count = 0;
}

static const mw_sample_entry_t *history_get_sample(const mw_proc_history_t *hist, size_t logical_idx) {
    if (hist == NULL || logical_idx >= hist->count) {
        return NULL;
    }
    /*
     * When count < MW_HISTORY_CAPACITY, oldest sample is at index 0.
     * When count == MW_HISTORY_CAPACITY, oldest sample is at hist->head.
     */
    size_t start = (hist->count < MW_HISTORY_CAPACITY) ? 0 : hist->head;
    size_t phys_idx = (start + logical_idx) % MW_HISTORY_CAPACITY;
    return &hist->samples[phys_idx];
}

static void history_reset_slot(mw_proc_history_t *slot, const mw_proc_info_t *proc) {
    memset(slot, 0, sizeof(*slot));
    if (proc != NULL) {
        slot->pid             = proc->pid;
        slot->starttime_ticks = proc->starttime_ticks;
        mw_strlcpy(slot->name, proc->name, sizeof(slot->name));
        slot->active          = true;
    }
}

bool mw_evaluate_growth(const mw_proc_history_t *hist,
                        const mw_growth_config_t *cfg,
                        mw_growth_alert_t *out_alert) {
    if (out_alert != NULL) {
        memset(out_alert, 0, sizeof(*out_alert));
    }
    if (hist == NULL || cfg == NULL || !hist->active) {
        return false;
    }
    size_t req_samples = (cfg->min_samples < 2) ? 2 : cfg->min_samples;
    if (hist->count < req_samples) {
        return false;
    }

    const mw_sample_entry_t *first = history_get_sample(hist, 0);
    const mw_sample_entry_t *last  = history_get_sample(hist, hist->count - 1);
    if (first == NULL || last == NULL || !first->valid || !last->valid) {
        return false;
    }

    uint64_t duration_ms = (last->timestamp_ms >= first->timestamp_ms)
                           ? (last->timestamp_ms - first->timestamp_ms)
                           : 0;
    if (cfg->min_duration_ms > 0 && duration_ms < cfg->min_duration_ms) {
        return false;
    }

    /* Count consecutive RSS increases at the tail of the sample window */
    size_t consecutive_inc = 0;
    size_t total_inc = 0;
    size_t non_decreasing_steps = 0;

    for (size_t i = 1; i < hist->count; i++) {
        const mw_sample_entry_t *prev = history_get_sample(hist, i - 1);
        const mw_sample_entry_t *curr = history_get_sample(hist, i);
        if (prev == NULL || curr == NULL) continue;

        if (curr->rss_bytes > prev->rss_bytes) {
            consecutive_inc++;
            total_inc++;
            non_decreasing_steps++;
        } else if (curr->rss_bytes == prev->rss_bytes) {
            non_decreasing_steps++;
        } else {
            consecutive_inc = 0;
        }
    }

    if (last->rss_bytes <= first->rss_bytes) {
        return false;
    }

    uint64_t abs_growth = last->rss_bytes - first->rss_bytes;
    double pct_growth = (first->rss_bytes > 0)
                        ? (((double)abs_growth * 100.0) / (double)first->rss_bytes)
                        : 100.0;

    /*
     * Require:
     * 1. Net absolute growth >= configured byte threshold
     * 2. Net percentage growth >= configured percentage threshold
     * 3. Sustained upward trend: either (req_samples - 1) consecutive increases
     *    OR monotonic non-decreasing steps with at least (req_samples / 2) strict increases
     */
    size_t needed_inc = (req_samples > 2) ? (req_samples - 2) : 1;
    bool sustained_trend = (consecutive_inc >= needed_inc) ||
                           (non_decreasing_steps == hist->count - 1 && total_inc >= needed_inc);

    bool exceeds_thresholds = (abs_growth >= cfg->abs_threshold_bytes) &&
                              (pct_growth >= cfg->pct_threshold);

    if (sustained_trend && exceeds_thresholds) {
        if (out_alert != NULL) {
            out_alert->suspected_growth      = true;
            out_alert->pid                   = hist->pid;
            mw_strlcpy(out_alert->name, hist->name, sizeof(out_alert->name));
            out_alert->initial_rss_bytes     = first->rss_bytes;
            out_alert->current_rss_bytes     = last->rss_bytes;
            out_alert->abs_growth_bytes      = (int64_t)abs_growth;
            out_alert->pct_growth            = pct_growth;
            out_alert->sample_count          = hist->count;
            out_alert->consecutive_increases = consecutive_inc;
            out_alert->duration_ms           = duration_ms;

            char growth_str[32];
            mw_format_bytes(abs_growth, growth_str, sizeof(growth_str));
            snprintf(out_alert->reason, sizeof(out_alert->reason),
                     "Suspected memory growth: RSS grew by +%s (+%.1f%%) across %zu samples (%zu consecutive increases)",
                     growth_str, pct_growth, hist->count, consecutive_inc);
        }
        return true;
    }

    return false;
}

mw_status_t mw_tracker_record_sample(mw_tracker_t *tracker,
                                     mw_proc_info_t *proc,
                                     mw_growth_alert_t *out_alert) {
    if (tracker == NULL || proc == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    if (out_alert != NULL) {
        memset(out_alert, 0, sizeof(*out_alert));
    }

    /* Do not record or fabricate history if RSS is missing */
    if (!proc->rss_valid) {
        proc->has_rss_delta = false;
        proc->rss_delta_bytes = 0;
        return MW_OK;
    }

    mw_proc_history_t *slot = NULL;
    mw_proc_history_t *free_slot = NULL;
    mw_proc_history_t *oldest_slot = NULL;

    for (size_t i = 0; i < tracker->capacity; i++) {
        mw_proc_history_t *cand = &tracker->entries[i];
        if (cand->active && cand->pid == proc->pid) {
            slot = cand;
            break;
        }
        if (!cand->active && free_slot == NULL) {
            free_slot = cand;
        }
        if (cand->active) {
            if (oldest_slot == NULL || cand->last_seen_ms < oldest_slot->last_seen_ms) {
                oldest_slot = cand;
            }
        }
    }

    if (slot != NULL) {
        /*
         * Check for PID reuse:
         * If starttime_ticks is available and differs, or if starttime_ticks is 0
         * and the process name changed, a new process reused the same PID.
         * Reset the history slot so histories of distinct processes never merge.
         */
        bool pid_reused = false;
        if (slot->starttime_ticks != 0 && proc->starttime_ticks != 0 &&
            slot->starttime_ticks != proc->starttime_ticks) {
            pid_reused = true;
        } else if (slot->starttime_ticks == 0 && proc->starttime_ticks == 0 &&
                   strcmp(slot->name, proc->name) != 0) {
            pid_reused = true;
        }

        if (pid_reused) {
            history_reset_slot(slot, proc);
        }
    } else {
        if (free_slot != NULL) {
            slot = free_slot;
            tracker->count++;
        } else {
            /* Bounded history eviction of least-recently-seen process */
            slot = oldest_slot;
        }
        history_reset_slot(slot, proc);
    }

    /* Compute RSS change relative to previous sample */
    if (slot->count > 0) {
        const mw_sample_entry_t *prev = history_get_sample(slot, slot->count - 1);
        if (prev != NULL && prev->valid) {
            proc->rss_delta_bytes = (int64_t)proc->rss_bytes - (int64_t)prev->rss_bytes;
            proc->has_rss_delta = true;
        }
    } else {
        proc->rss_delta_bytes = 0;
        proc->has_rss_delta = false;
    }

    /* Append new sample to bounded ring buffer */
    mw_sample_entry_t entry;
    entry.timestamp_ms = (proc->sample_time_ms > 0) ? proc->sample_time_ms : mw_monotonic_ms();
    entry.rss_bytes    = proc->rss_bytes;
    entry.vsz_bytes    = proc->vsz_bytes;
    entry.valid        = true;

    slot->samples[slot->head] = entry;
    slot->head = (slot->head + 1) % MW_HISTORY_CAPACITY;
    if (slot->count < MW_HISTORY_CAPACITY) {
        slot->count++;
    }
    slot->last_seen_ms = entry.timestamp_ms;
    mw_strlcpy(slot->name, proc->name, sizeof(slot->name));
    if (proc->starttime_ticks != 0) {
        slot->starttime_ticks = proc->starttime_ticks;
    }

    mw_evaluate_growth(slot, &tracker->config, out_alert);
    return MW_OK;
}

void mw_tracker_prune_disappeared(mw_tracker_t *tracker, uint64_t current_scan_ms) {
    if (tracker == NULL || tracker->entries == NULL) {
        return;
    }
    for (size_t i = 0; i < tracker->capacity; i++) {
        mw_proc_history_t *slot = &tracker->entries[i];
        if (slot->active && slot->last_seen_ms < current_scan_ms) {
            memset(slot, 0, sizeof(*slot));
            if (tracker->count > 0) {
                tracker->count--;
            }
        }
    }
}

mw_status_t mw_run_monitor(const mw_options_t *opts) {
    if (opts == NULL) {
        return MW_ERR_INVALID_ARG;
    }

    g_stop_monitoring = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = monitor_sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    mw_growth_config_t gcfg;
    mw_growth_config_defaults(&gcfg);
    if (opts->min_samples >= 2) {
        gcfg.min_samples = opts->min_samples;
    }
    gcfg.abs_threshold_bytes = opts->growth_threshold_bytes;
    gcfg.pct_threshold       = opts->growth_threshold_pct;
    if (opts->min_sustained_sec > 0) {
        gcfg.min_duration_ms = (uint64_t)opts->min_sustained_sec * 1000ULL;
    }

    mw_tracker_t tracker;
    mw_status_t st = mw_tracker_init(&tracker, MW_MAX_TRACKED_PROCS, &gcfg);
    if (st != MW_OK) {
        return st;
    }

    bool is_tty    = isatty(fileno(stdout));
    bool use_color = mw_should_use_color(stdout, opts->no_color);
    int interval   = (opts->interval_sec > 0) ? opts->interval_sec : 1;
    int duration   = opts->duration_sec;

    uint64_t start_ms = mw_monotonic_ms();
    size_t iteration  = 0;

    while (!g_stop_monitoring) {
        uint64_t loop_ms = mw_monotonic_ms();
        if (duration > 0 && iteration > 0) {
            uint64_t elapsed_sec = (loop_ms - start_ms) / 1000ULL;
            if (elapsed_sec >= (uint64_t)duration) {
                break;
            }
        }

        mw_sys_memory_t sys_mem;
        st = mw_read_meminfo(opts->proc_root, &sys_mem);
        if (st != MW_OK) {
            fprintf(stderr, "Error reading '%s/meminfo': %s\n", opts->proc_root, mw_status_str(st));
            mw_tracker_free(&tracker);
            return st;
        }

        if (opts->target_pid > 0) {
            /* Single-process monitoring mode */
            mw_proc_info_t proc;
            st = mw_read_process(opts->proc_root, opts->target_pid, sys_mem.mem_total_bytes, &proc);
            if (st != MW_OK) {
                if (st == MW_ERR_NOT_FOUND) {
                    if (iteration == 0) {
                        fprintf(stderr, "Error: Process PID %d does not exist.\n", (int)opts->target_pid);
                        mw_tracker_free(&tracker);
                        return MW_ERR_NOT_FOUND;
                    } else {
                        fprintf(stdout, "\n[MemWatch] Monitored process PID %d terminated during sampling.\n",
                                (int)opts->target_pid);
                        mw_tracker_prune_disappeared(&tracker, loop_ms);
                        break;
                    }
                } else if (st == MW_ERR_PERMISSION) {
                    fprintf(stderr, "Error: Permission denied inspecting PID %d. Try running with appropriate privileges.\n",
                            (int)opts->target_pid);
                    mw_tracker_free(&tracker);
                    return MW_ERR_PERMISSION;
                } else {
                    fprintf(stderr, "Error reading PID %d: %s\n", (int)opts->target_pid, mw_status_str(st));
                    mw_tracker_free(&tracker);
                    return st;
                }
            }

            proc.sample_time_ms = loop_ms;
            mw_growth_alert_t alert;
            mw_tracker_record_sample(&tracker, &proc, &alert);
            size_t alert_count = alert.suspected_growth ? 1 : 0;

            if (is_tty) {
                fprintf(stdout, "%s", MW_ANSI_CLEAR_SCREEN);
            } else if (iteration > 0) {
                fprintf(stdout, "\n");
            }

            char ts_buf[32];
            mw_format_timestamp(ts_buf, sizeof(ts_buf));
            const char *c_bold  = mw_color(use_color, MW_ANSI_BOLD);
            const char *c_cyan  = mw_color(use_color, MW_ANSI_CYAN);
            const char *c_reset = mw_color(use_color, MW_ANSI_RESET);

            fprintf(stdout, "%s%sMEMWATCH — PROCESS MONITOR (PID %d)%s  [%s | Sample #%zu | Interval: %ds]\n\n",
                    c_bold, c_cyan, (int)opts->target_pid, c_reset, ts_buf, iteration + 1, interval);

            mw_print_process_table(stdout, &proc, 1, 1,
                                   alert_count ? &alert : NULL, alert_count,
                                   true, use_color);
            if (alert_count > 0) {
                fprintf(stdout, "\n");
                mw_print_growth_alerts(stdout, &alert, 1, use_color);
            }
            fflush(stdout);
        } else {
            /* System-wide live process monitor */
            mw_proc_info_t *procs = NULL;
            size_t proc_count = 0;
            size_t inaccessible = 0;

            st = mw_enum_processes(opts->proc_root, sys_mem.mem_total_bytes, &procs, &proc_count, &inaccessible);
            if (st != MW_OK) {
                fprintf(stderr, "Error enumerating processes: %s\n", mw_status_str(st));
                mw_tracker_free(&tracker);
                return st;
            }
            sys_mem.total_processes = proc_count + inaccessible;

            mw_growth_alert_t *alerts = NULL;
            size_t alert_count = 0;
            if (proc_count > 0) {
                alerts = calloc(proc_count, sizeof(mw_growth_alert_t));
            }

            for (size_t i = 0; i < proc_count; i++) {
                procs[i].sample_time_ms = loop_ms;
                mw_growth_alert_t alert;
                mw_tracker_record_sample(&tracker, &procs[i], &alert);
                if (alert.suspected_growth && alerts != NULL) {
                    alerts[alert_count++] = alert;
                }
            }

            mw_tracker_prune_disappeared(&tracker, loop_ms);
            mw_sort_processes_by_rss(procs, proc_count);

            if (is_tty) {
                fprintf(stdout, "%s", MW_ANSI_CLEAR_SCREEN);
            } else if (iteration > 0) {
                fprintf(stdout, "\n");
            }

            char ts_buf[32];
            mw_format_timestamp(ts_buf, sizeof(ts_buf));
            const char *c_bold  = mw_color(use_color, MW_ANSI_BOLD);
            const char *c_cyan  = mw_color(use_color, MW_ANSI_CYAN);
            const char *c_reset = mw_color(use_color, MW_ANSI_RESET);

            fprintf(stdout, "%s%sMEMWATCH — LIVE MEMORY MONITOR%s  [%s | Sample #%zu | Interval: %ds | Ctrl+C to stop]\n\n",
                    c_bold, c_cyan, c_reset, ts_buf, iteration + 1, interval);

            mw_print_system_memory(stdout, &sys_mem, inaccessible, use_color);
            fprintf(stdout, "\n");

            size_t max_rows = (opts->top_n > 0) ? opts->top_n : 15;
            mw_print_process_table(stdout, procs, proc_count, max_rows,
                                   alerts, alert_count, true, use_color);

            if (alert_count > 0) {
                fprintf(stdout, "\n");
                mw_print_growth_alerts(stdout, alerts, alert_count, use_color);
            }

            fflush(stdout);
            free(alerts);
            free(procs);
        }

        iteration++;
        if (duration > 0) {
            uint64_t now_ms = mw_monotonic_ms();
            if ((now_ms - start_ms) / 1000ULL >= (uint64_t)duration) {
                break;
            }
        }

        /* Sleep in 100ms increments so Ctrl+C is responsive */
        int ticks = interval * 10;
        for (int t = 0; t < ticks && !g_stop_monitoring; t++) {
            if (duration > 0) {
                uint64_t now_ms = mw_monotonic_ms();
                if ((now_ms - start_ms) >= (uint64_t)duration * 1000ULL) {
                    break;
                }
            }
            struct timespec req = {0, 100000000L}; /* 100 ms */
            nanosleep(&req, NULL);
        }
    }

    if (g_stop_monitoring) {
        fprintf(stdout, "\n[MemWatch] Monitoring stopped cleanly via signal. Releasing resources.\n");
    }

    mw_tracker_free(&tracker);
    return MW_OK;
}
