#define _POSIX_C_SOURCE 200809L

#include "analyzer.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdlib.h>
#include <string.h>

void mw_leak_report_init(mw_leak_report_t *report) {
    if (report == NULL) {
        return;
    }
    memset(report, 0, sizeof(*report));
}

void mw_leak_report_free(mw_leak_report_t *report) {
    if (report == NULL) {
        return;
    }
    free(report->records);
    report->records = NULL;
    report->record_count = 0;
    report->record_capacity = 0;
}

const char *mw_leak_kind_label(mw_leak_kind_t kind) {
    switch (kind) {
        case MW_LEAK_DEFINITELY_LOST: return "Definitely lost";
        case MW_LEAK_INDIRECTLY_LOST: return "Indirectly lost";
        case MW_LEAK_POSSIBLY_LOST:   return "Possibly lost";
        case MW_LEAK_STILL_REACHABLE: return "Still reachable";
        case MW_LEAK_SUPPRESSED:      return "Suppressed";
        case MW_LEAK_OTHER_ERROR:     return "Other memory error";
        default:                      return "Unknown";
    }
}

mw_leak_kind_t mw_classify_valgrind_kind(const char *kind_str) {
    if (kind_str == NULL) {
        return MW_LEAK_OTHER_ERROR;
    }
    if (strcmp(kind_str, "Leak_DefinitelyLost") == 0) {
        return MW_LEAK_DEFINITELY_LOST;
    }
    if (strcmp(kind_str, "Leak_IndirectlyLost") == 0) {
        return MW_LEAK_INDIRECTLY_LOST;
    }
    if (strcmp(kind_str, "Leak_PossiblyLost") == 0) {
        return MW_LEAK_POSSIBLY_LOST;
    }
    if (strcmp(kind_str, "Leak_StillReachable") == 0) {
        return MW_LEAK_STILL_REACHABLE;
    }
    if (strcmp(kind_str, "Leak_Suppressed") == 0) {
        return MW_LEAK_SUPPRESSED;
    }
    return MW_LEAK_OTHER_ERROR;
}

static bool xml_node_is_element(const xmlNode *node, const char *name) {
    return (node != NULL &&
            node->type == XML_ELEMENT_NODE &&
            node->name != NULL &&
            strcmp((const char *)node->name, name) == 0);
}

static bool copy_node_text(const xmlNode *node, char *dst, size_t dst_len) {
    if (node == NULL || dst == NULL || dst_len == 0) {
        return false;
    }
    xmlChar *content = xmlNodeGetContent(node);
    if (content == NULL) {
        dst[0] = '\0';
        return false;
    }
    mw_strlcpy(dst, (const char *)content, dst_len);
    mw_strtrim(dst);
    xmlFree(content);
    return true;
}

static void parse_args_node(const xmlNode *args_node, mw_leak_report_t *report) {
    for (const xmlNode *cur = args_node->children; cur != NULL; cur = cur->next) {
        if (xml_node_is_element(cur, "argv")) {
            char args_accum[MW_MAX_CMDLINE];
            args_accum[0] = '\0';
            for (const xmlNode *arg_node = cur->children; arg_node != NULL; arg_node = arg_node->next) {
                if (xml_node_is_element(arg_node, "exe")) {
                    char exe_buf[MW_MAX_PATH];
                    if (copy_node_text(arg_node, exe_buf, sizeof(exe_buf)) && exe_buf[0] != '\0') {
                        if (report->target_exe[0] == '\0') {
                            mw_strlcpy(report->target_exe, exe_buf, sizeof(report->target_exe));
                        }
                        mw_strlcpy(args_accum, exe_buf, sizeof(args_accum));
                    }
                } else if (xml_node_is_element(arg_node, "arg")) {
                    char one_arg[256];
                    if (copy_node_text(arg_node, one_arg, sizeof(one_arg)) && one_arg[0] != '\0') {
                        size_t cur_len = strlen(args_accum);
                        if (cur_len + strlen(one_arg) + 2 < sizeof(args_accum)) {
                            if (cur_len > 0) {
                                strcat(args_accum, " ");
                            }
                            strcat(args_accum, one_arg);
                        }
                    }
                }
            }
            if (report->target_args[0] == '\0' && args_accum[0] != '\0') {
                mw_strlcpy(report->target_args, args_accum, sizeof(report->target_args));
            }
        }
    }
}

