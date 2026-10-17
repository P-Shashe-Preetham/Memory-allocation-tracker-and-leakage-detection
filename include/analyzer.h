#ifndef ANALYZER_H
#define ANALYZER_H

#include "memwatch.h"

typedef enum {
    MW_LEAK_DEFINITELY_LOST = 0,
    MW_LEAK_INDIRECTLY_LOST,
    MW_LEAK_POSSIBLY_LOST,
    MW_LEAK_STILL_REACHABLE,
    MW_LEAK_SUPPRESSED,
    MW_LEAK_OTHER_ERROR,
    MW_LEAK_KIND_COUNT
} mw_leak_kind_t;

typedef struct {
    char ip[32];
    char fn[128];
    char obj[MW_MAX_PATH];
    char dir[MW_MAX_PATH];
    char file[128];
    int  line;
} mw_stack_frame_t;

typedef struct {
    char             unique_id[32];
    mw_leak_kind_t   kind;
    char             raw_kind[64];
    char             what[256];
    uint64_t         leaked_bytes;
    uint64_t         leaked_blocks;
    mw_stack_frame_t frames[MW_MAX_STACK_FRAMES];
    size_t           frame_count;
} mw_leak_record_t;

typedef struct {
    char              target_exe[MW_MAX_PATH];
    char              target_args[MW_MAX_CMDLINE];
    char              xml_path[MW_MAX_PATH];
    char              summary_report_path[MW_MAX_PATH];
    pid_t             analyzed_pid;
    int               child_exit_code;
    int               child_term_signal;
    bool              child_exited_normally;
    bool              child_timed_out;
    bool              has_debug_info;
    bool              has_stack_traces;

    /* Aggregated leak totals (without double-counting) */
    uint64_t          definitely_lost_bytes;
    uint64_t          definitely_lost_blocks;
    uint64_t          indirectly_lost_bytes;
    uint64_t          indirectly_lost_blocks;
    uint64_t          possibly_lost_bytes;
    uint64_t          possibly_lost_blocks;
    uint64_t          still_reachable_bytes;
    uint64_t          still_reachable_blocks;
    uint64_t          suppressed_bytes;
    uint64_t          suppressed_blocks;
    size_t            non_leak_error_count;

    mw_leak_record_t *records;
    size_t            record_count;
    size_t            record_capacity;
} mw_leak_report_t;

void mw_leak_report_init(mw_leak_report_t *report);
void mw_leak_report_free(mw_leak_report_t *report);
const char *mw_leak_kind_label(mw_leak_kind_t kind);
mw_leak_kind_t mw_classify_valgrind_kind(const char *kind_str);

/* Valgrind XML parsing (src/valgrind_parser.c) */
mw_status_t mw_parse_valgrind_xml_file(const char *xml_path, mw_leak_report_t *out_report);
mw_status_t mw_parse_valgrind_xml_memory(const char *xml_buf, size_t len, mw_leak_report_t *out_report);

/* Deep analysis execution (src/leak_analyzer.c) */
mw_status_t mw_run_valgrind_target(const char *valgrind_bin,
                                   int target_argc,
                                   char **target_argv,
                                   const char *xml_output_path,
                                   int timeout_sec,
                                   mw_leak_report_t *out_report);
mw_status_t mw_run_analyze(const mw_options_t *opts);
mw_status_t mw_run_report_cmd(const mw_options_t *opts);

#endif /* ANALYZER_H */
