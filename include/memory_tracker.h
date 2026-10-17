/**
 * @file memory_tracker.h
 * @brief Dynamic Memory Allocation Tracker and Memory Leak Detector
 *
 * Provides wrappers around standard memory management functions (malloc, calloc,
 * realloc, free) to track dynamic allocations, detect memory leaks at program exit,
 * and diagnose common memory bugs (double-free, freeing unknown pointers, etc.).
 */

#ifndef MEMORY_TRACKER_H
#define MEMORY_TRACKER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocation lifecycle status
 */
typedef enum {
    ALLOC_STATUS_ACTIVE = 1,  /**< Currently allocated and active */
    ALLOC_STATUS_FREED  = 2   /**< Previously allocated, now freed */
} AllocationStatus;

/**
 * @brief Metadata record for a single tracked memory allocation
 */
typedef struct AllocationRecord {
    void *address;                     /**< Base memory address returned to user */
    size_t size;                       /**< Requested allocation size in bytes */
    char file[256];                    /**< Source filename where allocation occurred */
    int line;                          /**< Source line number where allocation occurred */
    unsigned long id;                  /**< Unique sequential allocation ID */
    AllocationStatus status;           /**< Active or Freed status */
    char realloc_file[256];            /**< File where last realloc occurred (if applicable) */
    int realloc_line;                  /**< Line where last realloc occurred (if applicable) */
    struct AllocationRecord *next;     /**< Collision chain pointer for hash table */
} AllocationRecord;

/**
 * @brief Global statistics and metrics maintained by the memory tracker
 */
typedef struct {
    size_t total_allocations;          /**< Total number of malloc/calloc calls */
    size_t total_frees;                /**< Total number of valid free calls */
    size_t total_reallocations;        /**< Total number of realloc calls */
    size_t current_active_blocks;      /**< Current active (unfreed) blocks */
    size_t current_active_bytes;       /**< Current total active bytes allocated */
    size_t peak_active_blocks;         /**< High watermark of concurrently active blocks */
    size_t peak_active_bytes;          /**< High watermark of active bytes allocated */
    size_t total_allocated_bytes;      /**< Cumulative bytes allocated over runtime */
    size_t double_free_attempts;       /**< Number of detected double-free attempts */
    size_t unknown_free_attempts;      /**< Number of attempted frees of untracked pointers */
    size_t untracked_realloc_attempts; /**< Number of attempted reallocs of untracked pointers */
    size_t allocation_failures;        /**< Number of system allocator failures */
    size_t calloc_overflow_attempts;   /**< Number of calloc calls with integer overflow */
} TrackerStats;

/* ========================================================================= */
/* Core Tracking Function Prototypes                                         */
/* ========================================================================= */

/**
 * @brief Wrapper for malloc() that records allocation metadata.
 *
 * @param size Requested memory block size in bytes.
 * @param file Source filename where allocation originated.
 * @param line Source line number where allocation originated.
 * @return Pointer to allocated memory, or NULL if allocation fails.
 */
void *tracked_malloc(size_t size, const char *file, int line);

/**
 * @brief Wrapper for calloc() that verifies overflow and records metadata.
 *
 * @param count Number of elements.
 * @param size Size of each element in bytes.
 * @param file Source filename where allocation originated.
 * @param line Source line number where allocation originated.
 * @return Pointer to zero-initialized memory, or NULL if allocation fails.
 */
void *tracked_calloc(size_t count, size_t size, const char *file, int line);

/**
 * @brief Wrapper for realloc() that handles resizing and metadata updates.
 *
 * Safely handles:
 * - ptr == NULL (acts as tracked_malloc)
 * - size == 0 (acts as tracked_free, returns NULL)
 * - Failed reallocation (preserves original block and metadata)
 * - Reallocation to new address (migrates metadata seamlessly)
 *
 * @param ptr Pointer to existing memory block, or NULL.
 * @param size New requested size in bytes.
 * @param file Source filename where reallocation originated.
 * @param line Source line number where reallocation originated.
 * @return Pointer to reallocated memory, or NULL on failure.
 */
void *tracked_realloc(void *ptr, size_t size, const char *file, int line);

/**
 * @brief Wrapper for free() that validates pointer and removes tracking.
 *
 * Safely handles:
 * - ptr == NULL (no-op, per standard C)
 * - Double-free detection (safely rejected, does not crash system allocator)
 * - Unknown/untracked pointer detection (safely rejected, no system free)
 *
 * @param ptr Pointer to memory block to release.
 * @param file Source filename where free originated.
 * @param line Source line number where free originated.
 */
void tracked_free(void *ptr, const char *file, int line);

/* ========================================================================= */
/* Tracking Management and Reporting Functions                               */
/* ========================================================================= */

/**
 * @brief Explicitly initialize the tracker subsystem.
 *
 * Note: Tracker auto-initializes on first allocation if not called manually.
 * Also registers tracker_report_leaks() with atexit().
 */
void tracker_init(void);

/**
 * @brief Configure output stream for reports and diagnostics.
 *
 * @param stream Destination FILE pointer (defaults to stdout).
 */
void tracker_set_output_stream(FILE *stream);

/**
 * @brief Enable or disable verbose real-time logging of each allocation/free.
 *
 * @param enable True to print log per operation, false for silent tracking.
 */
void tracker_set_verbose(bool enable);

/**
 * @brief Generate and print the memory leak report to the configured output stream.
 *
 * Formatted to match standard leak reporting specifications.
 * Automatically called at program exit via atexit().
 *
 * @return Number of leaked memory blocks found.
 */
size_t tracker_report_leaks(void);

/**
 * @brief Retrieve a snapshot of the current tracker statistics.
 *
 * @param stats Pointer to TrackerStats struct to populate.
 */
void tracker_get_stats(TrackerStats *stats);

/**
 * @brief Reset all tracker state and free internal metadata.
 *
 * Useful for test suites that need a clean slate between test scenarios.
 */
void tracker_reset(void);

/**
 * @brief Print active allocations currently tracked.
 *
 * @param stream Output stream to print to.
 */
void tracker_dump_active(FILE *stream);

/**
 * @brief Check if a pointer is currently active in the tracker.
 *
 * @param ptr Memory address to check.
 * @return true if currently tracked as active, false otherwise.
 */
bool tracker_is_pointer_active(const void *ptr);

/* ========================================================================= */
/* Convenience Macros for Source Location Capture                            */
/* ========================================================================= */

#define MALLOC(size)        tracked_malloc((size), __FILE__, __LINE__)
#define CALLOC(count, size) tracked_calloc((count), (size), __FILE__, __LINE__)
#define REALLOC(ptr, size)  tracked_realloc((ptr), (size), __FILE__, __LINE__)
#define FREE(ptr)           tracked_free((ptr), __FILE__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_TRACKER_H */
