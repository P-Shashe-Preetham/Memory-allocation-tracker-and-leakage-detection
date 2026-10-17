#define _POSIX_C_SOURCE 200809L

#include "proc_reader.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void describe_proc_state(char state, char *buf, size_t len) {
    switch (state) {
        case 'R': mw_strlcpy(buf, "Running", len); break;
        case 'S': mw_strlcpy(buf, "Sleeping", len); break;
        case 'D': mw_strlcpy(buf, "Disk Sleep", len); break;
        case 'Z': mw_strlcpy(buf, "Zombie", len); break;
        case 'T': mw_strlcpy(buf, "Stopped", len); break;
        case 't': mw_strlcpy(buf, "Tracing Stop", len); break;
        case 'I': mw_strlcpy(buf, "Idle", len); break;
        case 'X':
        case 'x': mw_strlcpy(buf, "Dead", len); break;
        default:  mw_strlcpy(buf, "Unknown", len); break;
    }
}

/*
 * Parse a "Key: <value> [kB]" line and convert to bytes.
 * Returns MW_OK on valid parse, MW_ERR_PARSE if malformed.
 */
static mw_status_t parse_kb_value_field(const char *val_str, uint64_t *out_bytes) {
    if (val_str == NULL || out_bytes == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    while (*val_str != '\0' && isspace((unsigned char)*val_str)) {
        val_str++;
    }
    if (!isdigit((unsigned char)*val_str)) {
        return MW_ERR_PARSE;
    }
    errno = 0;
    char *endptr = NULL;
    unsigned long long kb = strtoull(val_str, &endptr, 10);
    if (errno == ERANGE || endptr == val_str) {
        return MW_ERR_PARSE;
    }
    while (*endptr != '\0' && isspace((unsigned char)*endptr)) {
        endptr++;
    }
    if (*endptr == '\0' || strcmp(endptr, "kB") == 0 || strcmp(endptr, "KB") == 0) {
        return mw_kb_to_bytes((uint64_t)kb, out_bytes);
    }
    if (strcmp(endptr, "B") == 0) {
        *out_bytes = (uint64_t)kb;
        return MW_OK;
    }
    return MW_ERR_PARSE;
}

mw_status_t mw_parse_meminfo_str(const char *content, mw_sys_memory_t *out_mem) {
    if (content == NULL || out_mem == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    memset(out_mem, 0, sizeof(*out_mem));

    bool seen_total = false;
    bool seen_free  = false;
    bool seen_avail = false;

    const char *line_start = content;
    while (*line_start != '\0') {
        const char *line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);

        char line[256];
        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1;
        }
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';

        char *colon = strchr(line, ':');
        if (colon != NULL) {
            *colon = '\0';
            char *key = mw_strtrim(line);
            char *val = mw_strtrim(colon + 1);
            uint64_t bytes = 0;

            if (strcmp(key, "MemTotal") == 0) {
                if (parse_kb_value_field(val, &bytes) != MW_OK) return MW_ERR_PARSE;
                out_mem->mem_total_bytes = bytes;
                seen_total = true;
            } else if (strcmp(key, "MemFree") == 0) {
                if (parse_kb_value_field(val, &bytes) != MW_OK) return MW_ERR_PARSE;
                out_mem->mem_free_bytes = bytes;
                seen_free = true;
            } else if (strcmp(key, "MemAvailable") == 0) {
                if (parse_kb_value_field(val, &bytes) != MW_OK) return MW_ERR_PARSE;
                out_mem->mem_available_bytes = bytes;
                seen_avail = true;
            } else if (strcmp(key, "Buffers") == 0) {
                if (parse_kb_value_field(val, &bytes) == MW_OK) out_mem->buffers_bytes = bytes;
            } else if (strcmp(key, "Cached") == 0) {
                if (parse_kb_value_field(val, &bytes) == MW_OK) out_mem->cached_bytes = bytes;
            } else if (strcmp(key, "Shmem") == 0) {
                if (parse_kb_value_field(val, &bytes) == MW_OK) out_mem->shmem_bytes = bytes;
            } else if (strcmp(key, "SReclaimable") == 0) {
                if (parse_kb_value_field(val, &bytes) == MW_OK) out_mem->sreclaimable_bytes = bytes;
            } else if (strcmp(key, "SwapTotal") == 0) {
                if (parse_kb_value_field(val, &bytes) != MW_OK) return MW_ERR_PARSE;
                out_mem->swap_total_bytes = bytes;
            } else if (strcmp(key, "SwapFree") == 0) {
                if (parse_kb_value_field(val, &bytes) != MW_OK) return MW_ERR_PARSE;
                out_mem->swap_free_bytes = bytes;
            }
        }

        if (line_end == NULL) {
            break;
        }
        line_start = line_end + 1;
    }

    if (!seen_total || !seen_free || out_mem->mem_total_bytes == 0) {
        return MW_ERR_PARSE;
    }

    out_mem->has_mem_available = seen_avail;
    if (!seen_avail) {
        /*
         * Fallback calculation when MemAvailable is absent:
         * Estimate reclaimable cache as Free + Buffers + Cached + SReclaimable - Shmem
         */
        uint64_t reclaimable = out_mem->buffers_bytes + out_mem->cached_bytes + out_mem->sreclaimable_bytes;
        if (reclaimable > out_mem->shmem_bytes) {
            reclaimable -= out_mem->shmem_bytes;
        } else {
            reclaimable = 0;
        }
        uint64_t est_avail = out_mem->mem_free_bytes + reclaimable;
        if (est_avail > out_mem->mem_total_bytes) {
            est_avail = out_mem->mem_total_bytes;
        }
        out_mem->mem_available_bytes = est_avail;
    }

    /*
     * Used memory definition:
     * Used = MemTotal - MemAvailable (reflecting memory actively in use that
     * cannot be immediately reclaimed without swapping, unlike raw MemTotal - MemFree
     * which misclassifies reclaimable page cache as unavailable).
     */
    if (out_mem->mem_total_bytes > out_mem->mem_available_bytes) {
        out_mem->used_bytes = out_mem->mem_total_bytes - out_mem->mem_available_bytes;
    } else {
        out_mem->used_bytes = 0;
    }
    out_mem->utilization_pct = mw_calc_percentage(out_mem->used_bytes, out_mem->mem_total_bytes);

    out_mem->swap_available = (out_mem->swap_total_bytes > 0);
    if (out_mem->swap_available) {
        if (out_mem->swap_total_bytes > out_mem->swap_free_bytes) {
            out_mem->swap_used_bytes = out_mem->swap_total_bytes - out_mem->swap_free_bytes;
        } else {
            out_mem->swap_used_bytes = 0;
        }
        out_mem->swap_utilization_pct = mw_calc_percentage(out_mem->swap_used_bytes, out_mem->swap_total_bytes);
    }

    return MW_OK;
}

