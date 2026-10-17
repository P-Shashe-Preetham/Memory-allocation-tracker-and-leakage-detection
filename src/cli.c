#define _POSIX_C_SOURCE 200809L

#include "memwatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mw_options_init_defaults(mw_options_t *opts) {
    if (opts == NULL) {
        return;
    }
    memset(opts, 0, sizeof(*opts));
    opts->command                = MW_CMD_NONE;
    opts->target_pid             = 0;
    opts->interval_sec           = 1;
    opts->duration_sec           = 0;
    opts->top_n                  = 10;
    opts->min_samples            = 4;
    opts->growth_threshold_bytes = 64ULL * 1024ULL; /* 64 KiB */
    opts->growth_threshold_pct   = 5.0;             /* 5.0% */
    opts->min_sustained_sec      = 0;
    opts->timeout_sec            = 0;
    opts->no_color               = false;
    mw_strlcpy(opts->proc_root, "/proc", sizeof(opts->proc_root));
    mw_strlcpy(opts->output_dir, "reports", sizeof(opts->output_dir));
    mw_strlcpy(opts->valgrind_bin, "valgrind", sizeof(opts->valgrind_bin));
}

void mw_print_version(FILE *out) {
    if (out == NULL) return;
    fprintf(out, "MemWatch v%s — Linux Memory Monitor and Leak Detector in C\n", MEMWATCH_VERSION);
}

void mw_print_help(FILE *out, const char *prog_name) {
    if (out == NULL) return;
    const char *prog = (prog_name && prog_name[0]) ? prog_name : "memwatch";

    fprintf(out,
            "MemWatch v%s — Linux System Memory Monitor & Allocation Leak Detector\n\n"
            "USAGE:\n"
            "  %s system [options]\n"
            "  %s monitor [options]\n"
            "  %s top [options]\n"
            "  %s analyze [options] -- <program> [arg1 arg2 ...]\n"
            "  %s report <path/to/report.xml> [options]\n"
            "  %s --help | --version\n\n"
            "COMMANDS:\n"
            "  system               Display system physical memory, swap, and top consumers\n"
            "  monitor              Continuously monitor processes and detect sustained RSS growth\n"
            "  top                  Display top processes ranked by resident set size (RSS)\n"
            "  analyze              Launch a target binary under Valgrind Memcheck and parse XML output\n"
            "  report <xml_file>    Parse and summarize an existing Valgrind XML report\n\n"
            "OPTIONS:\n"
            "  --pid <PID>              Monitor a specific process ID (default: all processes)\n"
            "  --interval <SEC>         Sampling interval in seconds for monitor (default: 1)\n"
            "  --duration <SEC>         Maximum monitoring duration in seconds (default: 0 = until Ctrl+C)\n"
            "  --top, -n <COUNT>        Number of top processes to display (default: 10)\n"
            "  --min-samples <COUNT>    Minimum consecutive samples for growth alert (default: 4)\n"
            "  --threshold-kb <KiB>     Minimum absolute RSS growth threshold in KiB (default: 64)\n"
            "  --threshold-bytes <B>    Minimum absolute RSS growth threshold in bytes (default: 65536)\n"
            "  --threshold-pct <PCT>    Minimum percentage RSS growth threshold (default: 5.0)\n"
            "  --min-duration <SEC>     Minimum sustained duration in seconds for growth alert (default: 0)\n"
            "  --output-dir <DIR>       Output directory for Valgrind XML & MemWatch reports (default: reports)\n"
            "  --timeout <SEC>          Timeout in seconds for analyzed child process (default: 0 = none)\n"
            "  --valgrind-bin <PATH>    Path or name of Valgrind binary (default: valgrind)\n"
            "  --proc-root <PATH>       Alternate procfs mount path (default: /proc)\n"
            "  --no-color               Disable ANSI terminal color output\n"
            "  -h, --help               Show this help message and exit\n"
            "  -v, --version            Show version information and exit\n\n"
            "EXAMPLES:\n"
            "  %s system\n"
            "  %s top --top 15\n"
            "  %s monitor --pid 1234 --interval 1 --duration 60\n"
            "  %s analyze -- ./my_program --flag value\n"
            "  %s report reports/latest.xml\n",
            MEMWATCH_VERSION, prog, prog, prog, prog, prog, prog,
            prog, prog, prog, prog, prog);
}

