#define _POSIX_C_SOURCE 200809L

#include "analyzer.h"
#include "report.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_analyzer_interrupted = 0;

static void analyzer_sig_handler(int signo) {
    (void)signo;
    g_analyzer_interrupted = 1;
}

static mw_status_t copy_file_contents(const char *src_path, const char *dst_path) {
    if (src_path == NULL || dst_path == NULL || strcmp(src_path, dst_path) == 0) {
        return MW_OK;
    }
    FILE *in = fopen(src_path, "rb");
    if (in == NULL) {
        return MW_ERR_IO;
    }
    FILE *out = fopen(dst_path, "wb");
    if (out == NULL) {
        fclose(in);
        return MW_ERR_IO;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return MW_ERR_IO;
        }
    }
    fclose(in);
    fclose(out);
    return MW_OK;
}

mw_status_t mw_run_valgrind_target(const char *valgrind_bin,
                                   int target_argc,
                                   char **target_argv,
                                   const char *xml_output_path,
                                   int timeout_sec,
                                   mw_leak_report_t *out_report) {
    if (valgrind_bin == NULL || target_argc <= 0 || target_argv == NULL ||
        target_argv[0] == NULL || target_argv[0][0] == '\0' ||
        xml_output_path == NULL || out_report == NULL) {
        return MW_ERR_INVALID_ARG;
    }

    /* 1. Validate target executable */
    char resolved_target[MW_MAX_PATH];
    if (mw_find_executable(target_argv[0], resolved_target, sizeof(resolved_target)) != MW_OK) {
        return MW_ERR_NOT_FOUND;
    }

    /* 2. Verify that Valgrind is installed */
    char resolved_valgrind[MW_MAX_PATH];
    if (mw_find_executable(valgrind_bin, resolved_valgrind, sizeof(resolved_valgrind)) != MW_OK) {
        return MW_ERR_TOOL_MISSING;
    }

    /* Populate target metadata in report */
    mw_strlcpy(out_report->target_exe, target_argv[0], sizeof(out_report->target_exe));
    out_report->target_args[0] = '\0';
    for (int i = 0; i < target_argc; i++) {
        size_t len = strlen(out_report->target_args);
        if (len + strlen(target_argv[i]) + 2 < sizeof(out_report->target_args)) {
            if (i > 0) strcat(out_report->target_args, " ");
            strcat(out_report->target_args, target_argv[i]);
        }
    }
    mw_strlcpy(out_report->xml_path, xml_output_path, sizeof(out_report->xml_path));

    /*
     * 3. Build argv array for execvp() without shell interpolation.
     * Fixed Valgrind arguments (7) + "--" (1) + target_argc + NULL (1).
     */
    size_t vg_argc = 8 + (size_t)target_argc;
    char **vg_argv = calloc(vg_argc + 1, sizeof(char *));
    if (vg_argv == NULL) {
        return MW_ERR_NOMEM;
    }

    char xml_file_opt[MW_MAX_PATH + 32];
    snprintf(xml_file_opt, sizeof(xml_file_opt), "--xml-file=%s", xml_output_path);

    size_t idx = 0;
    vg_argv[idx++] = resolved_valgrind;
    vg_argv[idx++] = "--tool=memcheck";
    vg_argv[idx++] = "--leak-check=full";
    vg_argv[idx++] = "--show-leak-kinds=all";
    vg_argv[idx++] = "--track-origins=yes";
    vg_argv[idx++] = "--xml=yes";
    vg_argv[idx++] = xml_file_opt;
    vg_argv[idx++] = "--";
    for (int i = 0; i < target_argc; i++) {
        vg_argv[idx++] = target_argv[i];
    }
    vg_argv[idx] = NULL;

    /* Create a pipe with FD_CLOEXEC to detect immediate execvp() failure */
    int err_pipe[2] = {-1, -1};
    if (pipe(err_pipe) != 0) {
        free(vg_argv);
        return MW_ERR_IO;
    }
    fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC);

    g_analyzer_interrupted = 0;
    struct sigaction sa_new, sa_old_int, sa_old_term;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = analyzer_sig_handler;
    sigemptyset(&sa_new.sa_mask);
    sigaction(SIGINT, &sa_new, &sa_old_int);
    sigaction(SIGTERM, &sa_new, &sa_old_term);

    pid_t child_pid = fork();
    if (child_pid < 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        sigaction(SIGINT, &sa_old_int, NULL);
        sigaction(SIGTERM, &sa_old_term, NULL);
        free(vg_argv);
        return MW_ERR_EXEC;
    }

    if (child_pid == 0) {
        /* Child process */
        close(err_pipe[0]);
        execvp(resolved_valgrind, vg_argv);
        int exec_errno = errno;
        ssize_t unused = write(err_pipe[1], &exec_errno, sizeof(exec_errno));
        (void)unused;
        close(err_pipe[1]);
        _exit(127);
    }

    /* Parent process */
    close(err_pipe[1]);
    free(vg_argv);

    int child_exec_errno = 0;
    ssize_t nread = read(err_pipe[0], &child_exec_errno, sizeof(child_exec_errno));
    close(err_pipe[0]);
    if (nread > 0) {
        waitpid(child_pid, NULL, 0);
        sigaction(SIGINT, &sa_old_int, NULL);
        sigaction(SIGTERM, &sa_old_term, NULL);
        return MW_ERR_EXEC;
    }

    int status = 0;
    uint64_t start_ms = mw_monotonic_ms();
    bool timed_out = false;

    while (1) {
        pid_t w = waitpid(child_pid, &status, WNOHANG);
        if (w == child_pid) {
            break;
        }
        if (w < 0) {
            if (errno == EINTR) {
                if (g_analyzer_interrupted) {
                    kill(child_pid, SIGTERM);
                }
                continue;
            }
            break;
        }

        if (g_analyzer_interrupted) {
            kill(child_pid, SIGINT);
        }

        if (timeout_sec > 0) {
            uint64_t now_ms = mw_monotonic_ms();
            if ((now_ms - start_ms) >= (uint64_t)timeout_sec * 1000ULL) {
                timed_out = true;
                kill(child_pid, SIGTERM);
                struct timespec grace = {0, 200000000L}; /* 200ms grace */
                nanosleep(&grace, NULL);
                if (waitpid(child_pid, &status, WNOHANG) == 0) {
                    kill(child_pid, SIGKILL);
                    waitpid(child_pid, &status, 0);
                }
                break;
            }
        }

        struct timespec sleep_ts = {0, 50000000L}; /* 50ms poll */
        nanosleep(&sleep_ts, NULL);
    }

    sigaction(SIGINT, &sa_old_int, NULL);
    sigaction(SIGTERM, &sa_old_term, NULL);

    out_report->analyzed_pid = child_pid;
    out_report->child_timed_out = timed_out;
    if (WIFEXITED(status)) {
        out_report->child_exited_normally = true;
        out_report->child_exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        out_report->child_exited_normally = false;
        out_report->child_term_signal = WTERMSIG(status);
    }

    /* Check that the XML report was generated and is non-empty */
    struct stat st_xml;
    if (stat(xml_output_path, &st_xml) != 0 || st_xml.st_size == 0) {
        return timed_out ? MW_ERR_TIMEOUT : MW_ERR_PARSE;
    }

    /*
     * Parse the XML report even when the child exited with a non-zero exit code
     * so diagnostic reports are preserved on unsuccessful target exits.
     */
    mw_status_t parse_st = mw_parse_valgrind_xml_file(xml_output_path, out_report);
    if (parse_st != MW_OK) {
        return parse_st;
    }

    return timed_out ? MW_ERR_TIMEOUT : MW_OK;
}

