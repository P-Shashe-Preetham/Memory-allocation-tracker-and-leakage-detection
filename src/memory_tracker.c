/**
 * @file memory_tracker.c
 * @brief Dynamic Memory Allocation Tracker and Memory Leak Detector Implementation
 */

#define _POSIX_C_SOURCE 200809L

#include "memory_tracker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdbool.h>

/* Undefine standard allocator macros if any were imported */
#undef malloc
#undef calloc
#undef realloc
#undef free

/* Configure AddressSanitizer defaults if built with ASan */
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
const char *__asan_default_options(void) {
    return "allocator_may_return_null=1";
}
#endif

/* ========================================================================= */
/* Configuration Constants                                                   */
/* ========================================================================= */

#define HASH_TABLE_BUCKETS    2053   /* Prime number for active hash table */
#define FREED_TABLE_BUCKETS   509    /* Prime number for freed history */
#define MAX_FREED_HISTORY     2048   /* Maximum retained freed records */
#define EMERGENCY_POOL_SIZE   512    /* Reserve metadata pool if malloc fails */

/* ========================================================================= */
/* Internal Data Structures                                                  */
/* ========================================================================= */

/**
 * @brief Record of previously freed memory blocks to diagnose double-frees
 */
typedef struct FreedRecord {
    void *address;
    size_t size;
    unsigned long id;
    char alloc_file[256];
    int alloc_line;
    char free_file[256];
    int free_line;
    struct FreedRecord *next;
} FreedRecord;

/**
 * @brief State structure containing all tracker data and metrics
 */
typedef struct {
    AllocationRecord *active_buckets[HASH_TABLE_BUCKETS];
    FreedRecord      *freed_buckets[FREED_TABLE_BUCKETS];
    FreedRecord      freed_history_pool[MAX_FREED_HISTORY];
    size_t           freed_history_count;
    size_t           freed_history_head;

    AllocationRecord emergency_pool[EMERGENCY_POOL_SIZE];
    bool             emergency_used[EMERGENCY_POOL_SIZE];

    unsigned long    next_allocation_id;
    TrackerStats     stats;

    FILE             *out_stream;
    bool             verbose;
    bool             initialized;
    bool             atexit_registered;
    bool             report_printed;
    pthread_mutex_t  lock;
} MemoryTracker;

/* Global Tracker Instance */
static MemoryTracker g_tracker = {
    .active_buckets       = {NULL},
    .freed_buckets        = {NULL},
    .freed_history_count  = 0,
    .freed_history_head   = 0,
    .emergency_used       = {false},
    .next_allocation_id   = 0,
    .stats                = {0},
    .out_stream           = NULL,
    .verbose              = false,
    .initialized          = false,
    .atexit_registered    = false,
    .report_printed       = false,
    .lock                 = PTHREAD_MUTEX_INITIALIZER
};

/* ========================================================================= */
/* Hash & Utility Functions                                                  */
/* ========================================================================= */

/**
 * @brief Thomas Wang 64-bit to 32-bit integer hash for pointer addresses
 */
static inline size_t hash_address(const void *ptr, size_t bucket_count) {
    uintptr_t val = (uintptr_t)ptr;
    val = (~val) + (val << 18);
    val = val ^ (val >> 31);
    val = val * 21;
    val = val ^ (val >> 11);
    val = val + (val << 6);
    val = val ^ (val >> 22);
    return (size_t)(val % bucket_count);
}

static inline FILE *get_stream(void) {
    return g_tracker.out_stream != NULL ? g_tracker.out_stream : stdout;
}

/**
 * @brief Safely copy string and guarantee null-termination without strncpy truncation warnings
 */
