#define _POSIX_C_SOURCE 200809L

#include "memwatch.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char *mw_status_str(mw_status_t status) {
    switch (status) {
        case MW_OK:               return "Success";
        case MW_ERR_INVALID_ARG:  return "Invalid argument";
        case MW_ERR_IO:           return "I/O error";
        case MW_ERR_PERMISSION:   return "Permission denied";
        case MW_ERR_NOT_FOUND:    return "Resource or process not found";
        case MW_ERR_NOMEM:        return "Out of memory";
        case MW_ERR_PARSE:        return "Parse error / malformed input";
        case MW_ERR_TOOL_MISSING: return "Required diagnostic tool not found";
        case MW_ERR_EXEC:         return "Subprocess execution failed";
        case MW_ERR_TIMEOUT:      return "Execution timed out";
        case MW_ERR_OVERFLOW:     return "Numeric overflow";
        default:                  return "Unknown error";
    }
}

size_t mw_strlcpy(char *dst, const char *src, size_t dst_size) {
    if (dst == NULL || dst_size == 0) {
        return src ? strlen(src) : 0;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }
    size_t src_len = strlen(src);
    size_t copy_len = (src_len >= dst_size) ? (dst_size - 1) : src_len;
    if (copy_len > 0) {
        memcpy(dst, src, copy_len);
    }
    dst[copy_len] = '\0';
    return src_len;
}

char *mw_strtrim(char *str) {
    if (str == NULL) {
        return NULL;
    }
    while (*str != '\0' && isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0') {
        return str;
    }
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}

