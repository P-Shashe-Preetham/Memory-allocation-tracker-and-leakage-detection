#define _POSIX_C_SOURCE 200809L

#include "monitor.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_sustained_growth_flagged(void) {
    mw_growth_config_t cfg;
    mw_growth_config_defaults(&cfg);
    cfg.min_samples         = 4;
    cfg.abs_threshold_bytes = 64ULL * 1024ULL; /* 64 KiB */
    cfg.pct_threshold       = 10.0;            /* 10% */
    cfg.min_duration_ms     = 3000;            /* 3 seconds */

    mw_tracker_t tracker;
    assert(mw_tracker_init(&tracker, 64, &cfg) == MW_OK);

    uint64_t rss_samples[4] = {
        1000ULL * 1024ULL,
        1100ULL * 1024ULL,
        1250ULL * 1024ULL,
        1400ULL * 1024ULL
    };

    mw_growth_alert_t alert;
    for (size_t i = 0; i < 4; i++) {
        mw_proc_info_t p;
        memset(&p, 0, sizeof(p));
        p.pid             = 5001;
        mw_strlcpy(p.name, "leaky_daemon", sizeof(p.name));
        p.rss_valid       = true;
        p.vsz_valid       = true;
        p.rss_bytes       = rss_samples[i];
        p.vsz_bytes       = rss_samples[i] * 2;
        p.starttime_ticks = 123456ULL;
        p.sample_time_ms  = 1000ULL + i * 1000ULL;

        assert(mw_tracker_record_sample(&tracker, &p, &alert) == MW_OK);
        if (i < 3) {
            assert(alert.suspected_growth == false);
        }
    }

    assert(alert.suspected_growth == true);
    assert(alert.pid == 5001);
    assert(alert.abs_growth_bytes == (int64_t)(400ULL * 1024ULL));
    assert(alert.consecutive_increases == 3);
    assert(strstr(alert.reason, "Suspected memory growth") != NULL);

    mw_tracker_free(&tracker);
}

static void test_oscillating_memory_not_flagged(void) {
    mw_growth_config_t cfg;
    mw_growth_config_defaults(&cfg);
    cfg.min_samples         = 4;
    cfg.abs_threshold_bytes = 32ULL * 1024ULL;
    cfg.pct_threshold       = 5.0;

    mw_tracker_t tracker;
    assert(mw_tracker_init(&tracker, 64, &cfg) == MW_OK);

    uint64_t rss_samples[4] = {
        1000ULL * 1024ULL,
        1200ULL * 1024ULL,
        950ULL  * 1024ULL,
        1010ULL * 1024ULL
    };

    mw_growth_alert_t alert;
    for (size_t i = 0; i < 4; i++) {
        mw_proc_info_t p;
        memset(&p, 0, sizeof(p));
        p.pid             = 5002;
        mw_strlcpy(p.name, "normal_worker", sizeof(p.name));
        p.rss_valid       = true;
        p.rss_bytes       = rss_samples[i];
        p.starttime_ticks = 77777ULL;
        p.sample_time_ms  = 1000ULL + i * 1000ULL;

        assert(mw_tracker_record_sample(&tracker, &p, &alert) == MW_OK);
        assert(alert.suspected_growth == false);
    }

    mw_tracker_free(&tracker);
}

static void test_pid_reuse_resets_history(void) {
    mw_growth_config_t cfg;
    mw_growth_config_defaults(&cfg);
    cfg.min_samples         = 4;
    cfg.abs_threshold_bytes = 64ULL * 1024ULL;
    cfg.pct_threshold       = 5.0;

    mw_tracker_t tracker;
    assert(mw_tracker_init(&tracker, 64, &cfg) == MW_OK);

    /* Old process on PID 7000 grows for 3 samples */
    for (size_t i = 0; i < 3; i++) {
        mw_proc_info_t p;
        memset(&p, 0, sizeof(p));
        p.pid             = 7000;
        mw_strlcpy(p.name, "old_proc", sizeof(p.name));
        p.rss_valid       = true;
        p.rss_bytes       = (1000ULL + i * 200ULL) * 1024ULL;
        p.starttime_ticks = 10000ULL;
        p.sample_time_ms  = 1000ULL + i * 1000ULL;

        mw_growth_alert_t alert;
        assert(mw_tracker_record_sample(&tracker, &p, &alert) == MW_OK);
    }

    /* New process reuses PID 7000 with a different starttime_ticks */
    mw_proc_info_t new_proc;
    memset(&new_proc, 0, sizeof(new_proc));
    new_proc.pid             = 7000;
    mw_strlcpy(new_proc.name, "new_proc", sizeof(new_proc.name));
    new_proc.rss_valid       = true;
    new_proc.rss_bytes       = 2000ULL * 1024ULL;
    new_proc.starttime_ticks = 99999ULL; /* Different start time! */
    new_proc.sample_time_ms  = 4000ULL;

    mw_growth_alert_t alert;
    assert(mw_tracker_record_sample(&tracker, &new_proc, &alert) == MW_OK);
    /* Must NOT be flagged and must NOT have a delta against old_proc */
    assert(alert.suspected_growth == false);
    assert(new_proc.has_rss_delta == false);

    mw_tracker_free(&tracker);
}

int main(void) {
    test_sustained_growth_flagged();
    test_oscillating_memory_not_flagged();
    test_pid_reuse_resets_history();
    printf("[PASS] test_growth_detection: all assertions passed.\n");
    return 0;
}
