#define _POSIX_C_SOURCE 200809L

#include "report.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void mw_print_system_memory(FILE *out,
                            const mw_sys_memory_t *mem,
                            size_t inaccessible_procs,
                            bool use_color) {
    if (out == NULL || mem == NULL) {
        return;
    }

    const char *c_bold   = mw_color(use_color, MW_ANSI_BOLD);
    const char *c_cyan   = mw_color(use_color, MW_ANSI_CYAN);
    const char *c_green  = mw_color(use_color, MW_ANSI_GREEN);
    const char *c_yellow = mw_color(use_color, MW_ANSI_YELLOW);
    const char *c_red    = mw_color(use_color, MW_ANSI_RED);
    const char *c_reset  = mw_color(use_color, MW_ANSI_RESET);

    char total_str[32], used_str[32], avail_str[32], free_str[32], cache_str[32];
    mw_format_bytes(mem->mem_total_bytes, total_str, sizeof(total_str));
    mw_format_bytes(mem->used_bytes, used_str, sizeof(used_str));
    mw_format_bytes(mem->mem_available_bytes, avail_str, sizeof(avail_str));
    mw_format_bytes(mem->mem_free_bytes, free_str, sizeof(free_str));
    mw_format_bytes(mem->buffers_bytes + mem->cached_bytes + mem->sreclaimable_bytes,
                    cache_str, sizeof(cache_str));

    const char *util_color = c_green;
    if (mem->utilization_pct >= 85.0) {
        util_color = c_red;
    } else if (mem->utilization_pct >= 65.0) {
        util_color = c_yellow;
    }

    fprintf(out, "%s%sMEMWATCH — LINUX SYSTEM MEMORY OVERVIEW%s\n", c_bold, c_cyan, c_reset);
    fprintf(out, "  %-24s %12s\n", "Total Physical Memory:", total_str);
    fprintf(out, "  %-24s %12s  (%s%.2f%%%s utilized)\n",
            "Used Memory:", used_str, util_color, mem->utilization_pct, c_reset);
    fprintf(out, "  %-24s %12s  (free: %s, reclaimable cache/buffers: %s)\n",
            "Available Memory:", avail_str, free_str, cache_str);

    if (mem->swap_available) {
        char sw_total[32], sw_used[32], sw_free[32];
        mw_format_bytes(mem->swap_total_bytes, sw_total, sizeof(sw_total));
        mw_format_bytes(mem->swap_used_bytes, sw_used, sizeof(sw_used));
        mw_format_bytes(mem->swap_free_bytes, sw_free, sizeof(sw_free));
        fprintf(out, "  %-24s %12s / %s (%.2f%% used, %s free)\n",
                "Swap Usage:", sw_used, sw_total, mem->swap_utilization_pct, sw_free);
    } else {
        fprintf(out, "  %-24s %12s\n", "Swap Usage:", "Not configured (0 B)");
    }

    if (inaccessible_procs > 0) {
        fprintf(out, "  %-24s %12zu  (%zu accessible, %zu permission-restricted)\n",
                "Detected Processes:", mem->total_processes,
                mem->total_processes - inaccessible_procs, inaccessible_procs);
    } else {
        fprintf(out, "  %-24s %12zu\n", "Detected Processes:", mem->total_processes);
    }
}

static bool pid_is_flagged(pid_t pid, const mw_growth_alert_t *alerts, size_t alert_count) {
    if (alerts == NULL) return false;
    for (size_t i = 0; i < alert_count; i++) {
        if (alerts[i].suspected_growth && alerts[i].pid == pid) {
            return true;
        }
    }
    return false;
}

