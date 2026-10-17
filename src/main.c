#define _POSIX_C_SOURCE 200809L

#include "memwatch.h"
#include "proc_reader.h"
#include "monitor.h"
#include "analyzer.h"

#include <libxml/parser.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    mw_options_t opts;
    char err_buf[256];

    mw_status_t st = mw_parse_args(argc, argv, &opts, err_buf, sizeof(err_buf));
    if (st != MW_OK) {
        fprintf(stderr, "MemWatch CLI Error: %s\n", err_buf[0] ? err_buf : mw_status_str(st));
        fprintf(stderr, "Run '%s --help' for usage information.\n", argv[0] ? argv[0] : "memwatch");
        return EXIT_FAILURE;
    }

    switch (opts.command) {
        case MW_CMD_HELP:
            mw_print_help(stdout, argv[0]);
            st = MW_OK;
            break;
        case MW_CMD_VERSION:
            mw_print_version(stdout);
            st = MW_OK;
            break;
        case MW_CMD_SYSTEM:
            st = mw_run_system_overview(&opts);
            break;
        case MW_CMD_TOP:
            st = mw_run_top(&opts);
            break;
        case MW_CMD_MONITOR:
            st = mw_run_monitor(&opts);
            break;
        case MW_CMD_ANALYZE:
            st = mw_run_analyze(&opts);
            break;
        case MW_CMD_REPORT:
            st = mw_run_report_cmd(&opts);
            break;
        case MW_CMD_NONE:
        default:
            mw_print_help(stdout, argv[0]);
            st = MW_ERR_INVALID_ARG;
            break;
    }

    xmlCleanupParser();
    return (st == MW_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
