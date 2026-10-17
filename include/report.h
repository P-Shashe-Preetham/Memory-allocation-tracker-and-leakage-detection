#ifndef REPORT_H
#define REPORT_H

#include "memwatch.h"
#include "proc_reader.h"
#include "monitor.h"
#include "analyzer.h"

void mw_print_system_memory(FILE *out,
                            const mw_sys_memory_t *mem,
                            size_t inaccessible_procs,
                            bool use_color);

void mw_print_process_table(FILE *out,
                            const mw_proc_info_t *procs,
                            size_t count,
                            size_t max_rows,
                            const mw_growth_alert_t *alerts,
                            size_t alert_count,
                            bool show_delta,
                            bool use_color);

void mw_print_growth_alerts(FILE *out,
                            const mw_growth_alert_t *alerts,
                            size_t alert_count,
                            bool use_color);

void mw_print_leak_report(FILE *out,
                          const mw_leak_report_t *report,
                          bool use_color);

mw_status_t mw_save_text_report(const char *path,
                                const mw_leak_report_t *report);

mw_status_t mw_prune_old_reports(const char *output_dir, size_t max_files);

#endif /* REPORT_H */
