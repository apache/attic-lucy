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



#define C_LUCY_REGEXTOKENIZER
#define C_LUCY_DOC
#define C_LUCY_DOCREADER
#define C_LUCY_DEFAULTDOCREADER
#define C_LUCY_INVERTER
#define C_LUCY_INVERTERENTRY
#define CFISH_USE_SHORT_NAMES
#define LUCY_USE_SHORT_NAMES



#include <string.h>

#include "charmony.h"

#include "Lucy/Analysis/RegexTokenizer.h"
#include "Lucy/Document/Doc.h"
#include "Lucy/Index/DocReader.h"
#include "Lucy/Index/Inverter.h"
#include "Clownfish/Blob.h"
#include "Clownfish/String.h"
#include "Clownfish/Err.h"
#include "Clownfish/Hash.h"
#include "Clownfish/HashIterator.h"
#include "Clownfish/Num.h"
#include "Clownfish/Vector.h"
#include "Clownfish/Class.h"
#include "Clownfish/Util/Memory.h"
#include "Clownfish/Util/StringHelper.h"
#include "Lucy/Analysis/Token.h"
#include "Lucy/Analysis/Inversion.h"
#include "Lucy/Document/HitDoc.h"
#include "Lucy/Index/Segment.h"
#include "Lucy/Plan/FieldType.h"
#include "Lucy/Plan/Schema.h"
#include "Lucy/Store/InStream.h"
#include "Lucy/Store/OutStream.h"
#include "Lucy/Util/Freezer.h"

#if defined(CHY_HAS_PCRE_H)

#include <pcre.h>

static uint32_t
S_count_code_points(const char *string, size_t len);

bool
RegexTokenizer_is_available(void) {
    return true;
}

RegexTokenizer*
RegexTokenizer_init(RegexTokenizer *self, String *pattern) {
    Analyzer_init((Analyzer*)self);
    RegexTokenizerIVARS *const ivars = RegexTokenizer_IVARS(self);

    char *pattern_buf = NULL;
    const char *pattern_ptr;
    if (pattern) {
        ivars->pattern = Str_Clone(pattern);
        pattern_buf = Str_To_Utf8(ivars->pattern);
        pattern_ptr = pattern_buf;
    }
    else {
        pattern_ptr = "\\w+(?:['\\x{2019}]\\w+)*";
        ivars->pattern
            = Str_new_from_trusted_utf8(pattern_ptr, strlen(pattern_ptr));
    }

    int options = PCRE_UTF8 | PCRE_NO_UTF8_CHECK;
#ifdef PCRE_BSR_UNICODE
    // Available since PCRE 7.4
    options |= PCRE_BSR_UNICODE;
#endif
#ifdef PCRE_NEWLINE_LF
    // Available since PCRE 6.7
    options |= PCRE_NEWLINE_LF;
#endif
    const char *err_ptr;
    int err_offset;
    pcre *re = pcre_compile(pattern_ptr, options, &err_ptr, &err_offset, NULL);
    if (pattern_buf) {
        FREEMEM(pattern_buf);
    }
    if (!re) {
        THROW(ERR, "%s", err_ptr);
    }

    // TODO: Check whether pcre_study improves performance

    ivars->token_re = re;

    return self;
}

void
RegexTokenizer_Destroy_IMP(RegexTokenizer *self) {
    RegexTokenizerIVARS *const ivars = RegexTokenizer_IVARS(self);
    DECREF(ivars->pattern);
    pcre *re = (pcre*)ivars->token_re;
    if (re) {
        pcre_free(re);
    }
    SUPER_DESTROY(self, REGEXTOKENIZER);
}