static void parse_stack_node(const xmlNode *stack_node, mw_leak_record_t *rec, mw_leak_report_t *report) {
    for (const xmlNode *fnode = stack_node->children; fnode != NULL; fnode = fnode->next) {
        if (!xml_node_is_element(fnode, "frame")) {
            continue;
        }
        if (rec->frame_count >= MW_MAX_STACK_FRAMES) {
            break;
        }
        mw_stack_frame_t *frame = &rec->frames[rec->frame_count];
        memset(frame, 0, sizeof(*frame));

        for (const xmlNode *child = fnode->children; child != NULL; child = child->next) {
            if (xml_node_is_element(child, "ip")) {
                copy_node_text(child, frame->ip, sizeof(frame->ip));
            } else if (xml_node_is_element(child, "fn")) {
                copy_node_text(child, frame->fn, sizeof(frame->fn));
            } else if (xml_node_is_element(child, "obj")) {
                copy_node_text(child, frame->obj, sizeof(frame->obj));
            } else if (xml_node_is_element(child, "dir")) {
                copy_node_text(child, frame->dir, sizeof(frame->dir));
            } else if (xml_node_is_element(child, "file")) {
                copy_node_text(child, frame->file, sizeof(frame->file));
            } else if (xml_node_is_element(child, "line")) {
                char line_buf[32];
                int line_no = 0;
                if (copy_node_text(child, line_buf, sizeof(line_buf)) &&
                    mw_parse_i32(line_buf, &line_no) == MW_OK && line_no > 0) {
                    frame->line = line_no;
                }
            }
        }

        if (frame->file[0] != '\0' && frame->line > 0) {
            report->has_debug_info = true;
        }
        rec->frame_count++;
        report->has_stack_traces = true;
    }
}

