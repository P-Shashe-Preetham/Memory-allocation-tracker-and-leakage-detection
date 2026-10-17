#ifndef MEMWATCH_H
#define MEMWATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define MEMWATCH_VERSION "1.0.0"

#define MW_MAX_PATH          512
#define MW_MAX_NAME          128
#define MW_MAX_CMDLINE       1024
#define MW_MAX_STACK_FRAMES  16
#define MW_HISTORY_CAPACITY  64
#define MW_MAX_TRACKED_PROCS 2048
#define MW_MAX_REPORT_FILES  50

typedef enum {
    MW_OK = 0,
    MW_ERR_INVALID_ARG  = -1,
    MW_ERR_IO           = -2,
    MW_ERR_PERMISSION   = -3,
    MW_ERR_NOT_FOUND    = -4,
    MW_ERR_NOMEM        = -5,
    MW_ERR_PARSE        = -6,
    MW_ERR_TOOL_MISSING = -7,
    MW_ERR_EXEC         = -8,
    MW_ERR_TIMEOUT      = -9,
    MW_ERR_OVERFLOW     = -10
} mw_status_t;

typedef enum {
    MW_CMD_NONE = 0,
    MW_CMD_SYSTEM,
    MW_CMD_MONITOR,
    MW_CMD_TOP,
    MW_CMD_ANALYZE,
    MW_CMD_REPORT,
    MW_CMD_HELP,
    MW_CMD_VERSION
} mw_command_t;

/* ANSI Escape Codes */
#define MW_ANSI_RESET        "\033[0m"
#define MW_ANSI_BOLD         "\033[1m"
#define MW_ANSI_DIM          "\033[2m"
#define MW_ANSI_RED          "\033[31m"
#define MW_ANSI_GREEN        "\033[32m"
#define MW_ANSI_YELLOW       "\033[33m"
#define MW_ANSI_BLUE         "\033[34m"
#define MW_ANSI_MAGENTA      "\033[35m"
#define MW_ANSI_CYAN         "\033[36m"
#define MW_ANSI_CLEAR_SCREEN "\033[2J\033[H"

typedef struct {
    mw_command_t command;
    pid_t        target_pid;
    int          interval_sec;
    int          duration_sec;
    size_t       top_n;
    size_t       min_samples;
    uint64_t     growth_threshold_bytes;
    double       growth_threshold_pct;
    int          min_sustained_sec;
    int          timeout_sec;
    bool         no_color;
    char         proc_root[MW_MAX_PATH];
    char         output_dir[MW_MAX_PATH];
    char         report_path[MW_MAX_PATH];
    char         valgrind_bin[MW_MAX_PATH];
    int          target_argc;
    char       **target_argv;
} mw_options_t;

/* CLI functions (src/cli.c) */
void mw_options_init_defaults(mw_options_t *opts);
mw_status_t mw_parse_args(int argc, char **argv, mw_options_t *opts, char *err_buf, size_t err_buf_len);
void mw_print_help(FILE *out, const char *prog_name);
void mw_print_version(FILE *out);

/* Utility functions (src/utils.c) */
const char *mw_status_str(mw_status_t status);
size_t mw_strlcpy(char *dst, const char *src, size_t dst_size);
char *mw_strtrim(char *str);
mw_status_t mw_parse_u64(const char *str, uint64_t *out_val);
mw_status_t mw_parse_i32(const char *str, int *out_val);
mw_status_t mw_parse_double(const char *str, double *out_val);
mw_status_t mw_kb_to_bytes(uint64_t kb, uint64_t *out_bytes);
double mw_calc_percentage(uint64_t part, uint64_t total);
void mw_format_bytes(uint64_t bytes, char *buf, size_t buf_len);
void mw_format_bytes_with_commas(uint64_t bytes, char *buf, size_t buf_len);
void mw_format_signed_bytes(int64_t delta_bytes, char *buf, size_t buf_len);
uint64_t mw_monotonic_ms(void);
void mw_format_timestamp(char *buf, size_t buf_len);
bool mw_should_use_color(FILE *stream, bool force_no_color);
const char *mw_color(bool enabled, const char *ansi_code);
mw_status_t mw_find_executable(const char *name_or_path, char *resolved_out, size_t resolved_len);
mw_status_t mw_mkdir_p(const char *dir_path);

#endif /* MEMWATCH_H */