static mw_status_t read_entire_proc_file(const char *path, char **out_buf, size_t *out_len) {
    if (path == NULL || out_buf == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    if (out_len) *out_len = 0;

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        if (errno == EACCES || errno == EPERM) {
            return MW_ERR_PERMISSION;
        }
        if (errno == ENOENT || errno == ESRCH) {
            return MW_ERR_NOT_FOUND;
        }
        return MW_ERR_IO;
    }

    size_t cap = 4096;
    size_t total = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        fclose(fp);
        return MW_ERR_NOMEM;
    }

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t nread;
    while ((nread = getline(&line, &line_cap, fp)) != -1) {
        if (total + (size_t)nread + 1 > cap) {
            size_t new_cap = cap * 2;
            while (new_cap < total + (size_t)nread + 1) {
                new_cap *= 2;
            }
            char *new_buf = realloc(buf, new_cap);
            if (new_buf == NULL) {
                free(line);
                free(buf);
                fclose(fp);
                return MW_ERR_NOMEM;
            }
            buf = new_buf;
            cap = new_cap;
        }
        memcpy(buf + total, line, (size_t)nread);
        total += (size_t)nread;
    }
    free(line);

    if (ferror(fp)) {
        int saved_errno = errno;
        free(buf);
        fclose(fp);
        if (saved_errno == EACCES || saved_errno == EPERM) return MW_ERR_PERMISSION;
        if (saved_errno == ESRCH || saved_errno == ENOENT) return MW_ERR_NOT_FOUND;
        return MW_ERR_IO;
    }
    fclose(fp);
    buf[total] = '\0';
    *out_buf = buf;
    if (out_len) *out_len = total;
    return MW_OK;
}