void
RegexTokenizer_Tokenize_Utf8_IMP(RegexTokenizer *self, const char *string,
                                 size_t string_len, Inversion *inversion) {
    RegexTokenizerIVARS *const ivars = RegexTokenizer_IVARS(self);
    pcre      *re          = (pcre*)ivars->token_re;
    int        byte_offset = 0;
    uint32_t   cp_offset   = 0; // Code points
    int        options     = PCRE_NO_UTF8_CHECK;
    int        ovector[3];

    int return_code = pcre_exec(re, NULL, string, string_len, byte_offset,
                                options, ovector, 3);
    while (return_code >= 0) {
        const char *match     = string + ovector[0];
        size_t      match_len = ovector[1] - ovector[0];

        uint32_t cp_before  = S_count_code_points(string + byte_offset,
                                                  ovector[0] - byte_offset);
        uint32_t cp_start   = cp_offset + cp_before;
        uint32_t cp_matched = S_count_code_points(match, match_len);
        uint32_t cp_end     = cp_start + cp_matched;

        // Add a token to the new inversion.
        Token *token = Token_new(match, match_len, cp_start, cp_end, 1.0f, 1);
        Inversion_Append(inversion, token);

        byte_offset = ovector[1];
        cp_offset   = cp_end;
        return_code = pcre_exec(re, NULL, string, string_len, byte_offset,
                                options, ovector, 3);
    }

    if (return_code != PCRE_ERROR_NOMATCH) {
        THROW(ERR, "pcre_exec failed: %d", return_code);
    }
}

static uint32_t
S_count_code_points(const char *string, size_t len) {
    uint32_t num_code_points = 0;
    size_t i = 0;

    while (i < len) {
        i += StrHelp_UTF8_COUNT[(uint8_t)(string[i])];
        ++num_code_points;
    }

    if (i != len) {
        THROW(ERR, "Match between code point boundaries in '%s'", string);
    }

    return num_code_points;
}

#else // CHY_HAS_PCRE_H

bool
RegexTokenizer_is_available(void) {
    return false;
}

RegexTokenizer*
RegexTokenizer_init(RegexTokenizer *self, String *pattern) {
    UNUSED_VAR(self);
    UNUSED_VAR(pattern);
    THROW(ERR,
          "RegexTokenizer is not available because Lucy was compiled"
          " without PCRE.");
    UNREACHABLE_RETURN(RegexTokenizer*);
}

void
RegexTokenizer_Set_Token_RE_IMP(RegexTokenizer *self, void *token_re) {
    UNUSED_VAR(self);
    UNUSED_VAR(token_re);
    THROW(ERR,
          "RegexTokenizer is not available because Lucy was compiled"
          " without PCRE.");
}

void
RegexTokenizer_Destroy_IMP(RegexTokenizer *self) {
    UNUSED_VAR(self);
    THROW(ERR,
          "RegexTokenizer is not available because Lucy was compiled"
          " without PCRE.");
}

void
RegexTokenizer_Tokenize_Utf8_IMP(RegexTokenizer *self, const char *string,
                                 size_t string_len, Inversion *inversion) {
    UNUSED_VAR(self);
    UNUSED_VAR(string);
    UNUSED_VAR(string_len);
    UNUSED_VAR(inversion);
    THROW(ERR,
          "RegexTokenizer is not available because Lucy was compiled"
          " without PCRE.");
}

#endif // CHY_HAS_PCRE_H

/********************************** Doc ********************************/

Doc*
Doc_init(Doc *self, void *fields, int32_t doc_id) {
    DocIVARS *const ivars = Doc_IVARS(self);
    Hash *hash;

    if (fields) {
        hash = (Hash *)INCREF(CERTIFY(fields, HASH));
    }
    else {
        hash = Hash_new(0);
    }
    ivars->fields = hash;
    ivars->doc_id = doc_id;

    return self;
}

void
Doc_Set_Fields_IMP(Doc *self, void *fields) {
    DocIVARS *const ivars = Doc_IVARS(self);
    DECREF(ivars->fields);
    ivars->fields = CERTIFY(fields, HASH);
}

uint32_t
Doc_Get_Size_IMP(Doc *self) {
    Hash *hash = (Hash*)Doc_IVARS(self)->fields;
    return Hash_Get_Size(hash);
}