static bool is_duplicate_unique_id(const mw_leak_report_t *report, const char *unique_id) {
    if (unique_id == NULL || unique_id[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < report->record_count; i++) {
        if (strcmp(report->records[i].unique_id, unique_id) == 0) {
            return true;
        }
    }
    return false;
}

static mw_status_t append_leak_record(mw_leak_report_t *report, const mw_leak_record_t *rec) {
    if (report->record_count == report->record_capacity) {
        size_t new_cap = (report->record_capacity == 0) ? 16 : (report->record_capacity * 2);
        mw_leak_record_t *new_recs = realloc(report->records, new_cap * sizeof(mw_leak_record_t));
        if (new_recs == NULL) {
            return MW_ERR_NOMEM;
        }
        report->records = new_recs;
        report->record_capacity = new_cap;
    }
    report->records[report->record_count++] = *rec;
    return MW_OK;
}

static mw_status_t parse_error_node(const xmlNode *err_node, mw_leak_report_t *report) {
    mw_leak_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.kind = MW_LEAK_OTHER_ERROR;

    bool seen_kind = false;
    bool seen_stack = false;

    for (const xmlNode *cur = err_node->children; cur != NULL; cur = cur->next) {
        if (xml_node_is_element(cur, "unique")) {
            copy_node_text(cur, rec.unique_id, sizeof(rec.unique_id));
        } else if (xml_node_is_element(cur, "kind")) {
            if (copy_node_text(cur, rec.raw_kind, sizeof(rec.raw_kind)) && rec.raw_kind[0] != '\0') {
                rec.kind = mw_classify_valgrind_kind(rec.raw_kind);
                seen_kind = true;
            }
        } else if (xml_node_is_element(cur, "what")) {
            if (rec.what[0] == '\0') {
                copy_node_text(cur, rec.what, sizeof(rec.what));
            }
        } else if (xml_node_is_element(cur, "xwhat")) {
            for (const xmlNode *xchild = cur->children; xchild != NULL; xchild = xchild->next) {
                if (xml_node_is_element(xchild, "text")) {
                    copy_node_text(xchild, rec.what, sizeof(rec.what));
                } else if (xml_node_is_element(xchild, "leakedbytes")) {
                    char num_buf[64];
                    uint64_t val = 0;
                    if (copy_node_text(xchild, num_buf, sizeof(num_buf)) &&
                        mw_parse_u64(num_buf, &val) == MW_OK) {
                        rec.leaked_bytes = val;
                    } else {
                        return MW_ERR_PARSE;
                    }
                } else if (xml_node_is_element(xchild, "leakedblocks")) {
                    char num_buf[64];
                    uint64_t val = 0;
                    if (copy_node_text(xchild, num_buf, sizeof(num_buf)) &&
                        mw_parse_u64(num_buf, &val) == MW_OK) {
                        rec.leaked_blocks = val;
                    } else {
                        return MW_ERR_PARSE;
                    }
                }
            }
        } else if (xml_node_is_element(cur, "stack") && !seen_stack) {
            /* Take primary allocation stack */
            parse_stack_node(cur, &rec, report);
            seen_stack = true;
        }
    }

    if (!seen_kind) {
        return MW_ERR_PARSE;
    }

    /* Prevent double-counting if duplicate unique IDs appear */
    if (is_duplicate_unique_id(report, rec.unique_id)) {
        return MW_OK;
    }

    switch (rec.kind) {
        case MW_LEAK_DEFINITELY_LOST:
            report->definitely_lost_bytes  += rec.leaked_bytes;
            report->definitely_lost_blocks += rec.leaked_blocks;
            break;
        case MW_LEAK_INDIRECTLY_LOST:
            report->indirectly_lost_bytes  += rec.leaked_bytes;
            report->indirectly_lost_blocks += rec.leaked_blocks;
            break;
        case MW_LEAK_POSSIBLY_LOST:
            report->possibly_lost_bytes    += rec.leaked_bytes;
            report->possibly_lost_blocks   += rec.leaked_blocks;
            break;
        case MW_LEAK_STILL_REACHABLE:
            report->still_reachable_bytes  += rec.leaked_bytes;
            report->still_reachable_blocks += rec.leaked_blocks;
            break;
        case MW_LEAK_SUPPRESSED:
            report->suppressed_bytes       += rec.leaked_bytes;
            report->suppressed_blocks      += rec.leaked_blocks;
            break;
        case MW_LEAK_OTHER_ERROR:
        default:
            report->non_leak_error_count++;
            break;
    }

    return append_leak_record(report, &rec);
}

static mw_status_t parse_valgrind_xml_doc(xmlDocPtr doc, mw_leak_report_t *out_report) {
    if (doc == NULL || out_report == NULL) {
        return MW_ERR_PARSE;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!xml_node_is_element(root, "valgrindoutput")) {
        return MW_ERR_PARSE;
    }

    bool seen_protocol = false;

    for (const xmlNode *cur = root->children; cur != NULL; cur = cur->next) {
        if (xml_node_is_element(cur, "protocolversion")) {
            char ver_buf[32];
            int ver = 0;
            if (!copy_node_text(cur, ver_buf, sizeof(ver_buf)) ||
                mw_parse_i32(ver_buf, &ver) != MW_OK || ver <= 0) {
                return MW_ERR_PARSE;
            }
            seen_protocol = true;
        } else if (xml_node_is_element(cur, "pid")) {
            char pid_buf[32];
            int pid_val = 0;
            if (copy_node_text(cur, pid_buf, sizeof(pid_buf)) &&
                mw_parse_i32(pid_buf, &pid_val) == MW_OK && pid_val > 0) {
                out_report->analyzed_pid = (pid_t)pid_val;
            }
        } else if (xml_node_is_element(cur, "args")) {
            parse_args_node(cur, out_report);
        } else if (xml_node_is_element(cur, "error")) {
            mw_status_t st = parse_error_node(cur, out_report);
            if (st != MW_OK) {
                return st;
            }
        }
    }

    if (!seen_protocol) {
        return MW_ERR_PARSE;
    }

    return MW_OK;
}

mw_status_t mw_parse_valgrind_xml_file(const char *xml_path, mw_leak_report_t *out_report) {
    if (xml_path == NULL || xml_path[0] == '\0' || out_report == NULL) {
        return MW_ERR_INVALID_ARG;
    }

    LIBXML_TEST_VERSION;
    int options = XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING;
    xmlDocPtr doc = xmlReadFile(xml_path, NULL, options);
    if (doc == NULL) {
        return MW_ERR_PARSE;
    }

    mw_strlcpy(out_report->xml_path, xml_path, sizeof(out_report->xml_path));
    mw_status_t st = parse_valgrind_xml_doc(doc, out_report);
    xmlFreeDoc(doc);
    return st;
}

mw_status_t mw_parse_valgrind_xml_memory(const char *xml_buf, size_t len, mw_leak_report_t *out_report) {
    if (xml_buf == NULL || len == 0 || out_report == NULL) {
        return MW_ERR_INVALID_ARG;
    }

    LIBXML_TEST_VERSION;
    int options = XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING;
    xmlDocPtr doc = xmlReadMemory(xml_buf, (int)len, "memory.xml", NULL, options);
    if (doc == NULL) {
        return MW_ERR_PARSE;
    }

    mw_status_t st = parse_valgrind_xml_doc(doc, out_report);
    xmlFreeDoc(doc);
    return st;
}
