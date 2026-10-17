/**
 * @file test_memory_tracker.c
 * @brief Comprehensive automated unit and integration tests for memory tracker
 */

#include "memory_tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

/* Test Runner State */
static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define ANSI_GREEN  "\033[32m"
#define ANSI_RED    "\033[31m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_RESET  "\033[0m"

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf(ANSI_RED "    Assertion FAILED: %s (at %s:%d)\n" ANSI_RESET, msg, __FILE__, __LINE__); \
        return false; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    g_tests_run++; \
    tracker_reset(); \
    printf("Running %-50s ... ", #fn); \
    fflush(stdout); \
    if (fn()) { \
        printf(ANSI_GREEN "[PASS]\n" ANSI_RESET); \
        g_tests_passed++; \
    } else { \
        printf(ANSI_RED "[FAIL]\n" ANSI_RESET); \
        g_tests_failed++; \
    } \
} while (0)

/* ========================================================================= */
/* Test Cases                                                                */
/* ========================================================================= */

/**
 * Test 1: Verify allocation record creation and metadata accuracy
 */
static bool test_record_creation(void) {
    void *ptr = MALLOC(128);
    TEST_ASSERT(ptr != NULL, "MALLOC(128) must return non-NULL");
    TEST_ASSERT(tracker_is_pointer_active(ptr), "Pointer must be registered as active");

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.total_allocations == 1, "Total allocations must be 1");
    TEST_ASSERT(stats.current_active_blocks == 1, "Active blocks must be 1");
    TEST_ASSERT(stats.current_active_bytes == 128, "Active bytes must be 128");

    FREE(ptr);
    return true;
}

/**
 * Test 2: Verify record removal after successful free
 */
static bool test_record_removal(void) {
    void *ptr = MALLOC(256);
    TEST_ASSERT(ptr != NULL, "MALLOC(256) failed");

    FREE(ptr);
    TEST_ASSERT(!tracker_is_pointer_active(ptr), "Pointer must not be active after FREE");

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.total_frees == 1, "Total frees must be 1");
    TEST_ASSERT(stats.current_active_blocks == 0, "Active blocks must be 0");
    TEST_ASSERT(stats.current_active_bytes == 0, "Active bytes must be 0");
    return true;
}

/**
 * Test 3: Verify realloc correctly updates size, metrics, and pointer record
 */
static bool test_realloc_updates_record(void) {
    void *ptr = MALLOC(100);
    TEST_ASSERT(ptr != NULL, "Initial MALLOC(100) failed");

    void *expanded = REALLOC(ptr, 500);
    TEST_ASSERT(expanded != NULL, "REALLOC(ptr, 500) failed");
    TEST_ASSERT(tracker_is_pointer_active(expanded), "New pointer must be active");
    if (expanded != ptr) {
        TEST_ASSERT(!tracker_is_pointer_active(ptr), "Old pointer must no longer be active");
    }

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.current_active_blocks == 1, "Active blocks must remain 1");
    TEST_ASSERT(stats.current_active_bytes == 500, "Active bytes must be 500");
    TEST_ASSERT(stats.total_reallocations == 1, "Total reallocations must be 1");

    FREE(expanded);
    return true;
}

/**
 * Test 4: Verify realloc shrinking updates byte counts accurately
 */
static bool test_realloc_shrink(void) {
    void *ptr = MALLOC(1000);
    TEST_ASSERT(ptr != NULL, "Initial MALLOC(1000) failed");

    void *shrunk = REALLOC(ptr, 200);
    TEST_ASSERT(shrunk != NULL, "REALLOC(ptr, 200) failed");

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.current_active_blocks == 1, "Active blocks must be 1");
    TEST_ASSERT(stats.current_active_bytes == 200, "Active bytes must shrink to 200");

    FREE(shrunk);
    return true;
}

/**
 * Test 5: Verify failed reallocation preserves original pointer and record
 */