mw_status_t mw_read_meminfo(const char *proc_root, mw_sys_memory_t *out_mem) {
    if (proc_root == NULL || out_mem == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    char path[MW_MAX_PATH];
    int n = snprintf(path, sizeof(path), "%s/meminfo", proc_root);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return MW_ERR_INVALID_ARG;
    }
    char *content = NULL;
    mw_status_t st = read_entire_proc_file(path, &content, NULL);
    if (st != MW_OK) {
        return st;
    }
    st = mw_parse_meminfo_str(content, out_mem);
    free(content);
    return st;
}

mw_status_t mw_parse_proc_status_str(const char *content, mw_proc_info_t *out_proc) {
    if (content == NULL || out_proc == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    bool seen_name = false;
    bool seen_pid  = false;

    const char *line_start = content;
    while (*line_start != '\0') {
        const char *line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);

        char line[256];
        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1;
        }
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';

        char *colon = strchr(line, ':');
        if (colon != NULL) {
            *colon = '\0';
            char *key = mw_strtrim(line);
            char *val = mw_strtrim(colon + 1);

            if (strcmp(key, "Name") == 0) {
                if (val[0] == '\0') {
                    return MW_ERR_PARSE;
                }
                mw_strlcpy(out_proc->name, val, sizeof(out_proc->name));
                seen_name = true;
            } else if (strcmp(key, "State") == 0) {
                if (val[0] != '\0') {
                    out_proc->state = val[0];
                    describe_proc_state(out_proc->state, out_proc->state_desc, sizeof(out_proc->state_desc));
                }
            } else if (strcmp(key, "Pid") == 0) {
                int pid_val = 0;
                if (mw_parse_i32(val, &pid_val) != MW_OK || pid_val <= 0) {
                    return MW_ERR_PARSE;
                }
                out_proc->pid = (pid_t)pid_val;
                seen_pid = true;
            } else if (strcmp(key, "PPid") == 0) {
                int ppid_val = 0;
                if (mw_parse_i32(val, &ppid_val) == MW_OK && ppid_val >= 0) {
                    out_proc->ppid = (pid_t)ppid_val;
                }
            } else if (strcmp(key, "Uid") == 0) {
                unsigned long uid_val = 0;
                if (sscanf(val, "%lu", &uid_val) == 1) {
                    out_proc->uid = (uid_t)uid_val;
                }
            } else if (strcmp(key, "VmRSS") == 0) {
                uint64_t bytes = 0;
                if (parse_kb_value_field(val, &bytes) != MW_OK) {
                    return MW_ERR_PARSE;
                }
                out_proc->rss_bytes = bytes;
                out_proc->rss_valid = true;
            } else if (strcmp(key, "VmSize") == 0) {
                uint64_t bytes = 0;
                if (parse_kb_value_field(val, &bytes) != MW_OK) {
                    return MW_ERR_PARSE;
                }
                out_proc->vsz_bytes = bytes;
                out_proc->vsz_valid = true;
            }
        }

        if (line_end == NULL) {
            break;
        }
        line_start = line_end + 1;
    }

    if (!seen_name || !seen_pid) {
        return MW_ERR_PARSE;
    }

    /*
     * Important: If VmRSS / VmSize are absent in /proc/[PID]/status (e.g. kernel
     * threads or zombies), rss_valid and vsz_valid remain false so missing memory
     * information is never silently misinterpreted as zero usage.
     */
    if (!out_proc->rss_valid && !out_proc->vsz_valid) {
        out_proc->is_kernel_thread = true;
    }

    return MW_OK;
}

mw_status_t mw_parse_smaps_rollup_str(const char *content, mw_proc_info_t *out_proc) {
    if (content == NULL || out_proc == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    const char *line_start = content;
    while (*line_start != '\0') {
        const char *line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);

        char line[256];
        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1;
        }
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';

        char *colon = strchr(line, ':');
        if (colon != NULL) {
            *colon = '\0';
            char *key = mw_strtrim(line);
            char *val = mw_strtrim(colon + 1);
            uint64_t bytes = 0;

            if (strcmp(key, "Pss") == 0) {
                if (parse_kb_value_field(val, &bytes) != MW_OK) {
                    return MW_ERR_PARSE;
                }
                out_proc->pss_bytes = bytes;
                out_proc->pss_valid = true;
            } else if (strcmp(key, "Private_Dirty") == 0) {
                if (parse_kb_value_field(val, &bytes) == MW_OK) {
                    out_proc->private_dirty_bytes = bytes;
                }
            }
        }

        if (line_end == NULL) {
            break;
        }
        line_start = line_end + 1;
    }
    return MW_OK;
}

