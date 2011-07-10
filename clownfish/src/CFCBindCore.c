/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <stdio.h>

#define CFC_NEED_BASE_STRUCT_DEF
#include "CFCBase.h"
#include "CFCBindCore.h"
#include "CFCBindAliases.h"
#include "CFCBindClass.h"
#include "CFCBindFile.h"
#include "CFCClass.h"
#include "CFCFile.h"
#include "CFCHierarchy.h"
#include "CFCUtil.h"

struct CFCBindCore {
    CFCBase base;
    CFCHierarchy *hierarchy;
    char         *dest;
    char         *header;
    char         *footer;
};

#ifdef _WIN32
#define PATH_SEP "\\"
#define PATH_SEP_CHAR '\\'
#else
#define PATH_SEP "/"
#define PATH_SEP_CHAR '/'
#endif

CFCBindCore*
CFCBindCore_new(CFCHierarchy *hierarchy, const char *dest, const char *header, 
                const char *footer) {
    CFCBindCore *self
        = (CFCBindCore*)CFCBase_allocate(sizeof(CFCBindCore),
                                         "Clownfish::Binding::Core");
    return CFCBindCore_init(self, hierarchy, dest, header, footer);
}

CFCBindCore*
CFCBindCore_init(CFCBindCore *self, CFCHierarchy *hierarchy, const char *dest,
                 const char *header, const char *footer) {
    CFCUTIL_NULL_CHECK(hierarchy);
    CFCUTIL_NULL_CHECK(dest);
    CFCUTIL_NULL_CHECK(header);
    CFCUTIL_NULL_CHECK(footer);
    self->hierarchy = (CFCHierarchy*)CFCBase_incref((CFCBase*)hierarchy);
    self->dest      = CFCUtil_strdup(dest);
    self->header    = CFCUtil_strdup(header);
    self->footer    = CFCUtil_strdup(footer);
    return self;
}

void
CFCBindCore_destroy(CFCBindCore *self) {
    CFCBase_decref((CFCBase*)self->hierarchy);
    FREEMEM(self->dest);
    FREEMEM(self->header);
    FREEMEM(self->footer);
    CFCBase_destroy((CFCBase*)self);
}

int
CFCBindCore_write_all_modified(CFCBindCore *self, int modified) {
    CFCHierarchy *hierarchy = self->hierarchy;
    const char   *dest      = self->dest;
    const char   *header    = self->header;
    const char   *footer    = self->footer;

    // Discover whether files need to be regenerated.
    modified = CFCHierarchy_propagate_modified(hierarchy, modified);

    // Iterate over all File objects, writing out those which don't have
    // up-to-date auto-generated files.
    CFCFile **files = CFCHierarchy_files(hierarchy);
    for (int i = 0; files[i] != NULL; i++) {
        if (CFCFile_get_modified(files[i])) {
            CFCBindFile_write_h(files[i], dest, header, footer);
        }
    }

    // If any class definition has changed, rewrite the parcel.h and parcel.c
    // files.
    if (modified) {
        CFCBindCore_write_parcel_h(self);
        CFCBindCore_write_parcel_c(self);
    }

    return modified;
}

/* Write the "parcel.h" header file, which contains common symbols needed by
 * all classes, plus typedefs for all class structs.
 */