void mw_print_process_table(FILE *out,
                            const mw_proc_info_t *procs,
                            size_t count,
                            size_t max_rows,
                            const mw_growth_alert_t *alerts,
                            size_t alert_count,
                            bool show_delta,
                            bool use_color) {
    if (out == NULL || procs == NULL) {
        return;
    }

    const char *c_bold   = mw_color(use_color, MW_ANSI_BOLD);
    const char *c_yellow = mw_color(use_color, MW_ANSI_YELLOW);
    const char *c_red    = mw_color(use_color, MW_ANSI_RED);
    const char *c_green  = mw_color(use_color, MW_ANSI_GREEN);
    const char *c_dim    = mw_color(use_color, MW_ANSI_DIM);
    const char *c_reset  = mw_color(use_color, MW_ANSI_RESET);

    if (show_delta) {
        fprintf(out, "%s%-8s | %-22s | %-5s | %12s | %12s | %8s | %14s%s\n",
                c_bold, "PID", "PROCESS", "STATE", "RSS", "VSZ", "MEMORY %", "RSS CHANGE", c_reset);
        fprintf(out, "---------+------------------------+-------+--------------+--------------+----------+---------------\n");
    } else {
        fprintf(out, "%s%-8s | %-22s | %-5s | %12s | %12s | %8s%s\n",
                c_bold, "PID", "PROCESS", "STATE", "RSS", "VSZ", "MEMORY %", c_reset);
        fprintf(out, "---------+------------------------+-------+--------------+--------------+---------\n");
    }

    size_t limit = (max_rows > 0 && max_rows < count) ? max_rows : count;
    for (size_t i = 0; i < limit; i++) {
        const mw_proc_info_t *p = &procs[i];
        char rss_str[32], vsz_str[32], pct_str[24], delta_str[32];

        if (p->rss_valid) {
            mw_format_bytes(p->rss_bytes, rss_str, sizeof(rss_str));
            snprintf(pct_str, sizeof(pct_str), "%7.2f%%", p->mem_pct);
        } else {
            mw_strlcpy(rss_str, "N/A", sizeof(rss_str));
            mw_strlcpy(pct_str, "    N/A", sizeof(pct_str));
        }

        if (p->vsz_valid) {
            mw_format_bytes(p->vsz_bytes, vsz_str, sizeof(vsz_str));
        } else {
            mw_strlcpy(vsz_str, "N/A", sizeof(vsz_str));
        }

        bool flagged = pid_is_flagged(p->pid, alerts, alert_count);
        const char *row_prefix = flagged ? c_yellow : "";
        const char *row_suffix = flagged ? c_reset : "";

        if (show_delta) {
            const char *delta_color = "";
            const char *delta_end   = "";
            if (!p->rss_valid) {
                mw_strlcpy(delta_str, "N/A", sizeof(delta_str));
            } else if (!p->has_rss_delta) {
                mw_strlcpy(delta_str, "-- (initial)", sizeof(delta_str));
                delta_color = c_dim;
                delta_end   = c_reset;
            } else {
                mw_format_signed_bytes(p->rss_delta_bytes, delta_str, sizeof(delta_str));
                if (p->rss_delta_bytes > 0) {
                    delta_color = flagged ? c_red : c_yellow;
                    delta_end   = c_reset;
                } else if (p->rss_delta_bytes < 0) {
                    delta_color = c_green;
                    delta_end   = c_reset;
                }
            }

            fprintf(out, "%s%-8d | %-22.22s |   %c   | %12s | %12s | %8s | %s%14s%s%s\n",
                    row_prefix,
                    (int)p->pid,
                    p->name,
                    p->state ? p->state : '?',
                    rss_str,
                    vsz_str,
                    pct_str,
                    delta_color,
                    delta_str,
                    delta_end,
                    row_suffix);
        } else {
            fprintf(out, "%s%-8d | %-22.22s |   %c   | %12s | %12s | %8s%s\n",
                    row_prefix,
                    (int)p->pid,
                    p->name,
                    p->state ? p->state : '?',
                    rss_str,
                    vsz_str,
                    pct_str,
                    row_suffix);
        }
    }
}

void mw_print_growth_alerts(FILE *out,
                            const mw_growth_alert_t *alerts,
                            size_t alert_count,
                            bool use_color) {
    if (out == NULL || alerts == NULL || alert_count == 0) {
        return;
    }

    const char *c_bold   = mw_color(use_color, MW_ANSI_BOLD);
    const char *c_yellow = mw_color(use_color, MW_ANSI_YELLOW);
    const char *c_reset  = mw_color(use_color, MW_ANSI_RESET);

    fprintf(out, "%s%s[!] SUSPECTED MEMORY GROWTH ALERTS (%zu flagged)%s\n",
            c_bold, c_yellow, alert_count, c_reset);
    for (size_t i = 0; i < alert_count; i++) {
        const mw_growth_alert_t *a = &alerts[i];
        if (!a->suspected_growth) continue;

        char init_str[32], curr_str[32], diff_str[32];
        mw_format_bytes(a->initial_rss_bytes, init_str, sizeof(init_str));
        mw_format_bytes(a->current_rss_bytes, curr_str, sizeof(curr_str));
        mw_format_signed_bytes(a->abs_growth_bytes, diff_str, sizeof(diff_str));

        fprintf(out, "  * PID %d (%s): RSS %s -> %s (%s / +%.1f%% over %zu samples, %zu consecutive increases)\n",
                (int)a->pid, a->name, init_str, curr_str, diff_str,
                a->pct_growth, a->sample_count, a->consecutive_increases);
    }
    fprintf(out, "  Note: Flagged as suspected memory growth (not a confirmed leak). Legitimate caching,\n"
                 "        workload spikes, or delayed allocator/GC reclamation can also raise RSS.\n"
                 "        Use 'memwatch analyze -- ./program' for allocation-level leak verification.\n");
}

