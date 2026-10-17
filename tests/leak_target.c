#define _POSIX_C_SOURCE 200809L

/*
 * leak_target.c — Purpose-built standalone test target for MemWatch deep analysis.
 *
 * Modes:
 *   ./leak_target --clean           Allocates and frees all heap blocks cleanly (0 leaks).
 *   ./leak_target --leak            Intentionally leaks a 2,048-byte root block and a 512-byte
 *                                   indirect child block, plus a 4,096-byte still-reachable buffer.
 *   ./leak_target --leak-and-fail   Same intentional leak, then exits with code 3 to test
 *                                   diagnostic preservation on non-zero exit codes.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *child_payload;
    uint8_t  root_data[2048 - sizeof(uint8_t *)];
} leak_node_t;

static uint8_t *g_still_reachable_cache = NULL;

static uint8_t *create_child_payload(size_t size) {
    uint8_t *buf = malloc(size);
    if (buf != NULL) {
        memset(buf, 0xAB, size);
    }
    return buf;
}

static void create_intentional_leak(void) {
    /* 1. Still-reachable global allocation (4,096 bytes) */
    g_still_reachable_cache = calloc(1, 4096);
    if (g_still_reachable_cache != NULL) {
        g_still_reachable_cache[0] = 0x01;
    }

    /* 2. Definitely lost root node (2,048 bytes) + Indirectly lost child (512 bytes) */
    leak_node_t *node = malloc(sizeof(leak_node_t));
    if (node != NULL) {
        memset(node->root_data, 0xCD, sizeof(node->root_data));
        node->child_payload = create_child_payload(512);
        /* Intentionally lose pointer without freeing node or node->child_payload */
        volatile uintptr_t sink = (uintptr_t)node;
        (void)sink;
    }
}

static void run_leak_free_workload(void) {
    uint8_t *buf1 = malloc(2048);
    uint8_t *buf2 = calloc(512, 1);
    if (buf1 != NULL && buf2 != NULL) {
        memset(buf1, 0x11, 2048);
        memcpy(buf2, buf1, 512);
    }
    free(buf2);
    free(buf1);
}

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "--clean";

    if (strcmp(mode, "--leak") == 0) {
        create_intentional_leak();
        return 0;
    }
    if (strcmp(mode, "--leak-and-fail") == 0) {
        create_intentional_leak();
        return 3;
    }
    if (strcmp(mode, "--clean") == 0) {
        run_leak_free_workload();
        return 0;
    }

    fprintf(stderr, "Usage: %s [--clean | --leak | --leak-and-fail]\n", argv[0]);
    return 1;
}
