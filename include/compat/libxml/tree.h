#ifndef MW_COMPAT_LIBXML_TREE_H
#define MW_COMPAT_LIBXML_TREE_H

/*
 * Minimal ABI-compatible libxml2 declarations for environments where
 * runtime libxml2.so.2 is installed without the libxml2-dev header package.
 * When pkg-config --cflags libxml-2.0 is available, system <libxml/tree.h>
 * takes precedence.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char xmlChar;

#define BAD_CAST (xmlChar *)

typedef enum {
    XML_ELEMENT_NODE       = 1,
    XML_ATTRIBUTE_NODE     = 2,
    XML_TEXT_NODE          = 3,
    XML_CDATA_SECTION_NODE = 4,
    XML_ENTITY_REF_NODE    = 5,
    XML_ENTITY_NODE        = 6,
    XML_PI_NODE            = 7,
    XML_COMMENT_NODE       = 8,
    XML_DOCUMENT_NODE      = 9
} xmlElementType;

typedef struct _xmlNode xmlNode;
typedef xmlNode *xmlNodePtr;

typedef struct _xmlDoc xmlDoc;
typedef xmlDoc *xmlDocPtr;

struct _xmlNode {
    void           *_private;
    xmlElementType  type;
    const xmlChar  *name;
    struct _xmlNode *children;
    struct _xmlNode *last;
    struct _xmlNode *parent;
    struct _xmlNode *next;
    struct _xmlNode *prev;
    struct _xmlDoc  *doc;
    void            *ns;
    xmlChar         *content;
    void            *properties;
    void            *nsDef;
    void            *psvi;
    unsigned short   line;
    unsigned short   extra;
};

xmlNodePtr xmlDocGetRootElement(const xmlDoc *doc);
xmlChar   *xmlNodeGetContent(const xmlNode *cur);
xmlChar   *xmlGetProp(const xmlNode *node, const xmlChar *name);
void       xmlFreeDoc(xmlDocPtr cur);

extern void (*xmlFree)(void *mem);

#ifdef __cplusplus
}
#endif

#endif /* MW_COMPAT_LIBXML_TREE_H */
