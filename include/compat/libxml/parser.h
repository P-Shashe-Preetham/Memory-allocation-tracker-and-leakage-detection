#ifndef MW_COMPAT_LIBXML_PARSER_H
#define MW_COMPAT_LIBXML_PARSER_H

#include <libxml/tree.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XML_PARSE_RECOVER   = 1 << 0,
    XML_PARSE_NOENT     = 1 << 1,
    XML_PARSE_DTDLOAD   = 1 << 2,
    XML_PARSE_DTDATTR   = 1 << 3,
    XML_PARSE_DTDVALID  = 1 << 4,
    XML_PARSE_NOERROR   = 1 << 5,
    XML_PARSE_NOWARNING = 1 << 6,
    XML_PARSE_PEDANTIC  = 1 << 7,
    XML_PARSE_NOBLANKS  = 1 << 8,
    XML_PARSE_NONET     = 1 << 11
} xmlParserOption;

#define LIBXML_TEST_VERSION ((void)0)

xmlDocPtr xmlReadFile(const char *filename, const char *encoding, int options);
xmlDocPtr xmlReadMemory(const char *buffer, int size, const char *URL, const char *encoding, int options);
void      xmlCleanupParser(void);

#ifdef __cplusplus
}
#endif

#endif /* MW_COMPAT_LIBXML_PARSER_H */
