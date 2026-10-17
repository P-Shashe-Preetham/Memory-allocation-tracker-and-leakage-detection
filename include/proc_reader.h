#ifndef PROC_READER_H
#define PROC_READER_H

#include "memwatch.h"

typedef struct {
    uint64_t mem_total_bytes;
    uint64_t mem_free_bytes;
    uint64_t mem_available_bytes;
    uint64_t buffers_bytes;
    uint64_t cached_bytes;
    uint64_t shmem_bytes;
    uint64_t sreclaimable_bytes;
    uint64_t used_bytes;
    double   utilization_pct;
    bool     has_mem_available;
    bool     swap_available;
    uint64_t swap_total_bytes;
    uint64_t swap_free_bytes;
    uint64_t swap_used_bytes;
    double   swap_utilization_pct;
    size_t   total_processes;
} mw_sys_memory_t;

typedef struct {
    pid_t    pid;
    pid_t    ppid;
    uid_t    uid;
    char     name[MW_MAX_NAME];
    char     state;
    char     state_desc[32];
    bool     rss_valid;
    bool     vsz_valid;
    bool     pss_valid;
    bool     is_kernel_thread;
    bool     permission_denied;
    uint64_t rss_bytes;
    uint64_t vsz_bytes;
    uint64_t pss_bytes;
    uint64_t private_dirty_bytes;
    double   mem_pct;
    int64_t  rss_delta_bytes;
    bool     has_rss_delta;
    uint64_t starttime_ticks;
    uint64_t sample_time_ms;
} mw_proc_info_t;

/* Parse and read /proc/meminfo */
mw_status_t mw_parse_meminfo_str(const char *content, mw_sys_memory_t *out_mem);
mw_status_t mw_read_meminfo(const char *proc_root, mw_sys_memory_t *out_mem);

/* Parse and read /proc/[PID] entries */
mw_status_t mw_parse_proc_status_str(const char *content, mw_proc_info_t *out_proc);
mw_status_t mw_parse_smaps_rollup_str(const char *content, mw_proc_info_t *out_proc);
mw_status_t mw_parse_proc_stat_str(const char *content, char *out_state, uint64_t *out_starttime);

mw_status_t mw_read_process(const char *proc_root,
                            pid_t pid,
                            uint64_t total_mem_bytes,
                            mw_proc_info_t *out_proc);

mw_status_t mw_enum_processes(const char *proc_root,
                              uint64_t total_mem_bytes,
                              mw_proc_info_t **out_list,
                              size_t *out_count,
                              size_t *out_inaccessible);

void mw_sort_processes_by_rss(mw_proc_info_t *list, size_t count);

/* High-level system memory commands (src/system_memory.c) */
mw_status_t mw_collect_system_snapshot(const char *proc_root,
                                       mw_sys_memory_t *out_mem,
                                       mw_proc_info_t **out_procs,
                                       size_t *out_proc_count,
                                       size_t *out_inaccessible);
mw_status_t mw_run_system_overview(const mw_options_t *opts);
mw_status_t mw_run_top(const mw_options_t *opts);

#endif /* PROC_READER_H */