static void safe_strcpy(char *dst, const char *src, size_t dst_size) {
    if (dst == NULL || dst_size == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ========================================================================= */
/* Metadata Pool & Allocation Management                                     */
/* ========================================================================= */

/**
 * @brief Allocate an AllocationRecord, using system malloc with emergency fallback
 */
static AllocationRecord *create_allocation_record(void) {
    /* Attempt system malloc first */
    AllocationRecord *rec = (AllocationRecord *)(malloc)(sizeof(AllocationRecord));
    if (rec != NULL) {
        memset(rec, 0, sizeof(AllocationRecord));
        return rec;
    }

    /* Fallback to emergency static pool if system malloc fails */
    for (size_t i = 0; i < EMERGENCY_POOL_SIZE; ++i) {
        if (!g_tracker.emergency_used[i]) {
            g_tracker.emergency_used[i] = true;
            rec = &g_tracker.emergency_pool[i];
            memset(rec, 0, sizeof(AllocationRecord));
            return rec;
        }
    }

    return NULL;
}

/**
 * @brief Release an AllocationRecord back to system or emergency pool
 */
static void release_allocation_record(AllocationRecord *rec) {
    if (rec == NULL) return;

    /* Check if record came from emergency static pool */
    if (rec >= g_tracker.emergency_pool &&
        rec < g_tracker.emergency_pool + EMERGENCY_POOL_SIZE) {
        size_t index = (size_t)(rec - g_tracker.emergency_pool);
        g_tracker.emergency_used[index] = false;
        return;
    }

    (free)(rec);
}

/* ========================================================================= */
/* Active Table & Freed History Helpers (Must be called under tracker lock)   */
/* ========================================================================= */

static AllocationRecord *find_active_record(const void *ptr, size_t *out_bucket, AllocationRecord ***out_prev_ptr) {
    size_t bucket = hash_address(ptr, HASH_TABLE_BUCKETS);
    if (out_bucket) *out_bucket = bucket;

    AllocationRecord **curr = &g_tracker.active_buckets[bucket];
    while (*curr) {
        if ((*curr)->address == ptr) {
            if (out_prev_ptr) *out_prev_ptr = curr;
            return *curr;
        }
        curr = &((*curr)->next);
    }

    if (out_prev_ptr) *out_prev_ptr = NULL;
    return NULL;
}

static FreedRecord *find_freed_record(const void *ptr) {
    size_t bucket = hash_address(ptr, FREED_TABLE_BUCKETS);
    FreedRecord *curr = g_tracker.freed_buckets[bucket];
    while (curr) {
        if (curr->address == ptr) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

static void record_freed_history(const AllocationRecord *active, const char *free_file, int free_line) {
    /* Use fixed circular history pool to prevent unbounded memory growth */
    size_t idx = g_tracker.freed_history_head;
    FreedRecord *entry = &g_tracker.freed_history_pool[idx];

    /* If reusing an existing slot, remove it from old hash bucket */
    if (entry->address != NULL) {
        size_t old_bucket = hash_address(entry->address, FREED_TABLE_BUCKETS);
        FreedRecord **prev = &g_tracker.freed_buckets[old_bucket];
        while (*prev) {
            if (*prev == entry) {
                *prev = entry->next;
                break;
            }
            prev = &((*prev)->next);
        }
    }

    /* Populate entry */
    entry->address = active->address;
    entry->size = active->size;
    entry->id = active->id;
    safe_strcpy(entry->alloc_file, active->file, sizeof(entry->alloc_file));
    entry->alloc_line = active->line;
    safe_strcpy(entry->free_file, free_file ? free_file : "unknown", sizeof(entry->free_file));
    entry->free_line = free_line;

    /* Insert into new hash bucket */
    size_t new_bucket = hash_address(entry->address, FREED_TABLE_BUCKETS);
    entry->next = g_tracker.freed_buckets[new_bucket];
    g_tracker.freed_buckets[new_bucket] = entry;

    /* Advance circular buffer pointer */
    g_tracker.freed_history_head = (idx + 1) % MAX_FREED_HISTORY;
    if (g_tracker.freed_history_count < MAX_FREED_HISTORY) {
        g_tracker.freed_history_count++;
    }
}

/* ========================================================================= */
/* Initialization and atexit Handler                                         */
/* ========================================================================= */

static void atexit_leak_reporter(void) {
    tracker_report_leaks();
}

static void ensure_initialized_unlocked(void) {
    if (!g_tracker.initialized) {
        g_tracker.initialized = true;
        if (!g_tracker.atexit_registered) {
            atexit(atexit_leak_reporter);
            g_tracker.atexit_registered = true;
        }
    }
}

void tracker_init(void) {
    pthread_mutex_lock(&g_tracker.lock);
    ensure_initialized_unlocked();
    pthread_mutex_unlock(&g_tracker.lock);
}

void tracker_set_output_stream(FILE *stream) {
    pthread_mutex_lock(&g_tracker.lock);
    g_tracker.out_stream = stream;
    pthread_mutex_unlock(&g_tracker.lock);
}

void tracker_set_verbose(bool enable) {
    pthread_mutex_lock(&g_tracker.lock);
    g_tracker.verbose = enable;
    pthread_mutex_unlock(&g_tracker.lock);
}

/* ========================================================================= */
/* Core Allocation Tracking Implementations                                  */
/* ========================================================================= */

void *tracked_malloc(size_t size, const char *file, int line) {
    pthread_mutex_lock(&g_tracker.lock);
    ensure_initialized_unlocked();

    /* 1. Perform actual system allocation */
    void *ptr = (malloc)(size);
    if (ptr == NULL) {
        g_tracker.stats.allocation_failures++;
        FILE *out = get_stream();
        fprintf(out, "[ERROR] Memory allocation failed: malloc(%zu bytes) at %s:%d\n",
                size, file ? file : "unknown", line);
        pthread_mutex_unlock(&g_tracker.lock);
        return NULL;
    }

    /* 2. Allocate and initialize metadata record */
    AllocationRecord *rec = create_allocation_record();
    if (rec == NULL) {
        /*
         * Critical robustness safeguard: If metadata allocation fails and emergency
         * pool is depleted, roll back the allocation to prevent creating an untracked
         * memory block that cannot be tracked or safely verified.
         */
        g_tracker.stats.allocation_failures++;
        FILE *out = get_stream();
        fprintf(out, "[CRITICAL] Tracker metadata allocation failed at %s:%d. "
                     "Rolling back memory allocation (%zu bytes) to prevent untracked leak.\n",
                file ? file : "unknown", line, size);
        (free)(ptr);
        pthread_mutex_unlock(&g_tracker.lock);
        return NULL;
    }

    /* 3. Populate metadata */
    rec->address = ptr;
    rec->size = size;
    safe_strcpy(rec->file, file ? file : "unknown", sizeof(rec->file));
    rec->line = line;
    rec->id = ++g_tracker.next_allocation_id;
    rec->status = ALLOC_STATUS_ACTIVE;
    rec->realloc_file[0] = '\0';
    rec->realloc_line = 0;

    /* 4. Insert into active hash table */
    size_t bucket = hash_address(ptr, HASH_TABLE_BUCKETS);
    rec->next = g_tracker.active_buckets[bucket];
    g_tracker.active_buckets[bucket] = rec;

    /* 5. Update metrics */
    g_tracker.stats.total_allocations++;
    g_tracker.stats.current_active_blocks++;
    g_tracker.stats.current_active_bytes += size;
    g_tracker.stats.total_allocated_bytes += size;

    if (g_tracker.stats.current_active_blocks > g_tracker.stats.peak_active_blocks) {
        g_tracker.stats.peak_active_blocks = g_tracker.stats.current_active_blocks;
    }
    if (g_tracker.stats.current_active_bytes > g_tracker.stats.peak_active_bytes) {
        g_tracker.stats.peak_active_bytes = g_tracker.stats.current_active_bytes;
    }

    if (g_tracker.verbose) {
        FILE *out = get_stream();
        fprintf(out, "[TRACKER] MALLOC #%lu: %zu bytes at %p (%s:%d)\n",
                rec->id, size, ptr, rec->file, rec->line);
    }

    pthread_mutex_unlock(&g_tracker.lock);
    return ptr;
}

void *tracked_calloc(size_t count, size_t size, const char *file, int line) {
    pthread_mutex_lock(&g_tracker.lock);
    ensure_initialized_unlocked();

    /* Check for integer overflow */
    if (count != 0 && size > SIZE_MAX / count) {
        g_tracker.stats.calloc_overflow_attempts++;
        FILE *out = get_stream();
        fprintf(out, "[ERROR] Potential integer overflow detected: calloc(%zu, %zu) at %s:%d\n",
                count, size, file ? file : "unknown", line);
        pthread_mutex_unlock(&g_tracker.lock);
        return NULL;
    }

    size_t total_size = count * size;

    /* 1. Perform actual system calloc */
    void *ptr = (calloc)(count, size);
    if (ptr == NULL) {
        g_tracker.stats.allocation_failures++;
        FILE *out = get_stream();
        fprintf(out, "[ERROR] Memory allocation failed: calloc(%zu, %zu = %zu bytes) at %s:%d\n",
                count, size, total_size, file ? file : "unknown", line);
        pthread_mutex_unlock(&g_tracker.lock);
        return NULL;
    }

    /* 2. Allocate and initialize metadata record */
    AllocationRecord *rec = create_allocation_record();
    if (rec == NULL) {
        g_tracker.stats.allocation_failures++;
        FILE *out = get_stream();
        fprintf(out, "[CRITICAL] Tracker metadata allocation failed at %s:%d. "
                     "Rolling back calloc (%zu bytes) to prevent untracked leak.\n",
                file ? file : "unknown", line, total_size);
        (free)(ptr);
        pthread_mutex_unlock(&g_tracker.lock);
        return NULL;
    }

    /* 3. Populate metadata */
    rec->address = ptr;
    rec->size = total_size;
    safe_strcpy(rec->file, file ? file : "unknown", sizeof(rec->file));
    rec->line = line;
    rec->id = ++g_tracker.next_allocation_id;
    rec->status = ALLOC_STATUS_ACTIVE;
    rec->realloc_file[0] = '\0';
    rec->realloc_line = 0;

    /* 4. Insert into active hash table */
    size_t bucket = hash_address(ptr, HASH_TABLE_BUCKETS);
    rec->next = g_tracker.active_buckets[bucket];
    g_tracker.active_buckets[bucket] = rec;

    /* 5. Update metrics */
    g_tracker.stats.total_allocations++;
    g_tracker.stats.current_active_blocks++;
    g_tracker.stats.current_active_bytes += total_size;
    g_tracker.stats.total_allocated_bytes += total_size;

    if (g_tracker.stats.current_active_blocks > g_tracker.stats.peak_active_blocks) {
        g_tracker.stats.peak_active_blocks = g_tracker.stats.current_active_blocks;
    }
    if (g_tracker.stats.current_active_bytes > g_tracker.stats.peak_active_bytes) {
        g_tracker.stats.peak_active_bytes = g_tracker.stats.current_active_bytes;
    }

    if (g_tracker.verbose) {
        FILE *out = get_stream();
        fprintf(out, "[TRACKER] CALLOC #%lu: %zu elements * %zu bytes = %zu bytes at %p (%s:%d)\n",
                rec->id, count, size, total_size, ptr, rec->file, rec->line);
    }

    pthread_mutex_unlock(&g_tracker.lock);
    return ptr;
}

void *tracked_realloc(void *ptr, size_t size, const char *file, int line) {
    /* Case 1: NULL pointer input is equivalent to malloc(size) per C standard */
    if (ptr == NULL) {
        return tracked_malloc(size, file, line);
    }

    /*
     * Case 2: Zero size request.
     * Documented Behavior: In this tracker (aligning with POSIX/C99 guidelines and
     * avoiding C23 undefined behavior), realloc(ptr, 0) releases the memory block
     * and returns NULL.
     */
    if (size == 0) {
        tracked_free(ptr, file, line);
        return NULL;
    }

    pthread_mutex_lock(&g_tracker.lock);
    ensure_initialized_unlocked();

    /* Find active record */
    AllocationRecord **prev_ptr = NULL;
    size_t old_bucket = 0;
    AllocationRecord *rec = find_active_record(ptr, &old_bucket, &prev_ptr);

    if (rec == NULL) {
        /* Check if pointer was previously freed */
        FreedRecord *freed = find_freed_record(ptr);
        FILE *out = get_stream();
        if (freed != NULL) {
            fprintf(out, "[ERROR] Attempted to realloc previously freed pointer %p at %s:%d\n"
                         "        (Originally allocated at %s:%d, freed at %s:%d)\n",
                    ptr, file ? file : "unknown", line,
                    freed->alloc_file, freed->alloc_line,
                    freed->free_file, freed->free_line);
        } else {
            fprintf(out, "[ERROR] Attempted to realloc unknown/untracked pointer %p at %s:%d\n",
                    ptr, file ? file : "unknown", line);
        }
        g_tracker.stats.untracked_realloc_attempts++;
        pthread_mutex_unlock(&g_tracker.lock);
        return NULL;
    }

    /* Perform actual system reallocation */
    void *new_ptr = (realloc)(ptr, size);
    if (new_ptr == NULL) {
        /*
         * Failed reallocation: The original pointer is preserved and remains valid.
         * The active tracking record MUST NOT be lost or modified.
         */
        g_tracker.stats.allocation_failures++;
        FILE *out = get_stream();
        fprintf(out, "[WARNING] Reallocation failed: realloc(%p, %zu bytes) at %s:%d. "
                     "Original allocation (#%lu, %zu bytes) preserved.\n",
                ptr, size, file ? file : "unknown", line, rec->id, rec->size);
        pthread_mutex_unlock(&g_tracker.lock);
        return NULL;
    }

    /* Reallocation succeeded */
    size_t old_size = rec->size;

    /* If address changed, update hash table position */
    if (new_ptr != ptr) {
        /* Unlink from old bucket */
        *prev_ptr = rec->next;

        /* Insert into new bucket */
        size_t new_bucket = hash_address(new_ptr, HASH_TABLE_BUCKETS);
        rec->next = g_tracker.active_buckets[new_bucket];
        g_tracker.active_buckets[new_bucket] = rec;
        rec->address = new_ptr;
    }

    /* Update record metadata */
    rec->size = size;
    safe_strcpy(rec->realloc_file, file ? file : "unknown", sizeof(rec->realloc_file));
    rec->realloc_line = line;

    /* Update tracker metrics */
    g_tracker.stats.total_reallocations++;
    g_tracker.stats.current_active_bytes = (g_tracker.stats.current_active_bytes - old_size) + size;
    if (size > old_size) {
        g_tracker.stats.total_allocated_bytes += (size - old_size);
    }
    if (g_tracker.stats.current_active_bytes > g_tracker.stats.peak_active_bytes) {
        g_tracker.stats.peak_active_bytes = g_tracker.stats.current_active_bytes;
    }

    if (g_tracker.verbose) {
        FILE *out = get_stream();
        fprintf(out, "[TRACKER] REALLOC #%lu: %zu -> %zu bytes at %p (%s:%d)\n",
                rec->id, old_size, size, new_ptr, rec->realloc_file, rec->realloc_line);
    }

    pthread_mutex_unlock(&g_tracker.lock);
    return new_ptr;
}

void tracked_free(void *ptr, const char *file, int line) {
    /* Safe handling of NULL per standard C: free(NULL) is a valid no-op */
    if (ptr == NULL) {
        if (g_tracker.verbose) {
            FILE *out = get_stream();
            fprintf(out, "[TRACKER] FREE(NULL) called at %s:%d (no-op)\n",
                    file ? file : "unknown", line);
        }
        return;
    }

    pthread_mutex_lock(&g_tracker.lock);
    ensure_initialized_unlocked();

    AllocationRecord **prev_ptr = NULL;
    size_t bucket = 0;
    AllocationRecord *rec = find_active_record(ptr, &bucket, &prev_ptr);

    if (rec == NULL) {
        /* Pointer is not currently active: check if it was previously freed */
        FreedRecord *freed = find_freed_record(ptr);
        FILE *out = get_stream();

        if (freed != NULL) {
            /* DOUBLE-FREE DETECTED */
            g_tracker.stats.double_free_attempts++;
            fprintf(out, "[ERROR] Double-free detected at %s:%d!\n"
                         "        Pointer:            %p (Allocation #%lu, %zu bytes)\n"
                         "        Allocated at:       %s:%d\n"
                         "        First freed at:     %s:%d\n"
                         "        Attempted free at:  %s:%d\n",
                    file ? file : "unknown", line,
                    ptr, freed->id, freed->size,
                    freed->alloc_file, freed->alloc_line,
                    freed->free_file, freed->free_line,
                    file ? file : "unknown", line);
        } else {
            /* UNKNOWN / UNTRACKED POINTER DETECTED */
            g_tracker.stats.unknown_free_attempts++;
            fprintf(out, "[ERROR] Attempted to free unknown/untracked pointer %p at %s:%d!\n",
                    ptr, file ? file : "unknown", line);
        }

        /*
         * CRITICAL: Do NOT call system free() on unknown or already freed pointer!
         * This prevents undefined behavior, heap corruption, and crashes.
         */
        pthread_mutex_unlock(&g_tracker.lock);
        return;
    }

    /* Valid free operation */
    /* 1. Record in freed history for future double-free detection */
    record_freed_history(rec, file, line);

    /* 2. Unlink from active hash table */
    *prev_ptr = rec->next;

    /* 3. Update metrics */
    g_tracker.stats.total_frees++;
    g_tracker.stats.current_active_blocks--;
    g_tracker.stats.current_active_bytes -= rec->size;

    if (g_tracker.verbose) {
        FILE *out = get_stream();
        fprintf(out, "[TRACKER] FREE #%lu: %zu bytes at %p (%s:%d)\n",
                rec->id, rec->size, ptr, file ? file : "unknown", line);
    }

    /* 4. Release metadata record and call real system free */
    release_allocation_record(rec);
    (free)(ptr);

    pthread_mutex_unlock(&g_tracker.lock);
}

/* ========================================================================= */
/* Reporting & Diagnostic Utilities                                          */
/* ========================================================================= */

static int compare_records_by_id(const void *a, const void *b) {
    const AllocationRecord *const *ra = (const AllocationRecord *const *)a;
    const AllocationRecord *const *rb = (const AllocationRecord *const *)b;
    if ((*ra)->id < (*rb)->id) return -1;
    if ((*ra)->id > (*rb)->id) return 1;
    return 0;
}

size_t tracker_report_leaks(void) {
    pthread_mutex_lock(&g_tracker.lock);

    FILE *out = get_stream();

    /* Count active allocations */
    size_t leak_count = 0;
    size_t leaked_bytes = 0;

    for (size_t b = 0; b < HASH_TABLE_BUCKETS; ++b) {
        AllocationRecord *curr = g_tracker.active_buckets[b];
        while (curr) {
            leak_count++;
            leaked_bytes += curr->size;
            curr = curr->next;
        }
    }

    if (leak_count == 0) {
        fprintf(out, "\nNo memory leaks detected.\n");
        fprintf(out, "Total allocations: %zu\n", g_tracker.stats.total_allocations);
        fprintf(out, "Total frees: %zu\n", g_tracker.stats.total_frees);
        g_tracker.report_printed = true;
        pthread_mutex_unlock(&g_tracker.lock);
        return 0;
    }

    /* Collect records into an array to sort by allocation ID for neat output */
    AllocationRecord **records = (AllocationRecord **)(malloc)(leak_count * sizeof(AllocationRecord *));
    if (records != NULL) {
        size_t idx = 0;
        for (size_t b = 0; b < HASH_TABLE_BUCKETS; ++b) {
            AllocationRecord *curr = g_tracker.active_buckets[b];
            while (curr && idx < leak_count) {
                records[idx++] = curr;
                curr = curr->next;
            }
        }
        qsort(records, leak_count, sizeof(AllocationRecord *), compare_records_by_id);
    }

    fprintf(out, "\n========== MEMORY LEAK REPORT ==========\n\n");

    if (records != NULL) {
        for (size_t i = 0; i < leak_count; ++i) {
            fprintf(out, "[LEAK] Allocation #%lu\n", records[i]->id);
            fprintf(out, "       Address: %p\n", records[i]->address);
            fprintf(out, "       Size: %zu bytes\n", records[i]->size);
            fprintf(out, "       Location: %s:%d\n\n", records[i]->file, records[i]->line);
        }
        (free)(records);
    } else {
        /* If temporary array allocation fails, print unsorted directly */
        for (size_t b = 0; b < HASH_TABLE_BUCKETS; ++b) {
            AllocationRecord *curr = g_tracker.active_buckets[b];
            while (curr) {
                fprintf(out, "[LEAK] Allocation #%lu\n", curr->id);
                fprintf(out, "       Address: %p\n", curr->address);
                fprintf(out, "       Size: %zu bytes\n", curr->size);
                fprintf(out, "       Location: %s:%d\n\n", curr->file, curr->line);
                curr = curr->next;
            }
        }
    }

    fprintf(out, "========================================\n");
    fprintf(out, "Total leaked blocks: %zu\n", leak_count);
    fprintf(out, "Total leaked memory: %zu bytes\n", leaked_bytes);
    fprintf(out, "========================================\n\n");

    g_tracker.report_printed = true;
    pthread_mutex_unlock(&g_tracker.lock);
    return leak_count;
}

void tracker_get_stats(TrackerStats *stats) {
    if (stats == NULL) return;
    pthread_mutex_lock(&g_tracker.lock);
    *stats = g_tracker.stats;
    pthread_mutex_unlock(&g_tracker.lock);
}

void tracker_dump_active(FILE *stream) {
    if (stream == NULL) stream = stdout;
    pthread_mutex_lock(&g_tracker.lock);

    fprintf(stream, "--- Current Active Allocations (%zu blocks, %zu bytes) ---\n",
            g_tracker.stats.current_active_blocks, g_tracker.stats.current_active_bytes);

    for (size_t b = 0; b < HASH_TABLE_BUCKETS; ++b) {
        AllocationRecord *curr = g_tracker.active_buckets[b];
        while (curr) {
            fprintf(stream, "  #%lu: %p (%zu bytes) at %s:%d\n",
                    curr->id, curr->address, curr->size, curr->file, curr->line);
            curr = curr->next;
        }
    }
    fprintf(stream, "---------------------------------------------------------\n");

    pthread_mutex_unlock(&g_tracker.lock);
}

bool tracker_is_pointer_active(const void *ptr) {
    if (ptr == NULL) return false;
    pthread_mutex_lock(&g_tracker.lock);
    AllocationRecord *rec = find_active_record(ptr, NULL, NULL);
    bool active = (rec != NULL);
    pthread_mutex_unlock(&g_tracker.lock);
    return active;
}

void tracker_reset(void) {
    pthread_mutex_lock(&g_tracker.lock);

    /* Free all active records metadata and allocated memory */
    for (size_t b = 0; b < HASH_TABLE_BUCKETS; ++b) {
        AllocationRecord *curr = g_tracker.active_buckets[b];
        while (curr) {
            AllocationRecord *next = curr->next;
            (free)(curr->address);
            release_allocation_record(curr);
            curr = next;
        }
        g_tracker.active_buckets[b] = NULL;
    }

    /* Clear freed history */
    memset(g_tracker.freed_buckets, 0, sizeof(g_tracker.freed_buckets));
    memset(g_tracker.freed_history_pool, 0, sizeof(g_tracker.freed_history_pool));
    g_tracker.freed_history_count = 0;
    g_tracker.freed_history_head = 0;

    /* Reset emergency pool */
    memset(g_tracker.emergency_used, 0, sizeof(g_tracker.emergency_used));

    /* Reset counters and metrics */
    g_tracker.next_allocation_id = 0;
    memset(&g_tracker.stats, 0, sizeof(g_tracker.stats));
    g_tracker.report_printed = false;

    pthread_mutex_unlock(&g_tracker.lock);
}
