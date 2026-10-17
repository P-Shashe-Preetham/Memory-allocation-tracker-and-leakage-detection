#define _POSIX_C_SOURCE 200809L

#include "analyzer.h"
#include "report.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_valid_valgrind_xml_parsing(void) {
    const char *sample_xml =
        "<?xml version=\"1.0\"?>\n"
        "<valgrindoutput>\n"
        "  <protocolversion>4</protocolversion>\n"
        "  <protocoltool>memcheck</protocoltool>\n"
        "  <pid>4242</pid>\n"
        "  <args>\n"
        "    <vargv><exe>/usr/bin/valgrind</exe></vargv>\n"
        "    <argv><exe>./tests/leak_target</exe><arg>--leak</arg></argv>\n"
        "  </args>\n"
        "  <error>\n"
        "    <unique>0x1</unique>\n"
        "    <tid>1</tid>\n"
        "    <kind>Leak_DefinitelyLost</kind>\n"
        "    <xwhat>\n"
        "      <text>2,048 bytes in 2 blocks are definitely lost in loss record 1 of 3</text>\n"
        "      <leakedbytes>2048</leakedbytes>\n"
        "      <leakedblocks>2</leakedblocks>\n"
        "    </xwhat>\n"
        "    <stack>\n"
        "      <frame>\n"
        "        <ip>0x483B7F3</ip>\n"
        "        <obj>/usr/lib/valgrind/vgpreload_memcheck.so</obj>\n"
        "        <fn>malloc</fn>\n"
        "      </frame>\n"
        "      <frame>\n"
        "        <ip>0x109245</ip>\n"
        "        <obj>/app/tests/leak_target</obj>\n"
        "        <fn>create_intentional_leak</fn>\n"
        "        <dir>/app/tests</dir>\n"
        "        <file>leak_target.c</file>\n"
        "        <line>42</line>\n"
        "      </frame>\n"
        "    </stack>\n"
        "  </error>\n"
        "  <error>\n"
        "    <unique>0x2</unique>\n"
        "    <tid>1</tid>\n"
        "    <kind>Leak_IndirectlyLost</kind>\n"
        "    <xwhat>\n"
        "      <text>512 bytes in 1 blocks are indirectly lost in loss record 2 of 3</text>\n"
        "      <leakedbytes>512</leakedbytes>\n"
        "      <leakedblocks>1</leakedblocks>\n"
        "    </xwhat>\n"
        "    <stack>\n"
        "      <frame>\n"
        "        <ip>0x483B7F3</ip>\n"
        "        <fn>malloc</fn>\n"
        "      </frame>\n"
        "      <frame>\n"
        "        <ip>0x109260</ip>\n"
        "        <fn>create_child_payload</fn>\n"
        "        <file>leak_target.c</file>\n"
        "        <line>28</line>\n"
        "      </frame>\n"
        "    </stack>\n"
        "  </error>\n"
        "  <error>\n"
        "    <unique>0x3</unique>\n"
        "    <tid>1</tid>\n"
        "    <kind>Leak_StillReachable</kind>\n"
        "    <xwhat>\n"
        "      <text>8,192 bytes in 1 blocks are still reachable in loss record 3 of 3</text>\n"
        "      <leakedbytes>8192</leakedbytes>\n"
        "      <leakedblocks>1</leakedblocks>\n"
        "    </xwhat>\n"
        "    <stack>\n"
        "      <frame>\n"
        "        <ip>0x483B7F3</ip>\n"
        "        <fn>calloc</fn>\n"
        "        <file>leak_target.c</file>\n"
        "        <line>55</line>\n"
        "      </frame>\n"
        "    </stack>\n"
        "  </error>\n"
        "</valgrindoutput>\n";

    mw_leak_report_t report;
    mw_leak_report_init(&report);

    mw_status_t st = mw_parse_valgrind_xml_memory(sample_xml, strlen(sample_xml), &report);
    assert(st == MW_OK);
    assert(report.analyzed_pid == 4242);
    assert(strcmp(report.target_exe, "./tests/leak_target") == 0);
    assert(strcmp(report.target_args, "./tests/leak_target --leak") == 0);
    assert(report.definitely_lost_bytes == 2048);
    assert(report.definitely_lost_blocks == 2);
    assert(report.indirectly_lost_bytes == 512);
    assert(report.indirectly_lost_blocks == 1);
    assert(report.possibly_lost_bytes == 0);
    assert(report.still_reachable_bytes == 8192);
    assert(report.still_reachable_blocks == 1);
    assert(report.suppressed_bytes == 0);
    assert(report.has_stack_traces == true);
    assert(report.has_debug_info == true);
    assert(report.record_count == 3);
    assert(strcmp(report.records[0].frames[1].fn, "create_intentional_leak") == 0);
    assert(strcmp(report.records[0].frames[1].file, "leak_target.c") == 0);
    assert(report.records[0].frames[1].line == 42);

    /* Test saving report file */
    const char *tmp_report_path = "/tmp/mw_test_saved_report.txt";
    assert(mw_save_text_report(tmp_report_path, &report) == MW_OK);
    FILE *fp = fopen(tmp_report_path, "r");
    assert(fp != NULL);
    char content[2048];
    size_t n = fread(content, 1, sizeof(content) - 1, fp);
    content[n] = '\0';
    fclose(fp);
    unlink(tmp_report_path);

    assert(strstr(content, "MEMWATCH — MEMORY LEAK ANALYSIS") != NULL);
    assert(strstr(content, "2,048 bytes") != NULL);
    assert(strstr(content, "512 bytes") != NULL);
    assert(strstr(content, "8,192 bytes") != NULL);
    assert(strstr(content, "create_intentional_leak") != NULL);

    mw_leak_report_free(&report);
}