mw_status_t mw_run_analyze(const mw_options_t *opts) {
    if (opts == NULL || opts->target_argc <= 0 || opts->target_argv == NULL) {
        fprintf(stderr, "Error: No target command specified for 'analyze'. Usage: memwatch analyze -- ./program [args]\n");
        return MW_ERR_INVALID_ARG;
    }

    const char *vg_bin = opts->valgrind_bin[0] ? opts->valgrind_bin : "valgrind";
    const char *env_vg = getenv("MEMWATCH_VALGRIND_BIN");
    if (env_vg != NULL && env_vg[0] != '\0' && strcmp(vg_bin, "valgrind") == 0) {
        vg_bin = env_vg;
    }

    /* Validate target binary upfront */
    char resolved_target[MW_MAX_PATH];
    if (mw_find_executable(opts->target_argv[0], resolved_target, sizeof(resolved_target)) != MW_OK) {
        fprintf(stderr, "Error: Target program '%s' not found or not executable.\n", opts->target_argv[0]);
        return MW_ERR_NOT_FOUND;
    }

    /* Verify Valgrind installation upfront */
    char resolved_vg[MW_MAX_PATH];
    if (mw_find_executable(vg_bin, resolved_vg, sizeof(resolved_vg)) != MW_OK) {
        fprintf(stderr,
                "Error: Valgrind executable ('%s') is not installed or not in PATH.\n"
                "Install it on Ubuntu / WSL2 with:\n"
                "  sudo apt update && sudo apt install valgrind\n",
                vg_bin);
        return MW_ERR_TOOL_MISSING;
    }

    /* Ensure output directory exists and bound stored reports */
    mw_status_t st = mw_mkdir_p(opts->output_dir);
    if (st != MW_OK) {
        fprintf(stderr, "Error: Could not create report directory '%s'.\n", opts->output_dir);
        return st;
    }
    mw_prune_old_reports(opts->output_dir, MW_MAX_REPORT_FILES);

    time_t now = time(NULL);
    pid_t self_pid = getpid();
    char xml_path[MW_MAX_PATH];
    char latest_xml[MW_MAX_PATH];
    char txt_path[MW_MAX_PATH];
    char latest_txt[MW_MAX_PATH];

    int n1 = snprintf(xml_path, sizeof(xml_path), "%.420s/valgrind_%ld_%d.xml",
                      opts->output_dir, (long)now, (int)self_pid);
    int n2 = snprintf(latest_xml, sizeof(latest_xml), "%.420s/latest.xml", opts->output_dir);
    int n3 = snprintf(txt_path, sizeof(txt_path), "%.420s/memwatch_report_%ld_%d.txt",
                      opts->output_dir, (long)now, (int)self_pid);
    int n4 = snprintf(latest_txt, sizeof(latest_txt), "%.420s/latest_report.txt", opts->output_dir);
    if (n1 < 0 || n2 < 0 || n3 < 0 || n4 < 0) {
        return MW_ERR_INVALID_ARG;
    }

    mw_leak_report_t report;
    mw_leak_report_init(&report);
    mw_strlcpy(report.summary_report_path, txt_path, sizeof(report.summary_report_path));

    st = mw_run_valgrind_target(resolved_vg,
                                opts->target_argc,
                                opts->target_argv,
                                xml_path,
                                opts->timeout_sec,
                                &report);

    if (st != MW_OK && st != MW_ERR_TIMEOUT) {
        fprintf(stderr, "Error during Valgrind analysis: %s\n", mw_status_str(st));
        mw_leak_report_free(&report);
        return st;
    }

    /* Save copies to latest.xml and text summary reports */
    copy_file_contents(xml_path, latest_xml);
    mw_save_text_report(txt_path, &report);
    copy_file_contents(txt_path, latest_txt);

    bool use_color = mw_should_use_color(stdout, opts->no_color);
    mw_print_leak_report(stdout, &report, use_color);

    mw_leak_report_free(&report);
    return st;
}

mw_status_t mw_run_report_cmd(const mw_options_t *opts) {
    if (opts == NULL || opts->report_path[0] == '\0') {
        fprintf(stderr, "Error: Missing XML report path. Usage: memwatch report <path/to/report.xml>\n");
        return MW_ERR_INVALID_ARG;
    }

    struct stat st_file;
    if (stat(opts->report_path, &st_file) != 0) {
        fprintf(stderr, "Error: Report file '%s' not found.\n", opts->report_path);
        return MW_ERR_NOT_FOUND;
    }

    mw_leak_report_t report;
    mw_leak_report_init(&report);

    mw_status_t st = mw_parse_valgrind_xml_file(opts->report_path, &report);
    if (st != MW_OK) {
        fprintf(stderr, "Error: Failed to parse Valgrind XML report '%s' (%s).\n",
                opts->report_path, mw_status_str(st));
        mw_leak_report_free(&report);
        return st;
    }

    bool use_color = mw_should_use_color(stdout, opts->no_color);
    mw_print_leak_report(stdout, &report, use_color);

    mw_leak_report_free(&report);
    return MW_OK;
}