mw_status_t mw_parse_args(int argc, char **argv, mw_options_t *opts, char *err_buf, size_t err_buf_len) {
    if (opts == NULL || argc < 1 || argv == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    if (err_buf != NULL && err_buf_len > 0) {
        err_buf[0] = '\0';
    }
    mw_options_init_defaults(opts);

    if (argc == 1) {
        opts->command = MW_CMD_HELP;
        return MW_OK;
    }

    int i = 1;
    const char *first = argv[i];
    if (strcmp(first, "-h") == 0 || strcmp(first, "--help") == 0 || strcmp(first, "help") == 0) {
        opts->command = MW_CMD_HELP;
        return MW_OK;
    }
    if (strcmp(first, "-v") == 0 || strcmp(first, "--version") == 0 || strcmp(first, "version") == 0) {
        opts->command = MW_CMD_VERSION;
        return MW_OK;
    }

    if (strcmp(first, "system") == 0) {
        opts->command = MW_CMD_SYSTEM;
        i++;
    } else if (strcmp(first, "monitor") == 0) {
        opts->command = MW_CMD_MONITOR;
        i++;
    } else if (strcmp(first, "top") == 0) {
        opts->command = MW_CMD_TOP;
        opts->top_n = 15;
        i++;
    } else if (strcmp(first, "analyze") == 0) {
        opts->command = MW_CMD_ANALYZE;
        i++;
    } else if (strcmp(first, "report") == 0) {
        opts->command = MW_CMD_REPORT;
        i++;
    } else {
        if (err_buf) {
            snprintf(err_buf, err_buf_len, "Unknown command '%s'. Run 'memwatch --help' for usage.", first);
        }
        return MW_ERR_INVALID_ARG;
    }

    while (i < argc) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) {
            i++;
            opts->target_argc = argc - i;
            opts->target_argv = (opts->target_argc > 0) ? &argv[i] : NULL;
            break;
        }

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            opts->command = MW_CMD_HELP;
            return MW_OK;
        }
        if (strcmp(arg, "--no-color") == 0) {
            opts->no_color = true;
            i++;
            continue;
        }
        if (strcmp(arg, "--pid") == 0) {
            if (i + 1 >= argc) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '--pid' requires a numeric PID argument.");
                return MW_ERR_INVALID_ARG;
            }
            int pid_val = 0;
            if (mw_parse_i32(argv[++i], &pid_val) != MW_OK || pid_val <= 0) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Invalid PID '%s': must be a positive integer.", argv[i]);
                return MW_ERR_INVALID_ARG;
            }
            opts->target_pid = (pid_t)pid_val;
            i++;
            continue;
        }
        if (strcmp(arg, "--interval") == 0) {
            if (i + 1 >= argc) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '--interval' requires a positive integer (seconds).");
                return MW_ERR_INVALID_ARG;
            }
            int val = 0;
            if (mw_parse_i32(argv[++i], &val) != MW_OK || val <= 0 || val > 3600) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Invalid interval '%s': must be 1..3600 seconds.", argv[i]);
                return MW_ERR_INVALID_ARG;
            }
            opts->interval_sec = val;
            i++;
            continue;
        }
        if (strcmp(arg, "--duration") == 0) {
            if (i + 1 >= argc) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '--duration' requires a non-negative integer (seconds).");
                return MW_ERR_INVALID_ARG;
            }
            int val = 0;
            if (mw_parse_i32(argv[++i], &val) != MW_OK || val < 0) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Invalid duration '%s': must be >= 0 seconds.", argv[i]);
                return MW_ERR_INVALID_ARG;
            }
            opts->duration_sec = val;
            i++;
            continue;
        }
        if (strcmp(arg, "--top") == 0 || strcmp(arg, "-n") == 0 || strcmp(arg, "--limit") == 0) {
            if (i + 1 >= argc) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '%s' requires a positive count.", arg);
                return MW_ERR_INVALID_ARG;
            }
            int val = 0;
            if (mw_parse_i32(argv[++i], &val) != MW_OK || val <= 0) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Invalid count '%s': must be a positive integer.", argv[i]);
                return MW_ERR_INVALID_ARG;
            }
            opts->top_n = (size_t)val;
            i++;
            continue;
        }
        if (strcmp(arg, "--min-samples") == 0) {
            if (i + 1 >= argc) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '--min-samples' requires an integer >= 2.");
                return MW_ERR_INVALID_ARG;
            }
            int val = 0;
            if (mw_parse_i32(argv[++i], &val) != MW_OK || val < 2 || val > MW_HISTORY_CAPACITY) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Invalid --min-samples '%s': must be between 2 and %d.", argv[i], MW_HISTORY_CAPACITY);
                return MW_ERR_INVALID_ARG;
            }
            opts->min_samples = (size_t)val;
            i++;
            continue;
        }
        if (strcmp(arg, "--threshold-kb") == 0) {
            if (i + 1 >= argc) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '--threshold-kb' requires a value in KiB.");
                return MW_ERR_INVALID_ARG;
            }
            uint64_t kb = 0;
            if (mw_parse_u64(argv[++i], &kb) != MW_OK || mw_kb_to_bytes(kb, &opts->growth_threshold_bytes) != MW_OK) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Invalid --threshold-kb '%s'.", argv[i]);
                return MW_ERR_INVALID_ARG;
            }
            i++;
            continue;
        }
        if (strcmp(arg, "--threshold-bytes") == 0) {
            if (i + 1 >= argc) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '--threshold-bytes' requires a byte count.");
                return MW_ERR_INVALID_ARG;
            }
            if (mw_parse_u64(argv[++i], &opts->growth_threshold_bytes) != MW_OK) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Invalid --threshold-bytes '%s'.", argv[i]);
                return MW_ERR_INVALID_ARG;
            }
            i++;
            continue;
        }
        if (strcmp(arg, "--threshold-pct") == 0) {
            if (i + 1 >= argc) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '--threshold-pct' requires a percentage.");
                return MW_ERR_INVALID_ARG;
            }
            double pct = 0.0;
            if (mw_parse_double(argv[++i], &pct) != MW_OK || pct < 0.0) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Invalid --threshold-pct '%s'.", argv[i]);
                return MW_ERR_INVALID_ARG;
            }
            opts->growth_threshold_pct = pct;
            i++;
            continue;
        }
        if (strcmp(arg, "--min-duration") == 0) {
            if (i + 1 >= argc) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '--min-duration' requires seconds >= 0.");
                return MW_ERR_INVALID_ARG;
            }
            int val = 0;
            if (mw_parse_i32(argv[++i], &val) != MW_OK || val < 0) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Invalid --min-duration '%s'.", argv[i]);
                return MW_ERR_INVALID_ARG;
            }
            opts->min_sustained_sec = val;
            i++;
            continue;
        }
        if (strcmp(arg, "--timeout") == 0) {
            if (i + 1 >= argc) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '--timeout' requires seconds >= 0.");
                return MW_ERR_INVALID_ARG;
            }
            int val = 0;
            if (mw_parse_i32(argv[++i], &val) != MW_OK || val < 0) {
                if (err_buf) snprintf(err_buf, err_buf_len, "Invalid --timeout '%s'.", argv[i]);
                return MW_ERR_INVALID_ARG;
            }
            opts->timeout_sec = val;
            i++;
            continue;
        }
        if (strcmp(arg, "--output-dir") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '--output-dir' requires a directory path.");
                return MW_ERR_INVALID_ARG;
            }
            mw_strlcpy(opts->output_dir, argv[++i], sizeof(opts->output_dir));
            i++;
            continue;
        }
        if (strcmp(arg, "--valgrind-bin") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '--valgrind-bin' requires a binary path.");
                return MW_ERR_INVALID_ARG;
            }
            mw_strlcpy(opts->valgrind_bin, argv[++i], sizeof(opts->valgrind_bin));
            i++;
            continue;
        }
        if (strcmp(arg, "--proc-root") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                if (err_buf) snprintf(err_buf, err_buf_len, "Option '--proc-root' requires a directory path.");
                return MW_ERR_INVALID_ARG;
            }
            mw_strlcpy(opts->proc_root, argv[++i], sizeof(opts->proc_root));
            i++;
            continue;
        }

        if (opts->command == MW_CMD_REPORT && arg[0] != '-' && opts->report_path[0] == '\0') {
            mw_strlcpy(opts->report_path, arg, sizeof(opts->report_path));
            i++;
            continue;
        }

        if (err_buf) {
            snprintf(err_buf, err_buf_len, "Unrecognized option or argument '%s'.", arg);
        }
        return MW_ERR_INVALID_ARG;
    }

    if (opts->command == MW_CMD_ANALYZE && (opts->target_argc <= 0 || opts->target_argv == NULL)) {
        if (err_buf) {
            snprintf(err_buf, err_buf_len,
                     "Command 'analyze' requires '-- <program> [args...]'. Example: memwatch analyze -- ./program");
        }
        return MW_ERR_INVALID_ARG;
    }

    if (opts->command == MW_CMD_REPORT && opts->report_path[0] == '\0') {
        if (err_buf) {
            snprintf(err_buf, err_buf_len,
                     "Command 'report' requires an XML file path. Example: memwatch report reports/latest.xml");
        }
        return MW_ERR_INVALID_ARG;
    }

    return MW_OK;
}
