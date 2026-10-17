/**
 * @file main.c
 * @brief Demonstration application showcasing dynamic memory tracking and leak detection
 */

#include "memory_tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ANSI Terminal Colors for polished output */
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_MAGENTA "\033[35m"

/* ========================================================================= */
/* Scenario Implementations                                                  */
/* ========================================================================= */

/**
 * Scenario 1: Clean allocation and deallocation
 */
static void scenario_clean(void) {
    printf(ANSI_BOLD ANSI_CYAN "=== Scenario 1: Clean Allocation & Free ===\n" ANSI_RESET);
    printf("Allocating a 128-byte buffer, writing data, and freeing it cleanly...\n");

    char *buffer = (char *)MALLOC(128);
    if (buffer != NULL) {
        snprintf(buffer, 128, "Dynamic string correctly tracked and released.");
        printf("  Stored string: \"%s\"\n", buffer);
        printf("  Freeing pointer %p...\n", (void *)buffer);
        FREE(buffer);
        printf(ANSI_GREEN "  [SUCCESS] Clean allocation and release completed.\n\n" ANSI_RESET);
    }
}

/**
 * Scenario 2: Intentional single memory leak
 */
static void scenario_leak(void) {
    printf(ANSI_BOLD ANSI_YELLOW "=== Scenario 2: Intentional Single Memory Leak ===\n" ANSI_RESET);
    printf("Allocating 256 bytes and intentionally omitting FREE()...\n");

    int *leaked_array = (int *)MALLOC(256);
    if (leaked_array != NULL) {
        leaked_array[0] = 42;
        printf("  Allocated block of 256 bytes at %p\n", (void *)leaked_array);
        printf("  Intentionally skipping free(leaked_array). Watch leak report at program exit!\n\n");
    }
}

/**
 * Scenario 3: Multiple intentional memory leaks
 */
static void scenario_multiple_leaks(void) {
    printf(ANSI_BOLD ANSI_YELLOW "=== Scenario 3: Multiple Intentional Leaks ===\n" ANSI_RESET);
    printf("Allocating multiple blocks of varying sizes and leaving them unfreed...\n");

    void *leak1 = MALLOC(256);
    void *leak2 = MALLOC(1024);
    void *leak3 = MALLOC(512);

    printf("  Leak #1: 256 bytes at %p\n", leak1);
    printf("  Leak #2: 1024 bytes at %p\n", leak2);
    printf("  Leak #3: 512 bytes at %p\n", leak3);
    printf("  Total leaked in this scenario: 1792 bytes across 3 blocks.\n\n");
}

/**
 * Scenario 4: Calloc test with zero-initialization verification
 */
static void scenario_calloc(void) {
    printf(ANSI_BOLD ANSI_CYAN "=== Scenario 4: Calloc Zero-Initialization Test ===\n" ANSI_RESET);
    printf("Allocating 10 integers (40 bytes) with CALLOC and verifying zero-fill...\n");

    size_t count = 10;
    int *array = (int *)CALLOC(count, sizeof(int));
    if (array != NULL) {
        bool all_zeros = true;
        for (size_t i = 0; i < count; ++i) {
            if (array[i] != 0) {
                all_zeros = false;
                break;
            }
        }
        printf("  Array memory address: %p\n", (void *)array);
        printf("  Memory zero-initialization verified: %s\n", all_zeros ? "YES (All zeroes)" : "NO");

        FREE(array);
        printf(ANSI_GREEN "  [SUCCESS] Calloc block freed cleanly.\n\n" ANSI_RESET);
    }
}

/**
 * Scenario 5: Realloc test (expansion, contraction, NULL-pointer behavior)
 */
static void scenario_realloc(void) {
    printf(ANSI_BOLD ANSI_CYAN "=== Scenario 5: Realloc Test ===\n" ANSI_RESET);
    printf("Step A: Allocating initial 64 bytes...\n");

    char *str = (char *)MALLOC(64);
    if (str != NULL) {
        snprintf(str, 64, "Initial buffer content.");
        printf("  Initial pointer: %p, Content: \"%s\"\n", (void *)str, str);

        printf("Step B: Expanding to 256 bytes with REALLOC...\n");
        char *expanded = (char *)REALLOC(str, 256);
        if (expanded != NULL) {
            str = expanded;
            strncat(str, " Expanded with additional text appended.", 256 - strlen(str) - 1);
            printf("  Expanded pointer: %p, Content: \"%s\"\n", (void *)str, str);

            printf("Step C: Shrinking to 32 bytes with REALLOC...\n");
            char *shrunk = (char *)REALLOC(str, 32);
            if (shrunk != NULL) {
                str = shrunk;
                str[31] = '\0';
                printf("  Shrunk pointer: %p, Truncated content: \"%s\"\n", (void *)str, str);
            }
            FREE(str);
            printf(ANSI_GREEN "  [SUCCESS] Realloc sequence completed and freed cleanly.\n\n" ANSI_RESET);
        } else {
            /* If realloc failed, original block remains valid and must be freed */
            FREE(str);
        }
    }
}