void mw_print_leak_report(FILE *out,
                          const mw_leak_report_t *report,
                          bool use_color) {
    if (out == NULL || report == NULL) {
        return;
    }

    const char *c_bold   = mw_color(use_color, MW_ANSI_BOLD);
    const char *c_cyan   = mw_color(use_color, MW_ANSI_CYAN);
    const char *c_red    = mw_color(use_color, MW_ANSI_RED);
    const char *c_yellow = mw_color(use_color, MW_ANSI_YELLOW);
    const char *c_green  = mw_color(use_color, MW_ANSI_GREEN);
    const char *c_reset  = mw_color(use_color, MW_ANSI_RESET);

    char def_str[48], ind_str[48], pos_str[48], reach_str[48], sup_str[48];
    mw_format_bytes_with_commas(report->definitely_lost_bytes, def_str, sizeof(def_str));
    mw_format_bytes_with_commas(report->indirectly_lost_bytes, ind_str, sizeof(ind_str));
    mw_format_bytes_with_commas(report->possibly_lost_bytes, pos_str, sizeof(pos_str));
    mw_format_bytes_with_commas(report->still_reachable_bytes, reach_str, sizeof(reach_str));
    mw_format_bytes_with_commas(report->suppressed_bytes, sup_str, sizeof(sup_str));

    fprintf(out, "%s%sMEMWATCH — MEMORY LEAK ANALYSIS%s\n\n", c_bold, c_cyan, c_reset);
    fprintf(out, "Target: %s\n",
            report->target_args[0] ? report->target_args :
            (report->target_exe[0] ? report->target_exe : "(from XML report)"));

    if (report->analyzed_pid > 0) {
        fprintf(out, "Analyzed PID: %d\n", (int)report->analyzed_pid);
    }
    if (report->child_timed_out) {
        fprintf(out, "Execution Status: %sTimed out%s\n", c_yellow, c_reset);
    } else if (report->child_term_signal > 0) {
        fprintf(out, "Execution Status: %sTerminated by signal %d%s\n",
                c_red, report->child_term_signal, c_reset);
    } else if (report->child_exited_normally && report->child_exit_code != 0) {
        fprintf(out, "Execution Status: %sExited with non-zero code %d (diagnostic report preserved)%s\n",
                c_yellow, report->child_exit_code, c_reset);
    }
    fprintf(out, "\n");

    const char *def_color = (report->definitely_lost_bytes > 0) ? c_red : c_green;
    const char *ind_color = (report->indirectly_lost_bytes > 0) ? c_yellow : c_green;
    const char *pos_color = (report->possibly_lost_bytes > 0) ? c_yellow : c_green;

    fprintf(out, "Definitely lost:   %s%16s%s  (%" PRIu64 " blocks)\n",
            def_color, def_str, c_reset, report->definitely_lost_blocks);
    fprintf(out, "Indirectly lost:   %s%16s%s  (%" PRIu64 " blocks)\n",
            ind_color, ind_str, c_reset, report->indirectly_lost_blocks);
    fprintf(out, "Possibly lost:     %s%16s%s  (%" PRIu64 " blocks)\n",
            pos_color, pos_str, c_reset, report->possibly_lost_blocks);
    fprintf(out, "Still reachable:   %16s  (%" PRIu64 " blocks)\n",
            reach_str, report->still_reachable_blocks);
    fprintf(out, "Suppressed:        %16s  (%" PRIu64 " blocks)\n",
            sup_str, report->suppressed_blocks);

    uint64_t total_unfreed = report->definitely_lost_bytes +
                             report->indirectly_lost_bytes +
                             report->possibly_lost_bytes +
                             report->still_reachable_bytes +
                             report->suppressed_bytes;
    char total_str[48];
    mw_format_bytes_with_commas(total_unfreed, total_str, sizeof(total_str));
    fprintf(out, "Total unfreed:     %16s  (aggregated without double-counting)\n\n", total_str);

    fprintf(out, "Allocation stack traces: %s\n",
            report->has_stack_traces ? "Available where reported" : "None reported (0 leak records)");
    if (report->has_stack_traces && !report->has_debug_info) {
        fprintf(out, "Debug symbols note:      Source file/line info missing in stack frames; compile target with -g for source-level detail.\n");
    } else if (report->has_debug_info) {
        fprintf(out, "Debug symbols note:      DWARF debug symbols detected (source file & line numbers resolved).\n");
    }

    if (report->record_count > 0) {
        fprintf(out, "\n%sDetailed Findings (%zu record(s)):%s\n", c_bold, report->record_count, c_reset);
        for (size_t i = 0; i < report->record_count; i++) {
            const mw_leak_record_t *rec = &report->records[i];
            fprintf(out, "\n  [%zu] %s%s%s (%s)\n",
                    i + 1, c_bold, mw_leak_kind_label(rec->kind), c_reset,
                    rec->raw_kind[0] ? rec->raw_kind : "Unknown");
            if (rec->what[0] != '\0') {
                fprintf(out, "      Summary: %s\n", rec->what);
            }
            if (rec->leaked_bytes > 0 || rec->leaked_blocks > 0) {
                char rec_bytes[48];
                mw_format_bytes_with_commas(rec->leaked_bytes, rec_bytes, sizeof(rec_bytes));
                fprintf(out, "      Leaked:  %s in %" PRIu64 " block(s)\n", rec_bytes, rec->leaked_blocks);
            }
            for (size_t f = 0; f < rec->frame_count; f++) {
                const mw_stack_frame_t *fr = &rec->frames[f];
                const char *fn_name = fr->fn[0] ? fr->fn : "???";
                if (fr->file[0] != '\0' && fr->line > 0) {
                    fprintf(out, "        #%zu %s at %s (%s:%d)\n",
                            f, fr->ip[0] ? fr->ip : "0x?", fn_name, fr->file, fr->line);
                } else if (fr->obj[0] != '\0') {
                    fprintf(out, "        #%zu %s in %s (%s)\n",
                            f, fr->ip[0] ? fr->ip : "0x?", fn_name, fr->obj);
                } else {
                    fprintf(out, "        #%zu %s in %s\n",
                            f, fr->ip[0] ? fr->ip : "0x?", fn_name);
                }
            }
        }
    }

    fprintf(out, "\nDetailed XML report: %s\n",
            report->xml_path[0] ? report->xml_path : "(in-memory buffer)");
    if (report->summary_report_path[0] != '\0') {
        fprintf(out, "MemWatch summary:    %s\n", report->summary_report_path);
    }
}

