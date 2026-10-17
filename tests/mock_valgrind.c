#define _POSIX_C_SOURCE 200809L

/*
 * mock_valgrind.c — Deterministic Valgrind Memcheck test harness for unit tests
 * and container environments where the native valgrind package is not installed.
 *
 * Accepts standard Valgrind CLI flags (--tool=memcheck, --leak-check=full,
 * --show-leak-kinds=all, --xml=yes, --xml-file=<path>, -- <target> [args...]),
 * launches the target binary using fork()/execvp(), waits for completion, and
 * writes a Valgrind Protocol 4 XML report to <path>.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *xml_file = NULL;
    int target_idx = -1;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--xml-file=", 11) == 0) {
            xml_file = argv[i] + 11;
        } else if (strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc) {
                target_idx = i + 1;
            }
            break;
        } else if (argv[i][0] != '-') {
            target_idx = i;
            break;
        }
    }

    if (xml_file == NULL || target_idx < 0 || target_idx >= argc) {
        fprintf(stderr, "mock_valgrind: missing --xml-file or target executable\n");
        return 2;
    }

    bool leak_mode = false;
    for (int i = target_idx + 1; i < argc; i++) {
        if (strcmp(argv[i], "--leak") == 0 || strcmp(argv[i], "--leak-and-fail") == 0) {
            leak_mode = true;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        return 1;
    }
    if (pid == 0) {
        execvp(argv[target_idx], &argv[target_idx]);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    FILE *fp = fopen(xml_file, "w");
    if (fp == NULL) {
        return 1;
    }

    fprintf(fp, "<?xml version=\"1.0\"?>\n");
    fprintf(fp, "<valgrindoutput>\n");
    fprintf(fp, "  <protocolversion>4</protocolversion>\n");
    fprintf(fp, "  <protocoltool>memcheck</protocoltool>\n");
    fprintf(fp, "  <pid>%d</pid>\n", (int)pid);
    fprintf(fp, "  <args>\n");
    fprintf(fp, "    <argv>\n");
    fprintf(fp, "      <exe>%s</exe>\n", argv[target_idx]);
    for (int i = target_idx + 1; i < argc; i++) {
        fprintf(fp, "      <arg>%s</arg>\n", argv[i]);
    }
    fprintf(fp, "    </argv>\n");
    fprintf(fp, "  </args>\n");

    if (leak_mode) {
        fprintf(fp,
                "  <error>\n"
                "    <unique>0x10</unique>\n"
                "    <tid>1</tid>\n"
                "    <kind>Leak_DefinitelyLost</kind>\n"
                "    <xwhat>\n"
                "      <text>2,048 bytes in 1 blocks are definitely lost in loss record 2 of 3</text>\n"
                "      <leakedbytes>2048</leakedbytes>\n"
                "      <leakedblocks>1</leakedblocks>\n"
                "    </xwhat>\n"
                "    <stack>\n"
                "      <frame>\n"
                "        <ip>0x483B7F3</ip>\n"
                "        <obj>/usr/lib/valgrind/vgpreload_memcheck-amd64-linux.so</obj>\n"
                "        <fn>malloc</fn>\n"
                "      </frame>\n"
                "      <frame>\n"
                "        <ip>0x1091E2</ip>\n"
                "        <obj>%s</obj>\n"
                "        <fn>create_intentional_leak</fn>\n"
                "        <dir>tests</dir>\n"
                "        <file>leak_target.c</file>\n"
                "        <line>39</line>\n"
                "      </frame>\n"
                "      <frame>\n"
                "        <ip>0x10927A</ip>\n"
                "        <obj>%s</obj>\n"
                "        <fn>main</fn>\n"
                "        <dir>tests</dir>\n"
                "        <file>leak_target.c</file>\n"
                "        <line>61</line>\n"
                "      </frame>\n"
                "    </stack>\n"
                "  </error>\n"
                "  <error>\n"
                "    <unique>0x11</unique>\n"
                "    <tid>1</tid>\n"
                "    <kind>Leak_IndirectlyLost</kind>\n"
                "    <xwhat>\n"
                "      <text>512 bytes in 1 blocks are indirectly lost in loss record 1 of 3</text>\n"
                "      <leakedbytes>512</leakedbytes>\n"
                "      <leakedblocks>1</leakedblocks>\n"
                "    </xwhat>\n"
                "    <stack>\n"
                "      <frame>\n"
                "        <ip>0x483B7F3</ip>\n"
                "        <obj>/usr/lib/valgrind/vgpreload_memcheck-amd64-linux.so</obj>\n"
                "        <fn>malloc</fn>\n"
                "      </frame>\n"
                "      <frame>\n"
                "        <ip>0x109185</ip>\n"
                "        <obj>%s</obj>\n"
                "        <fn>create_child_payload</fn>\n"
                "        <dir>tests</dir>\n"
                "        <file>leak_target.c</file>\n"
                "        <line>25</line>\n"
                "      </frame>\n"
                "      <frame>\n"
                "        <ip>0x109204</ip>\n"
                "        <obj>%s</obj>\n"
                "        <fn>create_intentional_leak</fn>\n"
                "        <dir>tests</dir>\n"
                "        <file>leak_target.c</file>\n"
                "        <line>42</line>\n"
                "      </frame>\n"
                "    </stack>\n"
                "  </error>\n"
                "  <error>\n"
                "    <unique>0x12</unique>\n"
                "    <tid>1</tid>\n"
                "    <kind>Leak_StillReachable</kind>\n"
                "    <xwhat>\n"
                "      <text>4,096 bytes in 1 blocks are still reachable in loss record 3 of 3</text>\n"
                "      <leakedbytes>4096</leakedbytes>\n"
                "      <leakedblocks>1</leakedblocks>\n"
                "    </xwhat>\n"
                "    <stack>\n"
                "      <frame>\n"
                "        <ip>0x483DD99</ip>\n"
                "        <obj>/usr/lib/valgrind/vgpreload_memcheck-amd64-linux.so</obj>\n"
                "        <fn>calloc</fn>\n"
                "      </frame>\n"
                "      <frame>\n"
                "        <ip>0x1091B8</ip>\n"
                "        <obj>%s</obj>\n"
                "        <fn>create_intentional_leak</fn>\n"
                "        <dir>tests</dir>\n"
                "        <file>leak_target.c</file>\n"
                "        <line>33</line>\n"
                "      </frame>\n"
                "    </stack>\n"
                "  </error>\n",
                argv[target_idx], argv[target_idx], argv[target_idx], argv[target_idx], argv[target_idx]);
    }

    fprintf(fp, "</valgrindoutput>\n");
    fclose(fp);

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 1;
}