mw_status_t mw_parse_proc_stat_str(const char *content, char *out_state, uint64_t *out_starttime) {
    if (content == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    /*
     * /proc/[PID]/stat format:
     * field 1: pid
     * field 2: (comm)  -- comm may contain spaces and parentheses!
     * field 3: state
     * ...
     * field 22: starttime (unsigned long long ticks after boot)
     */
    const char *rparen = strrchr(content, ')');
    if (rparen == NULL || *(rparen + 1) == '\0') {
        return MW_ERR_PARSE;
    }
    const char *p = rparen + 1;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '\0') {
        return MW_ERR_PARSE;
    }

    char state_ch = *p;
    if (out_state != NULL) {
        *out_state = state_ch;
    }

    /* Advance past field 3 (state) to reach field 22 (19 fields after field 3) */
    int field_num = 3;
    while (*p != '\0' && field_num < 22) {
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        field_num++;
    }
    if (field_num != 22 || *p == '\0' || !isdigit((unsigned char)*p)) {
        return MW_ERR_PARSE;
    }

    errno = 0;
    char *endptr = NULL;
    unsigned long long starttime = strtoull(p, &endptr, 10);
    if (errno == ERANGE || endptr == p) {
        return MW_ERR_PARSE;
    }
    if (out_starttime != NULL) {
        *out_starttime = (uint64_t)starttime;
    }
    return MW_OK;
}

