#ifndef MONITOR_H
#define MONITOR_H

#include "memwatch.h"
#include "proc_reader.h"

typedef struct {
    uint64_t timestamp_ms;
    uint64_t rss_bytes;
    uint64_t vsz_bytes;
    bool     valid;
} mw_sample_entry_t;

typedef struct {
    pid_t             pid;
    uint64_t          starttime_ticks;
    char              name[MW_MAX_NAME];
    mw_sample_entry_t samples[MW_HISTORY_CAPACITY];
    size_t            head;
    size_t            count;
    uint64_t          last_seen_ms;
    bool              active;
} mw_proc_history_t;

typedef struct {
    size_t   min_samples;
    uint64_t abs_threshold_bytes;
    double   pct_threshold;
    uint64_t min_duration_ms;
} mw_growth_config_t;

typedef struct {
    bool     suspected_growth;
    pid_t    pid;
    char     name[MW_MAX_NAME];
    uint64_t initial_rss_bytes;
    uint64_t current_rss_bytes;
    int64_t  abs_growth_bytes;
    double   pct_growth;
    size_t   sample_count;
    size_t   consecutive_increases;
    uint64_t duration_ms;
    char     reason[256];
} mw_growth_alert_t;

typedef struct {
    mw_proc_history_t *entries;
    size_t             capacity;
    size_t             count;
    mw_growth_config_t config;
} mw_tracker_t;

void mw_growth_config_defaults(mw_growth_config_t *cfg);
mw_status_t mw_tracker_init(mw_tracker_t *tracker, size_t capacity, const mw_growth_config_t *config);
void mw_tracker_free(mw_tracker_t *tracker);

mw_status_t mw_tracker_record_sample(mw_tracker_t *tracker,
                                     mw_proc_info_t *proc,
                                     mw_growth_alert_t *out_alert);

void mw_tracker_prune_disappeared(mw_tracker_t *tracker, uint64_t current_scan_ms);

bool mw_evaluate_growth(const mw_proc_history_t *hist,
                        const mw_growth_config_t *cfg,
                        mw_growth_alert_t *out_alert);

mw_status_t mw_run_monitor(const mw_options_t *opts);

#endif /* MONITOR_H */
