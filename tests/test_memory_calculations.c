#define _POSIX_C_SOURCE 200809L

#include "memwatch.h"
#include "proc_reader.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_unit_conversions_and_overflow(void) {
    uint64_t bytes = 0;
    assert(mw_kb_to_bytes(0, &bytes) == MW_OK && bytes == 0);
    assert(mw_kb_to_bytes(1024, &bytes) == MW_OK && bytes == 1048576ULL);
    assert(mw_kb_to_bytes(UINT64_MAX / 1024ULL + 1ULL, &bytes) == MW_ERR_OVERFLOW);

    char buf[64];
    mw_format_bytes(512, buf, sizeof(buf));
    assert(strcmp(buf, "512 B") == 0);

    mw_format_bytes(2048, buf, sizeof(buf));
    assert(strcmp(buf, "2.0 KiB") == 0);

    mw_format_bytes(5ULL * 1024ULL * 1024ULL, buf, sizeof(buf));
    assert(strcmp(buf, "5.00 MiB") == 0);

    mw_format_bytes_with_commas(2048, buf, sizeof(buf));
    assert(strcmp(buf, "2,048 bytes") == 0);

    mw_format_bytes_with_commas(123456789ULL, buf, sizeof(buf));
    assert(strcmp(buf, "123,456,789 bytes") == 0);

    mw_format_signed_bytes(0, buf, sizeof(buf));
    assert(strcmp(buf, "0 B") == 0);

    mw_format_signed_bytes(4096, buf, sizeof(buf));
    assert(strcmp(buf, "+4.0 KiB") == 0);

    mw_format_signed_bytes(-2048, buf, sizeof(buf));
    assert(strcmp(buf, "-2.0 KiB") == 0);
}

static void test_percentage_calculations(void) {
    assert(fabs(mw_calc_percentage(0, 0) - 0.0) < 1e-6);
    assert(fabs(mw_calc_percentage(250, 1000) - 25.0) < 1e-6);
    assert(fabs(mw_calc_percentage(1024ULL * 1024ULL, 4ULL * 1024ULL * 1024ULL) - 25.0) < 1e-6);
}

static void test_process_ranking(void) {
    mw_proc_info_t procs[4];
    memset(procs, 0, sizeof(procs));

    /* Process 0: kernel thread with missing RSS */
    procs[0].pid = 2;
    mw_strlcpy(procs[0].name, "kthreadd", sizeof(procs[0].name));
    procs[0].rss_valid = false;
    procs[0].rss_bytes = 0;

    /* Process 1: 10 MiB RSS */
    procs[1].pid = 100;
    mw_strlcpy(procs[1].name, "worker_small", sizeof(procs[1].name));
    procs[1].rss_valid = true;
    procs[1].rss_bytes = 10ULL * 1024ULL * 1024ULL;

    /* Process 2: 500 MiB RSS */
    procs[2].pid = 200;
    mw_strlcpy(procs[2].name, "db_engine", sizeof(procs[2].name));
    procs[2].rss_valid = true;
    procs[2].rss_bytes = 500ULL * 1024ULL * 1024ULL;

    /* Process 3: 50 MiB RSS */
    procs[3].pid = 150;
    mw_strlcpy(procs[3].name, "web_server", sizeof(procs[3].name));
    procs[3].rss_valid = true;
    procs[3].rss_bytes = 50ULL * 1024ULL * 1024ULL;

    mw_sort_processes_by_rss(procs, 4);

    assert(procs[0].pid == 200);
    assert(procs[1].pid == 150);
    assert(procs[2].pid == 100);
    assert(procs[3].pid == 2); /* Missing RSS placed last */
}

int main(void) {
    test_unit_conversions_and_overflow();
    test_percentage_calculations();
    test_process_ranking();
    printf("[PASS] test_memory_calculations: all assertions passed.\n");
    return 0;
}