static void test_malformed_and_incomplete_xml(void) {
    mw_leak_report_t report;
    mw_leak_report_init(&report);

    /* Incomplete XML */
    const char *truncated = "<valgrindoutput><protocolversion>4</protocolversion><error>";
    assert(mw_parse_valgrind_xml_memory(truncated, strlen(truncated), &report) == MW_ERR_PARSE);
    mw_leak_report_free(&report);

    /* Wrong root element */
    mw_leak_report_init(&report);
    const char *wrong_root = "<otherroot><protocolversion>4</protocolversion></otherroot>";
    assert(mw_parse_valgrind_xml_memory(wrong_root, strlen(wrong_root), &report) == MW_ERR_PARSE);
    mw_leak_report_free(&report);

    /* Missing protocolversion */
    mw_leak_report_init(&report);
    const char *no_proto = "<valgrindoutput><pid>10</pid></valgrindoutput>";
    assert(mw_parse_valgrind_xml_memory(no_proto, strlen(no_proto), &report) == MW_ERR_PARSE);
    mw_leak_report_free(&report);
}

static void test_missing_diagnostic_tool(void) {
    mw_leak_report_t report;
    mw_leak_report_init(&report);
    char *target_argv[] = {"/bin/true", NULL};

    mw_status_t st = mw_run_valgrind_target("/nonexistent/valgrind_binary_xyz",
                                            1,
                                            target_argv,
                                            "/tmp/mw_should_not_exist.xml",
                                            5,
                                            &report);
    assert(st == MW_ERR_TOOL_MISSING);
    mw_leak_report_free(&report);
}

static void test_end_to_end_fork_exec_analyzer(void) {
    /*
     * Exercises mw_run_valgrind_target's fork()/execvp() pipeline using
     * ./tests/mock_valgrind and ./tests/leak_target when built.
     */
    if (access("./tests/mock_valgrind", X_OK) != 0 || access("./tests/leak_target", X_OK) != 0) {
        return;
    }

    const char *xml_out = "/tmp/mw_e2e_valgrind_out.xml";
    unlink(xml_out);

    /* 1. Leak path */
    mw_leak_report_t leak_rep;
    mw_leak_report_init(&leak_rep);
    char *leak_argv[] = {"./tests/leak_target", "--leak", NULL};
    mw_status_t st = mw_run_valgrind_target("./tests/mock_valgrind", 2, leak_argv, xml_out, 5, &leak_rep);
    assert(st == MW_OK);
    assert(leak_rep.definitely_lost_bytes == 2048);
    assert(leak_rep.indirectly_lost_bytes == 512);
    assert(leak_rep.still_reachable_bytes == 4096);
    mw_leak_report_free(&leak_rep);
    unlink(xml_out);

    /* 2. Clean path */
    mw_leak_report_t clean_rep;
    mw_leak_report_init(&clean_rep);
    char *clean_argv[] = {"./tests/leak_target", "--clean", NULL};
    st = mw_run_valgrind_target("./tests/mock_valgrind", 2, clean_argv, xml_out, 5, &clean_rep);
    assert(st == MW_OK);
    assert(clean_rep.definitely_lost_bytes == 0);
    assert(clean_rep.indirectly_lost_bytes == 0);
    mw_leak_report_free(&clean_rep);
    unlink(xml_out);
}

int main(void) {
    test_valid_valgrind_xml_parsing();
    test_malformed_and_incomplete_xml();
    test_missing_diagnostic_tool();
    test_end_to_end_fork_exec_analyzer();
    printf("[PASS] test_report_parser: all assertions passed.\n");
    return 0;
}