static bool test_failed_realloc_preserves_record(void) {
    void *ptr = MALLOC(64);
    TEST_ASSERT(ptr != NULL, "Initial allocation failed");

    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) tracker_set_output_stream(devnull);

    /* Attempting to realloc to SIZE_MAX - 1 will fail safely */
    void *failed_ptr = REALLOC(ptr, SIZE_MAX - 1);

    tracker_set_output_stream(stdout);
    if (devnull) fclose(devnull);

    TEST_ASSERT(failed_ptr == NULL, "Impossible reallocation must return NULL");

    /* Original pointer MUST still be active and tracked */
    TEST_ASSERT(tracker_is_pointer_active(ptr), "Original pointer must remain active after failed realloc");

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.current_active_blocks == 1, "Active blocks must still be 1");
    TEST_ASSERT(stats.current_active_bytes == 64, "Active bytes must still be 64");
    TEST_ASSERT(stats.allocation_failures >= 1, "Failure count must increment");

    /* Free original pointer */
    FREE(ptr);
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.current_active_blocks == 0, "Active blocks must be 0 after free");
    return true;
}

/**
 * Test 6: Verify leak counts and total byte calculations are accurate
 */
static bool test_leak_counts_and_bytes(void) {
    void *p1 = MALLOC(128);
    void *p2 = MALLOC(256);
    void *p3 = MALLOC(512);

    /* Free middle block */
    FREE(p2);

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.current_active_blocks == 2, "Active blocks must be 2");
    TEST_ASSERT(stats.current_active_bytes == (128 + 512), "Active bytes must equal 640");

    /* Redirect output temporarily to /dev/null for report calculation check */
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) {
        tracker_set_output_stream(devnull);
    }
    size_t leaks = tracker_report_leaks();
    tracker_set_output_stream(stdout);
    if (devnull) fclose(devnull);

    TEST_ASSERT(leaks == 2, "Reported leaks must be 2");

    FREE(p1);
    FREE(p3);
    return true;
}

/**
 * Test 7: Verify Calloc zero-initialization and integer overflow safety
 */
static bool test_calloc_behavior_and_overflow(void) {
    /* Test valid calloc */
    size_t count = 50;
    size_t elem_size = sizeof(int);
    int *arr = (int *)CALLOC(count, elem_size);
    TEST_ASSERT(arr != NULL, "CALLOC failed");

    for (size_t i = 0; i < count; ++i) {
        TEST_ASSERT(arr[i] == 0, "CALLOC elements must be zero-initialized");
    }
    FREE(arr);

    /* Test integer overflow detection */
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) tracker_set_output_stream(devnull);

    void *bad_calloc = CALLOC(SIZE_MAX / 2, 4);
    TEST_ASSERT(bad_calloc == NULL, "CALLOC with overflowing dimensions must return NULL");

    tracker_set_output_stream(stdout);
    if (devnull) fclose(devnull);

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.calloc_overflow_attempts == 1, "Calloc overflow counter must be 1");
    return true;
}

/**
 * Test 8: Verify double-free is safely caught and blocked
 */
static bool test_double_free_detection(void) {
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) tracker_set_output_stream(devnull);

    void *ptr = MALLOC(64);
    TEST_ASSERT(ptr != NULL, "MALLOC failed");

    FREE(ptr);
    /* Second free must be caught */
    FREE(ptr);

    tracker_set_output_stream(stdout);
    if (devnull) fclose(devnull);

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.double_free_attempts == 1, "Double-free attempt must be recorded");
    TEST_ASSERT(stats.current_active_blocks == 0, "Active blocks must remain 0");
    return true;
}

/**
 * Test 9: Verify unknown / untracked pointer free is safely caught
 */