mw_status_t mw_save_text_report(const char *path, const mw_leak_report_t *report) {
    if (path == NULL || path[0] == '\0' || report == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        return MW_ERR_IO;
    }
    mw_print_leak_report(fp, report, false);
    if (fclose(fp) != 0) {
        return MW_ERR_IO;
    }
    return MW_OK;
}

typedef struct {
    char   path[MW_MAX_PATH];
    time_t mtime;
} report_file_entry_t;

static int compare_report_mtime_asc(const void *a, const void *b) {
    const report_file_entry_t *fa = (const report_file_entry_t *)a;
    const report_file_entry_t *fb = (const report_file_entry_t *)b;
    if (fa->mtime < fb->mtime) return -1;
    if (fa->mtime > fb->mtime) return 1;
    return strcmp(fa->path, fb->path);
}

mw_status_t mw_prune_old_reports(const char *output_dir, size_t max_files) {
    if (output_dir == NULL || max_files == 0) {
        return MW_ERR_INVALID_ARG;
    }
    DIR *dir = opendir(output_dir);
    if (dir == NULL) {
        return MW_OK;
    }

    size_t cap = 64;
    size_t count = 0;
    report_file_entry_t *files = calloc(cap, sizeof(report_file_entry_t));
    if (files == NULL) {
        closedir(dir);
        return MW_ERR_NOMEM;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strcmp(ent->d_name, "latest.xml") == 0 ||
            strcmp(ent->d_name, "latest_report.txt") == 0 ||
            strcmp(ent->d_name, "sample_leak_report.xml") == 0) {
            continue;
        }
        if (strncmp(ent->d_name, "valgrind_", 9) != 0 &&
            strncmp(ent->d_name, "memwatch_report_", 16) != 0) {
            continue;
        }

        char full[MW_MAX_PATH];
        int n = snprintf(full, sizeof(full), "%s/%s", output_dir, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full)) continue;

        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            if (count == cap) {
                size_t new_cap = cap * 2;
                report_file_entry_t *tmp = realloc(files, new_cap * sizeof(report_file_entry_t));
                if (tmp == NULL) break;
                files = tmp;
                cap = new_cap;
            }
            mw_strlcpy(files[count].path, full, sizeof(files[count].path));
            files[count].mtime = st.st_mtime;
            count++;
        }
    }
    closedir(dir);

    if (count > max_files) {
        qsort(files, count, sizeof(report_file_entry_t), compare_report_mtime_asc);
        size_t to_remove = count - max_files;
        for (size_t i = 0; i < to_remove; i++) {
            unlink(files[i].path);
        }
    }

    free(files);
    return MW_OK;
}