mw_status_t mw_read_process(const char *proc_root,
                            pid_t pid,
                            uint64_t total_mem_bytes,
                            mw_proc_info_t *out_proc) {
    if (proc_root == NULL || pid <= 0 || out_proc == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    memset(out_proc, 0, sizeof(*out_proc));
    out_proc->pid = pid;
    out_proc->state = '?';
    mw_strlcpy(out_proc->state_desc, "Unknown", sizeof(out_proc->state_desc));
    out_proc->sample_time_ms = mw_monotonic_ms();

    char status_path[MW_MAX_PATH];
    int n = snprintf(status_path, sizeof(status_path), "%s/%d/status", proc_root, (int)pid);
    if (n < 0 || (size_t)n >= sizeof(status_path)) {
        return MW_ERR_INVALID_ARG;
    }

    char *status_buf = NULL;
    mw_status_t st = read_entire_proc_file(status_path, &status_buf, NULL);
    if (st != MW_OK) {
        if (st == MW_ERR_PERMISSION) {
            out_proc->permission_denied = true;
        }
        return st;
    }

    st = mw_parse_proc_status_str(status_buf, out_proc);
    free(status_buf);
    if (st != MW_OK) {
        return st;
    }

    /* Read /proc/[PID]/stat for starttime (PID reuse protection) and fallback state */
    char stat_path[MW_MAX_PATH];
    n = snprintf(stat_path, sizeof(stat_path), "%s/%d/stat", proc_root, (int)pid);
    if (n > 0 && (size_t)n < sizeof(stat_path)) {
        char *stat_buf = NULL;
        if (read_entire_proc_file(stat_path, &stat_buf, NULL) == MW_OK) {
            char stat_state = '?';
            uint64_t starttime = 0;
            if (mw_parse_proc_stat_str(stat_buf, &stat_state, &starttime) == MW_OK) {
                out_proc->starttime_ticks = starttime;
                if (out_proc->state == '?') {
                    out_proc->state = stat_state;
                    describe_proc_state(stat_state, out_proc->state_desc, sizeof(out_proc->state_desc));
                }
            }
            free(stat_buf);
        }
    }

    /* Read /proc/[PID]/smaps_rollup when accessible */
    char smaps_path[MW_MAX_PATH];
    n = snprintf(smaps_path, sizeof(smaps_path), "%s/%d/smaps_rollup", proc_root, (int)pid);
    if (n > 0 && (size_t)n < sizeof(smaps_path)) {
        char *smaps_buf = NULL;
        if (read_entire_proc_file(smaps_path, &smaps_buf, NULL) == MW_OK) {
            mw_parse_smaps_rollup_str(smaps_buf, out_proc);
            free(smaps_buf);
        }
    }

    if (out_proc->rss_valid && total_mem_bytes > 0) {
        out_proc->mem_pct = mw_calc_percentage(out_proc->rss_bytes, total_mem_bytes);
    } else {
        out_proc->mem_pct = 0.0;
    }

    return MW_OK;
}

static bool is_numeric_dir_name(const char *name, pid_t *out_pid) {
    if (name == NULL || *name == '\0') {
        return false;
    }
    for (const char *p = name; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    int val = 0;
    if (mw_parse_i32(name, &val) != MW_OK || val <= 0) {
        return false;
    }
    if (out_pid != NULL) {
        *out_pid = (pid_t)val;
    }
    return true;
}

mw_status_t mw_enum_processes(const char *proc_root,
                              uint64_t total_mem_bytes,
                              mw_proc_info_t **out_list,
                              size_t *out_count,
                              size_t *out_inaccessible) {
    if (proc_root == NULL || out_list == NULL || out_count == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    *out_list = NULL;
    *out_count = 0;
    if (out_inaccessible) {
        *out_inaccessible = 0;
    }

    DIR *dir = opendir(proc_root);
    if (dir == NULL) {
        if (errno == EACCES || errno == EPERM) return MW_ERR_PERMISSION;
        if (errno == ENOENT) return MW_ERR_NOT_FOUND;
        return MW_ERR_IO;
    }

    size_t cap = 64;
    size_t count = 0;
    size_t inaccessible = 0;
    mw_proc_info_t *list = calloc(cap, sizeof(mw_proc_info_t));
    if (list == NULL) {
        closedir(dir);
        return MW_ERR_NOMEM;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        pid_t pid = 0;
        if (!is_numeric_dir_name(entry->d_name, &pid)) {
            continue;
        }

        mw_proc_info_t proc;
        mw_status_t st = mw_read_process(proc_root, pid, total_mem_bytes, &proc);
        if (st == MW_OK) {
            if (count == cap) {
                if (cap > SIZE_MAX / (2 * sizeof(mw_proc_info_t))) {
                    free(list);
                    closedir(dir);
                    return MW_ERR_OVERFLOW;
                }
                size_t new_cap = cap * 2;
                mw_proc_info_t *new_list = realloc(list, new_cap * sizeof(mw_proc_info_t));
                if (new_list == NULL) {
                    free(list);
                    closedir(dir);
                    return MW_ERR_NOMEM;
                }
                list = new_list;
                cap = new_cap;
            }
            list[count++] = proc;
        } else if (st == MW_ERR_PERMISSION) {
            inaccessible++;
        } else if (st == MW_ERR_NOT_FOUND) {
            /* Process exited between readdir() and open() -- normal race condition */
            continue;
        }
    }

    closedir(dir);
    *out_list = list;
    *out_count = count;
    if (out_inaccessible) {
        *out_inaccessible = inaccessible;
    }
    return MW_OK;
}

static int compare_proc_rss_desc(const void *a, const void *b) {
    const mw_proc_info_t *pa = (const mw_proc_info_t *)a;
    const mw_proc_info_t *pb = (const mw_proc_info_t *)b;

    /* Processes with valid RSS come before processes with missing RSS */
    if (pa->rss_valid && !pb->rss_valid) return -1;
    if (!pa->rss_valid && pb->rss_valid) return 1;

    if (pa->rss_valid && pb->rss_valid) {
        if (pa->rss_bytes > pb->rss_bytes) return -1;
        if (pa->rss_bytes < pb->rss_bytes) return 1;
    }

    if (pa->vsz_valid && pb->vsz_valid) {
        if (pa->vsz_bytes > pb->vsz_bytes) return -1;
        if (pa->vsz_bytes < pb->vsz_bytes) return 1;
    }

    if (pa->pid < pb->pid) return -1;
    if (pa->pid > pb->pid) return 1;
    return 0;
}

void mw_sort_processes_by_rss(mw_proc_info_t *list, size_t count) {
    if (list == NULL || count <= 1) {
        return;
    }
    qsort(list, count, sizeof(mw_proc_info_t), compare_proc_rss_desc);
}