char*
CFCBindCore_write_parcel_h(CFCBindCore *self) {
    CFCHierarchy *hierarchy = self->hierarchy;

    // Declare object structs for all instantiable classes.
    char *typedefs = CFCUtil_strdup("");
    CFCClass **ordered = CFCHierarchy_ordered_classes(hierarchy);
    for (int i = 0; ordered[i] != NULL; i++) {
        CFCClass *klass = ordered[i];
        if (!CFCClass_inert(klass)) {
            const char *full_struct = CFCClass_full_struct_sym(klass);
            typedefs = CFCUtil_cat(typedefs, "typedef struct ", full_struct, 
                                   " ", full_struct, ";\n", NULL);
        }
    }
    FREEMEM(ordered);

    // Create Clownfish aliases if necessary.
    char *aliases = CFCBindAliases_c_aliases();

    const char pattern[] = 
        "%s\n"
        "#ifndef BOIL_H\n"
        "#define BOIL_H 1\n"
        "\n"
        "#ifdef __cplusplus\n"
        "extern \"C\" {\n"
        "#endif\n"
        "\n"
        "#include <stddef.h>\n"
        "#include \"charmony.h\"\n"
        "\n"
        "%s\n"
        "%s\n"
        "\n"
        "/* Refcount / host object */\n"
        "typedef union {\n"
        "    size_t  count;\n"
        "    void   *host_obj;\n"
        "} cfish_ref_t;\n"
        "\n"
        "/* Generic method pointer.\n"
        " */\n"
        "typedef void\n"
        "(*cfish_method_t)(const void *vself);\n"
        "\n"
        "/* Access the function pointer for a given method from the vtable.\n"
        " */\n"
        "#define LUCY_METHOD(_vtable, _class_nick, _meth_name) \\\n"
        "     cfish_method(_vtable, \\\n"
        "     Lucy_ ## _class_nick ## _ ## _meth_name ## _OFFSET)\n"
        "\n"
        "static CHY_INLINE cfish_method_t\n"
        "cfish_method(const void *vtable, size_t offset) {\n"
        "    union { char *cptr; cfish_method_t *fptr; } ptr;\n"
        "    ptr.cptr = (char*)vtable + offset;\n"
        "    return ptr.fptr[0];\n"
        "}\n"
        "\n"
        "/* Access the function pointer for the given method in the superclass's\n"
        " * vtable. */\n"
        "#define LUCY_SUPER_METHOD(_vtable, _class_nick, _meth_name) \\\n"
        "     cfish_super_method(_vtable, \\\n"
        "     Lucy_ ## _class_nick ## _ ## _meth_name ## _OFFSET)\n"
        "\n"
        "extern size_t cfish_VTable_offset_of_parent;\n"
        "static CHY_INLINE cfish_method_t\n"
        "cfish_super_method(const void *vtable, size_t offset) {\n"
        "    char *vt_as_char = (char*)vtable;\n"
        "    cfish_VTable **parent_ptr\n"
        "        = (cfish_VTable**)(vt_as_char + cfish_VTable_offset_of_parent);\n"
        "    return cfish_method(*parent_ptr, offset);\n"
        "}\n"
        "\n"
        "/* Return a boolean indicating whether a method has been overridden.\n"
        " */\n"
        "#define LUCY_OVERRIDDEN(_self, _class_nick, _meth_name, _micro_name) \\\n"
        "        (cfish_method(*((cfish_VTable**)_self), \\\n"
        "            Lucy_ ## _class_nick ## _ ## _meth_name ## _OFFSET )\\\n"
        "            != (cfish_method_t)lucy_ ## _class_nick ## _ ## _micro_name )\n"
        "\n"
        "#ifdef CFISH_USE_SHORT_NAMES\n"
        "  #define METHOD                   LUCY_METHOD\n"
        "  #define SUPER_METHOD             LUCY_SUPER_METHOD\n"
        "  #define OVERRIDDEN               LUCY_OVERRIDDEN\n"
        "#endif\n"
        "\n"
        "typedef struct cfish_Callback {\n"
        "    const char    *name;\n"
        "    size_t         name_len;\n"
        "    cfish_method_t func;\n"
        "    size_t         offset;\n"
        "} cfish_Callback;\n"
        "\n"
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n"
        "\n"
        "#endif /* BOIL_H */\n"
        "\n"
        "%s\n"
        "\n";
    size_t size = sizeof(pattern)
                  + strlen(self->header)
                  + strlen(aliases)
                  + strlen(typedefs)
                  + strlen(self->footer)
                  + 50;
    char *file_content = (char*)MALLOCATE(size);
    sprintf(file_content, pattern, self->header, aliases, typedefs,
            self->footer);

    // Unlink then write file.
    char *filepath = CFCUtil_cat(CFCUtil_strdup(""), self->dest, PATH_SEP, 
                                 "parcel.h", NULL);
    remove(filepath);
    CFCUtil_write_file(filepath, file_content, strlen(file_content));

    FREEMEM(aliases);
    FREEMEM(typedefs);
    FREEMEM(file_content);
}

char*
CFCBindCore_write_parcel_c(CFCBindCore *self) {
    CFCHierarchy *hierarchy = self->hierarchy;

    // Aggregate C code from all files.
    char *content      = CFCUtil_strdup("");
    char *privacy_syms = CFCUtil_strdup("");
    char *includes     = CFCUtil_strdup("");
    CFCFile **files    = CFCHierarchy_files(hierarchy);
    for (int i = 0; files[i] != NULL; i++) {
        CFCFile *file = files[i];
        CFCBase **blocks = CFCFile_blocks(file);
        for (int j = 0; blocks[j] != NULL; j++) {
            const char *cfc_class = CFCBase_get_cfc_class(blocks[j]);
            if (strcmp(cfc_class, "Clownfish::Class") == 0) {
                CFCClass *klass = (CFCClass*)blocks[j];
                
                CFCBindClass *class_binding = CFCBindClass_new(klass);
                char *c_code = CFCBindClass_to_c(class_binding);
                content = CFCUtil_cat(content, c_code, "\n", NULL);
                FREEMEM(c_code);
                CFCBase_decref((CFCBase*)class_binding);
                const char *privacy_sym = CFCClass_privacy_symbol(klass);
                privacy_syms = CFCUtil_cat(privacy_syms, "#define ",
                                           privacy_sym, "\n", NULL);
                const char *include_h = CFCClass_include_h(klass);
                includes = CFCUtil_cat(includes, "#include \"", include_h,
                                       "\"\n", NULL);
            }
        }
    }

    char pattern[] = 
        "%s\n"
        "\n"
        "%s\n"
        "#include \"parcel.h\"\n"
        "%s\n"
        "\n"
        "%s\n"
        "\n"
        "%s\n";
    size_t size = sizeof(pattern)
                  + strlen(self->header)
                  + strlen(privacy_syms)
                  + strlen(includes)
                  + strlen(content)
                  + strlen(self->footer)
                  + 50;
    char *file_content = (char*)MALLOCATE(size);
    sprintf(file_content, pattern, self->header, privacy_syms, includes,
            content, self->footer);

    // Unlink then open file.
    char *filepath = CFCUtil_cat(CFCUtil_strdup(""), self->dest, PATH_SEP, 
                                 "parcel.c", NULL);
    remove(filepath);
    CFCUtil_write_file(filepath, file_content, strlen(file_content));

    FREEMEM(privacy_syms);
    FREEMEM(includes);
    FREEMEM(content);
    FREEMEM(file_content);
}

CFCHierarchy*
CFCBindCore_get_hierarchy(CFCBindCore *self) {
    return self->hierarchy;
}

const char*
CFCBindCore_get_dest(CFCBindCore *self) {
    return self->dest;
}

const char*
CFCBindCore_get_header(CFCBindCore *self) {
    return self->header;
}

const char*
CFCBindCore_get_footer(CFCBindCore *self) {
    return self->footer;
}

