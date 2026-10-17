#define _POSIX_C_SOURCE 200809L

#include "proc_reader.h"
#include "report.h"

#include <stdlib.h>

mw_status_t mw_collect_system_snapshot(const char *proc_root,
                                       mw_sys_memory_t *out_mem,
                                       mw_proc_info_t **out_procs,
                                       size_t *out_proc_count,
                                       size_t *out_inaccessible) {
    if (proc_root == NULL || out_mem == NULL || out_procs == NULL || out_proc_count == NULL) {
        return MW_ERR_INVALID_ARG;
    }

    mw_status_t st = mw_read_meminfo(proc_root, out_mem);
    if (st != MW_OK) {
        return st;
    }

    mw_proc_info_t *procs = NULL;
    size_t proc_count = 0;
    size_t inaccessible = 0;

    st = mw_enum_processes(proc_root, out_mem->mem_total_bytes, &procs, &proc_count, &inaccessible);
    if (st != MW_OK) {
        return st;
    }

    mw_sort_processes_by_rss(procs, proc_count);
    out_mem->total_processes = proc_count + inaccessible;

    *out_procs = procs;
    *out_proc_count = proc_count;
    if (out_inaccessible) {
        *out_inaccessible = inaccessible;
    }
    return MW_OK;
}

mw_status_t mw_run_system_overview(const mw_options_t *opts) {
    if (opts == NULL) {
        return MW_ERR_INVALID_ARG;
    }

    mw_sys_memory_t mem;
    mw_proc_info_t *procs = NULL;
    size_t proc_count = 0;
    size_t inaccessible = 0;

    mw_status_t st = mw_collect_system_snapshot(opts->proc_root, &mem, &procs, &proc_count, &inaccessible);
    if (st != MW_OK) {
        fprintf(stderr, "Error reading system memory from '%s': %s\n",
                opts->proc_root, mw_status_str(st));
        return st;
    }

    bool use_color = mw_should_use_color(stdout, opts->no_color);
    mw_print_system_memory(stdout, &mem, inaccessible, use_color);

    fprintf(stdout, "\n");
    size_t top_rows = (opts->top_n > 0) ? opts->top_n : 10;
    mw_print_process_table(stdout, procs, proc_count, top_rows, NULL, 0, false, use_color);

    free(procs);
    return MW_OK;
}

mw_status_t mw_run_top(const mw_options_t *opts) {
    if (opts == NULL) {
        return MW_ERR_INVALID_ARG;
    }

    mw_sys_memory_t mem;
    mw_proc_info_t *procs = NULL;
    size_t proc_count = 0;
    size_t inaccessible = 0;

    mw_status_t st = mw_collect_system_snapshot(opts->proc_root, &mem, &procs, &proc_count, &inaccessible);
    if (st != MW_OK) {
        fprintf(stderr, "Error collecting process snapshot from '%s': %s\n",
                opts->proc_root, mw_status_str(st));
        return st;
    }

    bool use_color = mw_should_use_color(stdout, opts->no_color);
    size_t top_rows = (opts->top_n > 0) ? opts->top_n : 15;

    const char *c_bold  = mw_color(use_color, MW_ANSI_BOLD);
    const char *c_cyan  = mw_color(use_color, MW_ANSI_CYAN);
    const char *c_reset = mw_color(use_color, MW_ANSI_RESET);

    char total_str[32], used_str[32], avail_str[32];
    mw_format_bytes(mem.mem_total_bytes, total_str, sizeof(total_str));
    mw_format_bytes(mem.used_bytes, used_str, sizeof(used_str));
    mw_format_bytes(mem.mem_available_bytes, avail_str, sizeof(avail_str));

    fprintf(stdout, "%s%sMEMWATCH — TOP MEMORY CONSUMERS%s\n", c_bold, c_cyan, c_reset);
    fprintf(stdout, "System RAM: %s total | %s used (%.1f%%) | %s available | Processes: %zu\n\n",
            total_str, used_str, mem.utilization_pct, avail_str, mem.total_processes);

    mw_print_process_table(stdout, procs, proc_count, top_rows, NULL, 0, false, use_color);

    if (inaccessible > 0) {
        fprintf(stdout, "\nNote: %zu process(es) could not be inspected due to permissions.\n", inaccessible);
    }

    free(procs);
    return MW_OK;
}