mw_status_t mw_parse_u64(const char *str, uint64_t *out_val) {
    if (str == NULL || out_val == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    while (*str != '\0' && isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0' || *str == '-' || *str == '+') {
        if (*str == '+') {
            str++;
            if (!isdigit((unsigned char)*str)) {
                return MW_ERR_PARSE;
            }
        } else {
            return MW_ERR_PARSE;
        }
    }
    if (!isdigit((unsigned char)*str)) {
        return MW_ERR_PARSE;
    }
    errno = 0;
    char *endptr = NULL;
    unsigned long long val = strtoull(str, &endptr, 10);
    if (errno == ERANGE) {
        return MW_ERR_OVERFLOW;
    }
    if (endptr == str) {
        return MW_ERR_PARSE;
    }
    while (*endptr != '\0' && isspace((unsigned char)*endptr)) {
        endptr++;
    }
    if (*endptr != '\0') {
        return MW_ERR_PARSE;
    }
    *out_val = (uint64_t)val;
    return MW_OK;
}

mw_status_t mw_parse_i32(const char *str, int *out_val) {
    if (str == NULL || out_val == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    while (*str != '\0' && isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0') {
        return MW_ERR_PARSE;
    }
    errno = 0;
    char *endptr = NULL;
    long val = strtol(str, &endptr, 10);
    if (errno == ERANGE || val < INT_MIN || val > INT_MAX) {
        return MW_ERR_OVERFLOW;
    }
    if (endptr == str) {
        return MW_ERR_PARSE;
    }
    while (*endptr != '\0' && isspace((unsigned char)*endptr)) {
        endptr++;
    }
    if (*endptr != '\0') {
        return MW_ERR_PARSE;
    }
    *out_val = (int)val;
    return MW_OK;
}

mw_status_t mw_parse_double(const char *str, double *out_val) {
    if (str == NULL || out_val == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    while (*str != '\0' && isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0') {
        return MW_ERR_PARSE;
    }
    errno = 0;
    char *endptr = NULL;
    double val = strtod(str, &endptr);
    if (errno == ERANGE || !isfinite(val)) {
        return MW_ERR_PARSE;
    }
    if (endptr == str) {
        return MW_ERR_PARSE;
    }
    while (*endptr != '\0' && isspace((unsigned char)*endptr)) {
        endptr++;
    }
    if (*endptr != '\0') {
        return MW_ERR_PARSE;
    }
    *out_val = val;
    return MW_OK;
}

mw_status_t mw_kb_to_bytes(uint64_t kb, uint64_t *out_bytes) {
    if (out_bytes == NULL) {
        return MW_ERR_INVALID_ARG;
    }
    if (kb > UINT64_MAX / 1024ULL) {
        return MW_ERR_OVERFLOW;
    }
    *out_bytes = kb * 1024ULL;
    return MW_OK;
}

double mw_calc_percentage(uint64_t part, uint64_t total) {
    if (total == 0) {
        return 0.0;
    }
    double pct = ((double)part * 100.0) / (double)total;
    if (pct < 0.0) {
        return 0.0;
    }
    return pct;
}

void mw_format_bytes(uint64_t bytes, char *buf, size_t buf_len) {
    if (buf == NULL || buf_len == 0) {
        return;
    }
    const double kib = 1024.0;
    const double mib = 1024.0 * 1024.0;
    const double gib = 1024.0 * 1024.0 * 1024.0;
    const double tib = 1024.0 * 1024.0 * 1024.0 * 1024.0;

    if ((double)bytes >= tib) {
        snprintf(buf, buf_len, "%.2f TiB", (double)bytes / tib);
    } else if ((double)bytes >= gib) {
        snprintf(buf, buf_len, "%.2f GiB", (double)bytes / gib);
    } else if ((double)bytes >= mib) {
        snprintf(buf, buf_len, "%.2f MiB", (double)bytes / mib);
    } else if ((double)bytes >= kib) {
        snprintf(buf, buf_len, "%.1f KiB", (double)bytes / kib);
    } else {
        snprintf(buf, buf_len, "%" PRIu64 " B", bytes);
    }
}

void mw_format_bytes_with_commas(uint64_t bytes, char *buf, size_t buf_len) {
    if (buf == NULL || buf_len == 0) {
        return;
    }
    char raw[32];
    int len = snprintf(raw, sizeof(raw), "%" PRIu64, bytes);
    if (len <= 0) {
        mw_strlcpy(buf, "0 bytes", buf_len);
        return;
    }
    char formatted[48];
    size_t out_idx = 0;
    for (int i = 0; i < len && out_idx + 2 < sizeof(formatted); i++) {
        if (i > 0 && ((len - i) % 3 == 0)) {
            formatted[out_idx++] = ',';
        }
        formatted[out_idx++] = raw[i];
    }
    formatted[out_idx] = '\0';
    snprintf(buf, buf_len, "%s bytes", formatted);
}

void mw_format_signed_bytes(int64_t delta_bytes, char *buf, size_t buf_len) {
    if (buf == NULL || buf_len == 0) {
        return;
    }
    if (delta_bytes == 0) {
        mw_strlcpy(buf, "0 B", buf_len);
        return;
    }
    char sign = (delta_bytes > 0) ? '+' : '-';
    uint64_t abs_val;
    if (delta_bytes == INT64_MIN) {
        abs_val = (uint64_t)INT64_MAX + 1ULL;
    } else {
        abs_val = (delta_bytes < 0) ? (uint64_t)(-delta_bytes) : (uint64_t)delta_bytes;
    }
    char mag[32];
    mw_format_bytes(abs_val, mag, sizeof(mag));
    snprintf(buf, buf_len, "%c%s", sign, mag);
}

uint64_t mw_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

void mw_format_timestamp(char *buf, size_t buf_len) {
    if (buf == NULL || buf_len == 0) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm_now;
    if (localtime_r(&now, &tm_now) == NULL) {
        mw_strlcpy(buf, "unknown-time", buf_len);
        return;
    }
    strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", &tm_now);
}

bool mw_should_use_color(FILE *stream, bool force_no_color) {
    if (force_no_color || stream == NULL) {
        return false;
    }
    if (getenv("NO_COLOR") != NULL) {
        return false;
    }
    const char *term = getenv("TERM");
    if (term == NULL || strcmp(term, "dumb") == 0) {
        return false;
    }
    int fd = fileno(stream);
    if (fd < 0 || !isatty(fd)) {
        return false;
    }
    return true;
}

const char *mw_color(bool enabled, const char *ansi_code) {
    return (enabled && ansi_code != NULL) ? ansi_code : "";
}

mw_status_t mw_find_executable(const char *name_or_path, char *resolved_out, size_t resolved_len) {
    if (name_or_path == NULL || name_or_path[0] == '\0' || resolved_out == NULL || resolved_len == 0) {
        return MW_ERR_INVALID_ARG;
    }

    if (strchr(name_or_path, '/') != NULL) {
        if (access(name_or_path, X_OK) == 0) {
            struct stat st;
            if (stat(name_or_path, &st) == 0 && S_ISREG(st.st_mode)) {
                mw_strlcpy(resolved_out, name_or_path, resolved_len);
                return MW_OK;
            }
        }
        return MW_ERR_TOOL_MISSING;
    }

    const char *path_env = getenv("PATH");
    if (path_env == NULL || path_env[0] == '\0') {
        path_env = "/usr/local/bin:/usr/bin:/bin";
    }

    char path_copy[4096];
    mw_strlcpy(path_copy, path_env, sizeof(path_copy));

    char *saveptr = NULL;
    char *dir = strtok_r(path_copy, ":", &saveptr);
    while (dir != NULL) {
        char candidate[MW_MAX_PATH];
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", dir, name_or_path);
        if (n > 0 && (size_t)n < sizeof(candidate)) {
            if (access(candidate, X_OK) == 0) {
                struct stat st;
                if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
                    mw_strlcpy(resolved_out, candidate, resolved_len);
                    return MW_OK;
                }
            }
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }

    return MW_ERR_TOOL_MISSING;
}

mw_status_t mw_mkdir_p(const char *dir_path) {
    if (dir_path == NULL || dir_path[0] == '\0') {
        return MW_ERR_INVALID_ARG;
    }
    char tmp[MW_MAX_PATH];
    if (mw_strlcpy(tmp, dir_path, sizeof(tmp)) >= sizeof(tmp)) {
        return MW_ERR_INVALID_ARG;
    }
    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return MW_ERR_IO;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return MW_ERR_IO;
    }
    return MW_OK;
}
