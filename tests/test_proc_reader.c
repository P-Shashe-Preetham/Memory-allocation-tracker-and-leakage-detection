#define _POSIX_C_SOURCE 200809L

#include "proc_reader.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_parse_meminfo_standard(void) {
    const char *sample_meminfo =
        "MemTotal:        8000000 kB\n"
        "MemFree:         2000000 kB\n"
        "MemAvailable:    5000000 kB\n"
        "Buffers:          200000 kB\n"
        "Cached:          2500000 kB\n"
        "SwapTotal:       4000000 kB\n"
        "SwapFree:        3000000 kB\n"
        "SReclaimable:     300000 kB\n"
        "Shmem:            100000 kB\n";

    mw_sys_memory_t mem;
    mw_status_t st = mw_parse_meminfo_str(sample_meminfo, &mem);
    assert(st == MW_OK);
    assert(mem.mem_total_bytes == 8000000ULL * 1024ULL);
    assert(mem.mem_free_bytes == 2000000ULL * 1024ULL);
    assert(mem.mem_available_bytes == 5000000ULL * 1024ULL);
    assert(mem.used_bytes == 3000000ULL * 1024ULL);
    assert(fabs(mem.utilization_pct - 37.5) < 0.01);
    assert(mem.swap_available == true);
    assert(mem.swap_total_bytes == 4000000ULL * 1024ULL);
    assert(mem.swap_used_bytes == 1000000ULL * 1024ULL);
    assert(fabs(mem.swap_utilization_pct - 25.0) < 0.01);
}

static void test_parse_meminfo_fallback_no_available(void) {
    const char *legacy_meminfo =
        "MemTotal:        4000000 kB\n"
        "MemFree:         1000000 kB\n"
        "Buffers:          200000 kB\n"
        "Cached:           800000 kB\n"
        "SReclaimable:     100000 kB\n"
        "Shmem:            100000 kB\n"
        "SwapTotal:             0 kB\n"
        "SwapFree:              0 kB\n";

    mw_sys_memory_t mem;
    mw_status_t st = mw_parse_meminfo_str(legacy_meminfo, &mem);
    assert(st == MW_OK);
    assert(mem.has_mem_available == false);
    /* Reclaimable = 200000 + 800000 + 100000 - 100000 = 1000000 kB; Available = 2000000 kB */
    assert(mem.mem_available_bytes == 2000000ULL * 1024ULL);
    assert(mem.used_bytes == 2000000ULL * 1024ULL);
    assert(fabs(mem.utilization_pct - 50.0) < 0.01);
    assert(mem.swap_available == false);
}

static void test_parse_meminfo_malformed(void) {
    mw_sys_memory_t mem;
    assert(mw_parse_meminfo_str("", &mem) == MW_ERR_PARSE);
    assert(mw_parse_meminfo_str("MemTotal: invalid kB\nMemFree: 100 kB\n", &mem) == MW_ERR_PARSE);
    assert(mw_parse_meminfo_str("MemTotal: 0 kB\nMemFree: 0 kB\n", &mem) == MW_ERR_PARSE);
}

static void test_parse_proc_status_and_missing_rss(void) {
    const char *normal_status =
        "Name:\tnginx\n"
        "State:\tS (sleeping)\n"
        "Tgid:\t4321\n"
        "Pid:\t4321\n"
        "PPid:\t1\n"
        "Uid:\t33\t33\t33\t33\n"
        "VmSize:\t  120000 kB\n"
        "VmRSS:\t   45000 kB\n";

    mw_proc_info_t proc;
    memset(&proc, 0, sizeof(proc));
    mw_status_t st = mw_parse_proc_status_str(normal_status, &proc);
    assert(st == MW_OK);
    assert(strcmp(proc.name, "nginx") == 0);
    assert(proc.pid == 4321);
    assert(proc.ppid == 1);
    assert(proc.uid == 33);
    assert(proc.state == 'S');
    assert(proc.rss_valid == true);
    assert(proc.vsz_valid == true);
    assert(proc.rss_bytes == 45000ULL * 1024ULL);
    assert(proc.vsz_bytes == 120000ULL * 1024ULL);
    assert(proc.is_kernel_thread == false);

    /* Kernel thread without VmRSS / VmSize must NOT be treated as 0 bytes valid RSS */
    const char *kthread_status =
        "Name:\tkworker/u16:0\n"
        "State:\tI (idle)\n"
        "Pid:\t24\n"
        "PPid:\t2\n";

    mw_proc_info_t kproc;
    memset(&kproc, 0, sizeof(kproc));
    st = mw_parse_proc_status_str(kthread_status, &kproc);
    assert(st == MW_OK);
    assert(kproc.pid == 24);
    assert(kproc.rss_valid == false);
    assert(kproc.vsz_valid == false);
    assert(kproc.is_kernel_thread == true);

    /* Malformed VmRSS must return MW_ERR_PARSE */
    const char *bad_status =
        "Name:\tbroken\n"
        "Pid:\t99\n"
        "VmRSS:\tnot_a_number kB\n";
    mw_proc_info_t bproc;
    memset(&bproc, 0, sizeof(bproc));
    assert(mw_parse_proc_status_str(bad_status, &bproc) == MW_ERR_PARSE);
}

