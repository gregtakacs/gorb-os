#pragma once
/**
 * orb_json — the OS's streaming JSON tokenizer (D51, plan §43).
 *
 * A pull reader over one contiguous buffer: each call answers the next event — a
 * container opening or closing, or a scalar — with the member key it sits under
 * (empty inside an array), its depth, and its value. Nothing is allocated and nothing
 * is kept but the cursor and a small container stack, so the same reader runs over a
 * 200 KB network body in PSRAM or a 700 B buffer. Apps keep the schema (which keys
 * they want and what they mean); the OS keeps the lexing, natively.
 *
 * Pure C with no IDF include, so tools/host_json_test.py compiles it as is.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    ORB_JSON_END = 0,        /* the document closed (or the buffer ran out cleanly) */
    ORB_JSON_OBJ, ORB_JSON_OBJ_END, ORB_JSON_ARR, ORB_JSON_ARR_END,
    ORB_JSON_STR, ORB_JSON_NUM, ORB_JSON_TRUE, ORB_JSON_FALSE, ORB_JSON_NULL,
    ORB_JSON_ERROR = -1      /* malformed: every later call answers this again */
};

#define ORB_JSON_DEPTH   16
#define ORB_JSON_KEY_MAX 31
#define ORB_JSON_STR_MAX 95

typedef struct {
    int32_t type;                    /* ORB_JSON_* */
    int32_t depth;                   /* containers open around this event (an opening
                                      * event counts its own container's parent) */
    double  num;                     /* NUM: the value */
    uint8_t key_trunc, str_trunc;    /* the text did not fit and was cut */
    char    key[ORB_JSON_KEY_MAX + 1];
    char    str[ORB_JSON_STR_MAX + 1];   /* STR: the unescaped value (UTF-8 kept) */
} orb_json_tok_t;

typedef struct {
    const char *p, *end;
    int         depth;
    char        stack[ORB_JSON_DEPTH];   /* '{' or '[' per open level */
    bool        want_key[ORB_JSON_DEPTH];/* inside an object, the next string is a key */
    bool        first[ORB_JSON_DEPTH];   /* no element read yet at this level */
    bool        error, done;
} orb_json_t;

void    orb_json_begin(orb_json_t *j, const char *buf, size_t len);
/** The next event into *t; answers its type. */
int32_t orb_json_next(orb_json_t *j, orb_json_tok_t *t);
/** Skip to just past the container the last OBJ/ARR event opened. */
int32_t orb_json_skip(orb_json_t *j);
