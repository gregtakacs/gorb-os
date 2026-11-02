/* The streaming JSON tokenizer — contract in orb_json.h. */
#include "orb_json.h"

#include <stdlib.h>
#include <string.h>

void orb_json_begin(orb_json_t *j, const char *buf, size_t len)
{
    memset(j, 0, sizeof *j);
    j->p = buf;
    j->end = buf + len;
}

static int32_t fail(orb_json_t *j) { j->error = true; return ORB_JSON_ERROR; }

static void ws(orb_json_t *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        const char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* One string at j->p (the opening quote), unescaped into out (cap incl. NUL). */
static bool string(orb_json_t *j, char *out, size_t cap, uint8_t *trunc)
{
    size_t n = 0;
    *trunc = 0;
    j->p++;
    while (j->p < j->end && *j->p != '"') {
        unsigned c = (unsigned char)*j->p++;
        if (c < 0x20) return false;                  /* control bytes are not JSON */
        char tmp[4];
        int k = 1;
        tmp[0] = (char)c;
        if (c == '\\') {
            if (j->p >= j->end) return false;
            const char e = *j->p++;
            switch (e) {
            case '"': case '\\': case '/': tmp[0] = e; break;
            case 'b': tmp[0] = '\b'; break;
            case 'f': tmp[0] = '\f'; break;
            case 'n': tmp[0] = '\n'; break;
            case 'r': tmp[0] = '\r'; break;
            case 't': tmp[0] = '\t'; break;
            case 'u': {
                if (j->end - j->p < 4) return false;
                int u = hex4(j->p);
                if (u < 0) return false;
                j->p += 4;
                /* A surrogate half is kept as '?': no app needs astral text. */
                if (u >= 0xD800 && u <= 0xDFFF) u = '?';
                if (u < 0x80) { tmp[0] = (char)u; }
                else if (u < 0x800) { tmp[0] = (char)(0xC0 | u >> 6); tmp[1] = (char)(0x80 | (u & 0x3F)); k = 2; }
                else { tmp[0] = (char)(0xE0 | u >> 12); tmp[1] = (char)(0x80 | ((u >> 6) & 0x3F));
                       tmp[2] = (char)(0x80 | (u & 0x3F)); k = 3; }
                break; }
            default: return false;
            }
        }
        if (n + (size_t)k < cap) { memcpy(out + n, tmp, (size_t)k); n += (size_t)k; }
        else *trunc = 1;
    }
    if (j->p >= j->end) return false;                /* unterminated */
    j->p++;
    if (cap) out[n] = 0;
    return true;
}

static bool literal(orb_json_t *j, const char *w)
{
    const size_t n = strlen(w);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, w, n)) return false;
    j->p += n;
    return true;
}

/* A number at j->p, by the JSON grammar, converted with strtod over a bounded copy
 * (the buffer is not NUL-terminated). */
static bool number(orb_json_t *j, double *out)
{
    const char *s = j->p;
    if (j->p < j->end && *j->p == '-') j->p++;
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') return false;
    while (j->p < j->end && ((*j->p >= '0' && *j->p <= '9') || *j->p == '.' || *j->p == 'e' ||
                             *j->p == 'E' || *j->p == '+' || *j->p == '-'))
        j->p++;
    char b[40];
    const size_t n = (size_t)(j->p - s);
    if (n >= sizeof b) return false;
    memcpy(b, s, n);
    b[n] = 0;
    char *e = NULL;
    *out = strtod(b, &e);
    return e == b + n;
}

int32_t orb_json_next(orb_json_t *j, orb_json_tok_t *t)
{
    memset(t, 0, sizeof *t);
    if (j->error) return ORB_JSON_ERROR;
    if (j->done) return ORB_JSON_END;
    ws(j);
    /* Between members: a comma, or the close of the open container. */
    if (j->depth > 0 && j->p < j->end) {
        const char top = j->stack[j->depth - 1];
        if (*j->p == ',') {
            if (j->first[j->depth - 1]) return fail(j);          /* a leading comma */
            j->p++; ws(j);
            if (top == '{') j->want_key[j->depth - 1] = true;
        } else if (*j->p == (top == '{' ? '}' : ']')) {
            j->p++;
            j->depth--;
            t->depth = j->depth;
            t->type = top == '{' ? ORB_JSON_OBJ_END : ORB_JSON_ARR_END;
            if (j->depth == 0) j->done = true;
            return t->type;
        } else if (!j->first[j->depth - 1]) {
            return fail(j);                                      /* a missing comma */
        }
    }
    if (j->p >= j->end) {
        if (j->depth) return fail(j);
        j->done = true;
        return ORB_JSON_END;
    }
    /* A member's key, then its colon. */
    if (j->depth > 0 && j->stack[j->depth - 1] == '{' && j->want_key[j->depth - 1]) {
        if (*j->p != '"' || !string(j, t->key, sizeof t->key, &t->key_trunc)) return fail(j);
        ws(j);
        if (j->p >= j->end || *j->p != ':') return fail(j);
        j->p++;
        ws(j);
        j->want_key[j->depth - 1] = false;
        if (j->p >= j->end) return fail(j);
    } else if (j->depth > 0 && j->stack[j->depth - 1] == '{') {
        return fail(j);                              /* a value where a key belongs */
    }
    t->depth = j->depth;
    if (j->depth > 0) j->first[j->depth - 1] = false;
    const char c = *j->p;
    if (c == '{' || c == '[') {
        if (j->depth >= ORB_JSON_DEPTH) return fail(j);
        j->p++;
        j->stack[j->depth] = c;
        j->want_key[j->depth] = c == '{';
        j->first[j->depth] = true;
        j->depth++;
        /* An empty container closes on the next call through the branch above, but
         * an empty object would first look for a key: let it see the close. */
        ws(j);
        if (c == '{' && j->p < j->end && *j->p == '}') j->want_key[j->depth - 1] = false;
        return t->type = c == '{' ? ORB_JSON_OBJ : ORB_JSON_ARR;
    }
    if (c == '"') {
        if (!string(j, t->str, sizeof t->str, &t->str_trunc)) return fail(j);
        t->type = ORB_JSON_STR;
    } else if (literal(j, "true"))  t->type = ORB_JSON_TRUE;
    else if (literal(j, "false"))   t->type = ORB_JSON_FALSE;
    else if (literal(j, "null"))    t->type = ORB_JSON_NULL;
    else if (number(j, &t->num))    t->type = ORB_JSON_NUM;
    else return fail(j);
    if (j->depth == 0) j->done = true;               /* a bare scalar document */
    return t->type;
}

int32_t orb_json_skip(orb_json_t *j)
{
    if (j->error) return ORB_JSON_ERROR;
    const int target = j->depth - 1;
    if (target < 0) return ORB_JSON_END;
    orb_json_tok_t t;
    for (;;) {
        const int32_t r = orb_json_next(j, &t);
        if (r == ORB_JSON_ERROR || r == ORB_JSON_END) return r;
        if ((r == ORB_JSON_OBJ_END || r == ORB_JSON_ARR_END) && j->depth == target) return r;
    }
}