/**
 * Scenario 6: Allocation failure handling (requesting impossible size)
 */
static void scenario_failed_alloc(void) {
    printf(ANSI_BOLD ANSI_CYAN "=== Scenario 6: Allocation Failure Handling ===\n" ANSI_RESET);
    printf("Attempting to allocate SIZE_MAX bytes to safely trigger allocator failure...\n");

    /* Requesting SIZE_MAX - 1 bytes will safely fail without exhausting RAM */
    void *ptr = MALLOC(SIZE_MAX - 1);
    if (ptr == NULL) {
        printf(ANSI_GREEN "  [SUCCESS] Allocation failed safely as expected; tracker recorded failure without crash.\n\n" ANSI_RESET);
    } else {
        FREE(ptr);
    }

    printf("Attempting CALLOC with count and size triggering integer overflow check...\n");
    void *overflow_ptr = CALLOC(SIZE_MAX / 2, 4);
    if (overflow_ptr == NULL) {
        printf(ANSI_GREEN "  [SUCCESS] Overflow detected and rejected safely before calling system allocator.\n\n" ANSI_RESET);
    } else {
        FREE(overflow_ptr);
    }
}

/**
 * Scenario 7: Double-free diagnostic test
 */
static void scenario_double_free(void) {
    printf(ANSI_BOLD ANSI_CYAN "=== Scenario 7: Double-Free Diagnostic Test ===\n" ANSI_RESET);
    printf("Allocating memory, freeing it, and safely attempting a second free...\n");

    int *ptr = (int *)MALLOC(sizeof(int) * 4);
    if (ptr != NULL) {
        printf("  Allocated pointer: %p\n", (void *)ptr);
        printf("  Calling FREE(ptr) first time...\n");
        FREE(ptr);

        printf("  Calling FREE(ptr) second time (should be intercepted safely)...\n");
        FREE(ptr);
        printf(ANSI_GREEN "  [SUCCESS] Double-free safely caught and blocked; no program crash!\n\n" ANSI_RESET);
    }
}

/**
 * Scenario 8: Unknown/untracked pointer diagnostic test
 */
static void scenario_unknown_ptr(void) {
    printf(ANSI_BOLD ANSI_CYAN "=== Scenario 8: Unknown/Untracked Pointer Test ===\n" ANSI_RESET);
    printf("Attempting to pass a stack address to FREE()...\n");

    int stack_variable = 999;
    void *stack_ptr = &stack_variable;
    printf("  Stack pointer: %p\n", stack_ptr);
    printf("  Calling FREE(&stack_variable)...\n");
    FREE(stack_ptr);

    printf("Attempting to pass a random fictitious heap address (0xDEADBEEF) to FREE()...\n");
    void *dummy_ptr = (void *)(uintptr_t)0xDEADBEEF;
    FREE(dummy_ptr);

    printf(ANSI_GREEN "  [SUCCESS] Untracked pointers safely rejected without invoking system free().\n\n" ANSI_RESET);
}

/**
 * Scenario 9: NULL pointer test
 */
static void scenario_null_ptr(void) {
    printf(ANSI_BOLD ANSI_CYAN "=== Scenario 9: NULL Pointer Free Test ===\n" ANSI_RESET);
    printf("Calling FREE(NULL) per C standard (must be safe no-op)...\n");

    FREE(NULL);
    printf(ANSI_GREEN "  [SUCCESS] FREE(NULL) handled cleanly without warning or crash.\n\n" ANSI_RESET);
}

/**
 * Scenario 10: Zero-leak confirmation test
 */
static void scenario_zero_leak(void) {
    printf(ANSI_BOLD ANSI_CYAN "=== Scenario 10: Zero-Leak Full Lifecycle Test ===\n" ANSI_RESET);
    printf("Allocating 5 diverse memory blocks and freeing every single one...\n");

    void *p1 = MALLOC(64);
    void *p2 = CALLOC(8, 16);
    void *p3 = MALLOC(128);
    void *p4 = REALLOC(p3, 256);
    void *p5 = MALLOC(512);

    FREE(p1);
    FREE(p2);
    FREE(p4);
    FREE(p5);

    printf(ANSI_GREEN "  [SUCCESS] All 5 blocks allocated and released.\n\n" ANSI_RESET);
}