void
Doc_Store_IMP(Doc *self, String *field, Obj *value) {
    Hash *hash = (Hash*)Doc_IVARS(self)->fields;
    Hash_Store(hash, field, INCREF(value));
}

void
Doc_Serialize_IMP(Doc *self, OutStream *outstream) {
    DocIVARS *const ivars = Doc_IVARS(self);
    Hash *hash = (Hash*)ivars->fields;
    Freezer_serialize_hash(hash, outstream);
    OutStream_Write_C32(outstream, ivars->doc_id);
}

Doc*
Doc_Deserialize_IMP(Doc *self, InStream *instream) {
    DocIVARS *const ivars = Doc_IVARS(self);
    ivars->fields = Freezer_read_hash(instream);
    ivars->doc_id = InStream_Read_C32(instream);
    return self;
}

Obj*
Doc_Extract_IMP(Doc *self, String *field) {
    Hash *hash = (Hash*)Doc_IVARS(self)->fields;
    return INCREF(Hash_Fetch(hash, field));
}

Hash*
Doc_Dump_IMP(Doc *self) {
    UNUSED_VAR(self);
    THROW(ERR, "TODO");
    UNREACHABLE_RETURN(Hash*);
}

Doc*
Doc_Load_IMP(Doc *self, Obj *dump) {
    UNUSED_VAR(self);
    UNUSED_VAR(dump);
    THROW(ERR, "TODO");
    UNREACHABLE_RETURN(Doc*);
}

bool
Doc_Equals_IMP(Doc *self, Obj *other) {
    if ((Doc*)other == self)   { return true;  }
    if (!Obj_is_a(other, DOC)) { return false; }
    DocIVARS *const ivars = Doc_IVARS(self);
    DocIVARS *const ovars = Doc_IVARS((Doc*)other);
    return Hash_Equals((Hash*)ivars->fields, (Obj*)ovars->fields);
}

void
Doc_Destroy_IMP(Doc *self) {
    DocIVARS *const ivars = Doc_IVARS(self);
    DECREF(ivars->fields);
    SUPER_DESTROY(self, DOC);
}


/**************************** DocReader *****************************/

HitDoc*
DefDocReader_Fetch_Doc_IMP(DefaultDocReader *self, int32_t doc_id) {
    DefaultDocReaderIVARS *const ivars = DefDocReader_IVARS(self);
    Schema   *const schema = ivars->schema;
    InStream *const dat_in = ivars->dat_in;
    InStream *const ix_in  = ivars->ix_in;
    Hash     *const fields = Hash_new(1);
    int64_t   start;
    uint32_t  num_fields;
    uint32_t  field_name_cap = 31;
    char     *field_name = (char*)MALLOCATE(field_name_cap + 1);

    // Get data file pointer from index, read number of fields.
    InStream_Seek(ix_in, (int64_t)doc_id * 8);
    start = InStream_Read_U64(ix_in);
    InStream_Seek(dat_in, start);
    num_fields = InStream_Read_C32(dat_in);

    // Decode stored data and build up the doc field by field.
    while (num_fields--) {
        uint32_t        field_name_len;
        Obj       *value;
        FieldType *type;

        // Read field name.
        field_name_len = InStream_Read_C32(dat_in);
        if (field_name_len > field_name_cap) {
            field_name_cap = field_name_len;
            field_name     = (char*)REALLOCATE(field_name,
                                                    field_name_cap + 1);
        }
        InStream_Read_Bytes(dat_in, field_name, field_name_len);

        // Find the Field's FieldType.
        String *field_name_str = SSTR_WRAP_UTF8(field_name, field_name_len);
        type = Schema_Fetch_Type(schema, field_name_str);

        // Read the field value.
        switch (FType_Primitive_ID(type) & FType_PRIMITIVE_ID_MASK) {
            case FType_TEXT: {
                    uint32_t value_len = InStream_Read_C32(dat_in);
                    char *buf = (char*)MALLOCATE(value_len + 1);
                    InStream_Read_Bytes(dat_in, buf, value_len);
                    buf[value_len] = '\0';
                    value = (Obj*)Str_new_steal_utf8(buf, value_len);
                    break;
                }
            case FType_BLOB: {
                    uint32_t value_len = InStream_Read_C32(dat_in);
                    char *buf = (char*)MALLOCATE(value_len);
                    InStream_Read_Bytes(dat_in, buf, value_len);
                    value = (Obj*)Blob_new_steal(buf, value_len);
                    break;
                }
            case FType_FLOAT32:
                value = (Obj*)Float_new(InStream_Read_F32(dat_in));
                break;
            case FType_FLOAT64:
                value = (Obj*)Float_new(InStream_Read_F64(dat_in));
                break;
            case FType_INT32:
                value = (Obj*)Int_new((int32_t)InStream_Read_C32(dat_in));
                break;
            case FType_INT64:
                value = (Obj*)Int_new((int64_t)InStream_Read_C64(dat_in));
                break;
            default:
                value = NULL;
                THROW(ERR, "Unrecognized type: %o", type);
        }

        // Store the value.
        Hash_Store_Utf8(fields, field_name, field_name_len, value);
    }
    FREEMEM(field_name);

    HitDoc *retval = HitDoc_new(fields, doc_id, 0.0);
    DECREF(fields);
    return retval;
}