static void test_parse_proc_stat_with_parens_in_comm(void) {
    const char *stat_line =
        "1234 (my (weird) worker) R 1 1234 1234 0 -1 4194304 100 0 0 0 10 5 0 0 20 0 1 0 987654321 12345678 500 18446744073709551615\n";

    char state = '?';
    uint64_t starttime = 0;
    mw_status_t st = mw_parse_proc_stat_str(stat_line, &state, &starttime);
    assert(st == MW_OK);
    assert(state == 'R');
    assert(starttime == 987654321ULL);
}

static void test_synthetic_proc_root_and_invalid_pids(void) {
    char tmp_dir[] = "/tmp/mw_test_proc_XXXXXX";
    char *root = mkdtemp(tmp_dir);
    assert(root != NULL);

    char meminfo_path[MW_MAX_PATH];
    snprintf(meminfo_path, sizeof(meminfo_path), "%s/meminfo", root);
    FILE *fp = fopen(meminfo_path, "w");
    assert(fp != NULL);
    fprintf(fp, "MemTotal: 1000000 kB\nMemFree: 400000 kB\nMemAvailable: 600000 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
    fclose(fp);

    /* Create PID 101 */
    char pid101_dir[256];
    snprintf(pid101_dir, sizeof(pid101_dir), "%s/101", root);
    assert(mkdir(pid101_dir, 0755) == 0);

    char status101[MW_MAX_PATH];
    snprintf(status101, sizeof(status101), "%s/status", pid101_dir);
    fp = fopen(status101, "w");
    assert(fp != NULL);
    fprintf(fp, "Name:\tapp_alpha\nState:\tS (sleeping)\nPid:\t101\nPPid:\t1\nVmSize:\t50000 kB\nVmRSS:\t25000 kB\n");
    fclose(fp);

    char stat101[MW_MAX_PATH];
    snprintf(stat101, sizeof(stat101), "%s/stat", pid101_dir);
    fp = fopen(stat101, "w");
    assert(fp != NULL);
    fprintf(fp, "101 (app_alpha) S 1 101 101 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 55555 51200000 6250 0\n");
    fclose(fp);

    char smaps101[MW_MAX_PATH];
    snprintf(smaps101, sizeof(smaps101), "%s/smaps_rollup", pid101_dir);
    fp = fopen(smaps101, "w");
    assert(fp != NULL);
    fprintf(fp, "Pss:               22000 kB\nPrivate_Dirty:     18000 kB\n");
    fclose(fp);

    mw_sys_memory_t sys_mem;
    assert(mw_read_meminfo(root, &sys_mem) == MW_OK);

    mw_proc_info_t p101;
    assert(mw_read_process(root, 101, sys_mem.mem_total_bytes, &p101) == MW_OK);
    assert(p101.pid == 101);
    assert(strcmp(p101.name, "app_alpha") == 0);
    assert(p101.rss_valid == true);
    assert(p101.rss_bytes == 25000ULL * 1024ULL);
    assert(p101.pss_valid == true);
    assert(p101.pss_bytes == 22000ULL * 1024ULL);
    assert(p101.starttime_ticks == 55555ULL);
    assert(fabs(p101.mem_pct - 2.5) < 0.01);

    /* Disappearing / non-existent PID must return MW_ERR_NOT_FOUND */
    mw_proc_info_t missing;
    assert(mw_read_process(root, 99999, sys_mem.mem_total_bytes, &missing) == MW_ERR_NOT_FOUND);
    assert(mw_read_process(root, -5, sys_mem.mem_total_bytes, &missing) == MW_ERR_INVALID_ARG);

    /* Enumerate synthetic proc root */
    mw_proc_info_t *list = NULL;
    size_t count = 0;
    size_t inacc = 0;
    assert(mw_enum_processes(root, sys_mem.mem_total_bytes, &list, &count, &inacc) == MW_OK);
    assert(count == 1);
    assert(list[0].pid == 101);
    free(list);

    /* Cleanup synthetic files */
    unlink(smaps101);
    unlink(stat101);
    unlink(status101);
    rmdir(pid101_dir);
    unlink(meminfo_path);
    rmdir(root);
}

int main(void) {
    test_parse_meminfo_standard();
    test_parse_meminfo_fallback_no_available();
    test_parse_meminfo_malformed();
    test_parse_proc_status_and_missing_rss();
    test_parse_proc_stat_with_parens_in_comm();
    test_synthetic_proc_root_and_invalid_pids();
    printf("[PASS] test_proc_reader: all assertions passed.\n");
    return 0;
}