static bool test_unknown_pointer_detection(void) {
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) tracker_set_output_stream(devnull);

    int stack_var = 123;
    FREE(&stack_var);

    void *dummy_heap = (void *)(uintptr_t)0xDEADCAFE;
    FREE(dummy_heap);

    tracker_set_output_stream(stdout);
    if (devnull) fclose(devnull);

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.unknown_free_attempts == 2, "Unknown free attempts must be 2");
    return true;
}

/**
 * Test 10: Verify FREE(NULL) standard compliance
 */
static bool test_null_pointer_free(void) {
    FREE(NULL);

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.total_frees == 0, "FREE(NULL) must not count as a valid free");
    TEST_ASSERT(stats.unknown_free_attempts == 0, "FREE(NULL) must not trigger unknown pointer warning");
    return true;
}

/**
 * Test 11: Verify realloc(NULL, size) behaves like malloc(size)
 */
static bool test_realloc_null_input(void) {
    void *ptr = REALLOC(NULL, 128);
    TEST_ASSERT(ptr != NULL, "REALLOC(NULL, 128) must allocate 128 bytes");
    TEST_ASSERT(tracker_is_pointer_active(ptr), "Pointer must be active");

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.current_active_bytes == 128, "Active bytes must be 128");

    FREE(ptr);
    return true;
}

/**
 * Test 12: Verify realloc(ptr, 0) behaves as documented (frees block)
 */
static bool test_realloc_zero_size(void) {
    void *ptr = MALLOC(128);
    TEST_ASSERT(ptr != NULL, "MALLOC failed");

    void *res = REALLOC(ptr, 0);
    TEST_ASSERT(res == NULL, "REALLOC(ptr, 0) must return NULL");
    TEST_ASSERT(!tracker_is_pointer_active(ptr), "Pointer must be freed");

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.current_active_blocks == 0, "Active blocks must be 0");
    return true;
}

/**
 * Test 13: Verify tracker behavior when no allocations remain
 */
static bool test_zero_allocations_behavior(void) {
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) tracker_set_output_stream(devnull);

    size_t leaks = tracker_report_leaks();

    tracker_set_output_stream(stdout);
    if (devnull) fclose(devnull);

    TEST_ASSERT(leaks == 0, "Leaks must be 0");

    TrackerStats stats;
    tracker_get_stats(&stats);
    TEST_ASSERT(stats.current_active_blocks == 0, "Active blocks must be 0");
    TEST_ASSERT(stats.current_active_bytes == 0, "Active bytes must be 0");
    return true;
}

/* ========================================================================= */
/* Test Main Runner                                                          */
/* ========================================================================= */

int main(void) {
    printf(ANSI_BOLD "============================================================\n" ANSI_RESET);
    printf(ANSI_BOLD " Running Memory Tracker Automated Test Suite\n" ANSI_RESET);
    printf(ANSI_BOLD "============================================================\n\n" ANSI_RESET);

    tracker_init();

    RUN_TEST(test_record_creation);
    RUN_TEST(test_record_removal);
    RUN_TEST(test_realloc_updates_record);
    RUN_TEST(test_realloc_shrink);
    RUN_TEST(test_failed_realloc_preserves_record);
    RUN_TEST(test_leak_counts_and_bytes);
    RUN_TEST(test_calloc_behavior_and_overflow);
    RUN_TEST(test_double_free_detection);
    RUN_TEST(test_unknown_pointer_detection);
    RUN_TEST(test_null_pointer_free);
    RUN_TEST(test_realloc_null_input);
    RUN_TEST(test_realloc_zero_size);
    RUN_TEST(test_zero_allocations_behavior);

    printf(ANSI_BOLD "\n============================================================\n" ANSI_RESET);
    printf(ANSI_BOLD " Test Results: %d/%d passed (%d failed)\n" ANSI_RESET,
           g_tests_passed, g_tests_run, g_tests_failed);
    printf(ANSI_BOLD "============================================================\n\n" ANSI_RESET);

    return (g_tests_failed == 0) ? 0 : 1;
}