/**************************** Inverter *****************************/

static InverterEntry*
S_fetch_entry(InverterIVARS *ivars, String *field) {
    Schema *const schema = ivars->schema;
    int32_t field_num = Seg_Field_Num(ivars->segment, field);
    if (!field_num) {
        // This field seems not to be in the segment yet.  Try to find it in
        // the Schema.
        if (Schema_Fetch_Type(schema, field)) {
            // The field is in the Schema.  Get a field num from the Segment.
            field_num = Seg_Add_Field(ivars->segment, field);
        }
        else {
            // We've truly failed to find the field.  The user must
            // not have spec'd it.
            THROW(ERR, "Unknown field name: '%o'", field);
        }
    }

    InverterEntry *entry
        = (InverterEntry*)Vec_Fetch(ivars->entry_pool, field_num);
    if (!entry) {
        entry = InvEntry_new(schema, (String*)field, field_num);
        Vec_Store(ivars->entry_pool, field_num, (Obj*)entry);
    }
    return entry;
}

void
Inverter_Invert_Doc_IMP(Inverter *self, Doc *doc) {
    InverterIVARS *const ivars = Inverter_IVARS(self);
    Hash *const fields = (Hash*)Doc_Get_Fields(doc);

    // Prepare for the new doc.
    Inverter_Set_Doc(self, doc);

    // Extract and invert the doc's fields.
    HashIterator *iter = HashIter_new(fields);
    while (HashIter_Next(iter)) {
        String *field = HashIter_Get_Key(iter);
        Obj    *obj   = HashIter_Get_Value(iter);

        InverterEntry *inventry = S_fetch_entry(ivars, field);
        InverterEntryIVARS *inventry_ivars = InvEntry_IVARS(inventry);
        FieldType *type = inventry_ivars->type;

        // Get the field value.
        switch (FType_Primitive_ID(type) & FType_PRIMITIVE_ID_MASK) {
            case FType_TEXT: {
                    CERTIFY(obj, STRING);
                    break;
                }
            case FType_BLOB: {
                    CERTIFY(obj, BLOB);
                    break;
                }
            case FType_INT32:
            case FType_INT64: {
                    CERTIFY(obj, INTEGER);
                    break;
                }
            case FType_FLOAT32:
            case FType_FLOAT64: {
                    CERTIFY(obj, FLOAT);
                    break;
                }
            default:
                THROW(ERR, "Unrecognized type: %o", type);
        }

        if (inventry_ivars->value != obj) {
            DECREF(inventry_ivars->value);
            inventry_ivars->value = INCREF(obj);
        }

        Inverter_Add_Field(self, inventry);
    }
    DECREF(iter);
}