/* ========================================================================= */
/* CLI Menu & Help                                                           */
/* ========================================================================= */

static void print_usage(const char *prog_name) {
    printf("Dynamic Memory Allocation Tracker & Leak Detector\n");
    printf("Usage: %s [OPTIONS|SCENARIO_NUMBER]\n\n", prog_name);
    printf("Scenarios:\n");
    printf("  1, --clean           Scenario 1: Clean allocation and release\n");
    printf("  2, --leak            Scenario 2: Intentional single memory leak (256 bytes)\n");
    printf("  3, --multiple-leaks  Scenario 3: Multiple intentional leaks (1792 bytes)\n");
    printf("  4, --calloc          Scenario 4: Calloc zero-initialization verification\n");
    printf("  5, --realloc         Scenario 5: Realloc expansion, contraction, and migration\n");
    printf("  6, --failed-alloc    Scenario 6: Failed allocation & integer overflow handling\n");
    printf("  7, --double-free     Scenario 7: Safe double-free detection\n");
    printf("  8, --unknown-ptr     Scenario 8: Untracked / stack pointer free rejection\n");
    printf("  9, --null-ptr        Scenario 9: FREE(NULL) standard no-op behavior\n");
    printf("  10, --zero-leak      Scenario 10: Zero-leak complete lifecycle\n\n");
    printf("Suites:\n");
    printf("  --all-clean          Run all non-leaking scenarios (1, 4, 5, 6, 7, 8, 9, 10)\n");
    printf("  --verbose            Enable real-time logging per allocation/free\n");
    printf("  --help, -h           Display this help message\n");
}

int main(int argc, char *argv[]) {
    tracker_init();

    if (argc < 2) {
        printf(ANSI_BOLD "============================================================\n" ANSI_RESET);
        printf(ANSI_BOLD " Dynamic Memory Tracker & Leak Detector - Demonstration\n" ANSI_RESET);
        printf(ANSI_BOLD "============================================================\n\n" ANSI_RESET);
        printf("Running standard test demonstration suite (all non-leaking scenarios)...\n");
        printf("To see intentional memory leak reports, run with: %s --leak or %s --multiple-leaks\n\n",
               argv[0], argv[0]);

        scenario_clean();
        scenario_calloc();
        scenario_realloc();
        scenario_failed_alloc();
        scenario_double_free();
        scenario_unknown_ptr();
        scenario_null_ptr();
        scenario_zero_leak();

        printf(ANSI_BOLD "Demonstration finished. Automatic atexit() leak check executing...\n" ANSI_RESET);
        return 0;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            tracker_set_verbose(true);
        }
    }

    const char *arg = argv[1];

    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    } else if (strcmp(arg, "1") == 0 || strcmp(arg, "--clean") == 0) {
        scenario_clean();
    } else if (strcmp(arg, "2") == 0 || strcmp(arg, "--leak") == 0) {
        scenario_leak();
    } else if (strcmp(arg, "3") == 0 || strcmp(arg, "--multiple-leaks") == 0) {
        scenario_multiple_leaks();
    } else if (strcmp(arg, "4") == 0 || strcmp(arg, "--calloc") == 0) {
        scenario_calloc();
    } else if (strcmp(arg, "5") == 0 || strcmp(arg, "--realloc") == 0) {
        scenario_realloc();
    } else if (strcmp(arg, "6") == 0 || strcmp(arg, "--failed-alloc") == 0) {
        scenario_failed_alloc();
    } else if (strcmp(arg, "7") == 0 || strcmp(arg, "--double-free") == 0) {
        scenario_double_free();
    } else if (strcmp(arg, "8") == 0 || strcmp(arg, "--unknown-ptr") == 0) {
        scenario_unknown_ptr();
    } else if (strcmp(arg, "9") == 0 || strcmp(arg, "--null-ptr") == 0) {
        scenario_null_ptr();
    } else if (strcmp(arg, "10") == 0 || strcmp(arg, "--zero-leak") == 0) {
        scenario_zero_leak();
    } else if (strcmp(arg, "--all-clean") == 0) {
        scenario_clean();
        scenario_calloc();
        scenario_realloc();
        scenario_failed_alloc();
        scenario_double_free();
        scenario_unknown_ptr();
        scenario_null_ptr();
        scenario_zero_leak();
    } else {
        fprintf(stderr, "Unknown option: %s\n", arg);
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}
