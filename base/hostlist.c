#include <stdio.h>
#include <string.h>

#include "base/hostlist.h"

/* One pass over the list: count its hosts, or stop at the one wanted. */
struct walk {
    uint32_t want;              /* the index wanted, or UINT32_MAX to count */
    uint32_t seen;              /* hosts before the current one */
    char *out;
    size_t out_len;
    bool found;
    const char *why;
};

/* A run of 1 to 9 decimal digits: the number and its width. */
static bool number(const char *s, size_t n, uint32_t *value, int *width)
{
    if (n == 0 || n > 9)
        return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    *value = v;
    *width = (int)n;
    return true;
}

/* Hand the walk `count` hosts named prefix + (first + k, zero-padded to width)
 * + suffix, or one host named by the entry itself when width is 0. */
static bool hosts(struct walk *w, const char *prefix, size_t prefix_len, uint32_t first,
                  uint32_t count, int width, const char *suffix, size_t suffix_len)
{
    if (w->want != UINT32_MAX && w->want >= w->seen && w->want - w->seen < count) {
        uint32_t k = w->want - w->seen;
        int n = width ? snprintf(w->out, w->out_len, "%.*s%0*u%.*s", (int)prefix_len, prefix,
                                 width, first + k, (int)suffix_len, suffix)
                      : snprintf(w->out, w->out_len, "%.*s", (int)prefix_len, prefix);
        w->found = n >= 0 && (size_t)n < w->out_len;
        if (!w->found)
            w->why = "the host name is too long";
        return false;                   /* stop: found, or it does not fit */
    }
    if (count > UINT32_MAX - w->seen) {
        w->why = "more than 2^32 hosts";
        return false;
    }
    w->seen += count;
    return true;
}

/* One entry of the list, s[0..n). Returns false to stop the walk. */
static bool entry(struct walk *w, const char *s, size_t n)
{
    if (n == 0) {
        w->why = "an empty entry";
        return false;
    }
    const char *open = memchr(s, '[', n);
    if (!open) {
        if (memchr(s, ']', n)) {
            w->why = "a ']' without its '['";
            return false;
        }
        return hosts(w, s, n, 0, 1, 0, "", 0);
    }
    const char *close = memchr(open, ']', (size_t)(s + n - open));
    if (!close) {
        w->why = "a '[' without its ']'";
        return false;
    }
    const char *suffix = close + 1;
    size_t suffix_len = (size_t)(s + n - suffix);
    if (memchr(open + 1, '[', (size_t)(close - open - 1)) || memchr(suffix, '[', suffix_len) ||
        memchr(suffix, ']', suffix_len)) {
        w->why = "more than one bracketed range in a name (multi-dimensional names are "
                 "not supported)";
        return false;
    }
    /* Inside the brackets: numbers and ranges, separated by commas. */
    for (const char *item = open + 1; item < close; ) {
        const char *comma = memchr(item, ',', (size_t)(close - item));
        const char *end = comma ? comma : close;
        const char *dash = memchr(item, '-', (size_t)(end - item));
        uint32_t lo, hi;
        int width, hi_width;
        if (!number(item, (size_t)((dash ? dash : end) - item), &lo, &width) ||
            (dash && !number(dash + 1, (size_t)(end - dash - 1), &hi, &hi_width))) {
            w->why = "a bracket item that is not a number or a range of numbers";
            return false;
        }
        if (!dash)
            hi = lo;
        if (hi < lo) {
            w->why = "a range that runs backwards";
            return false;
        }
        if (!hosts(w, s, (size_t)(open - s), lo, hi - lo + 1, width, suffix, suffix_len))
            return false;
        item = comma ? comma + 1 : close;
        if (comma && item == close) {
            w->why = "a trailing ',' inside brackets";
            return false;
        }
    }
    return true;
}

/* Split the list at commas outside brackets, entry by entry. */
static void walk_list(struct walk *w, const char *list)
{
    const char *start = list;
    int depth = 0;
    for (const char *p = list;; p++) {
        if (*p == '[')
            depth++;
        else if (*p == ']')
            depth--;
        if ((*p == ',' && depth == 0) || *p == '\0') {
            if (!entry(w, start, (size_t)(p - start)) || *p == '\0')
                return;
            start = p + 1;
        }
    }
}

bool osoe_hostlist_count(const char *list, uint32_t *count, char *why, size_t why_len)
{
    struct walk w = { .want = UINT32_MAX };
    walk_list(&w, list);
    if (w.why) {
        snprintf(why, why_len, "%s", w.why);
        return false;
    }
    *count = w.seen;
    return true;
}

bool osoe_hostlist_entry(const char *list, uint32_t index, char *out, size_t out_len)
{
    if (index == UINT32_MAX || out_len == 0)
        return false;
    struct walk w = { .want = index, .out = out, .out_len = out_len };
    walk_list(&w, list);
    return w.found;
}
