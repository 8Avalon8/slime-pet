#include "cc_track.h"

#include <stdio.h>
#include <string.h>

/* A turn can go quiet for a long time (a 10-minute build), but an interrupted turn sends no
 * Stop, so busy states eventually expire back to idle. */
#define THINK_EXPIRE_S (10 * 60)
#define BUSY_EXPIRE_S (30 * 60)
#define FORGET_S (4 * 3600)

void cc_init(cc_t *c) { memset(c, 0, sizeof(*c)); }

static cc_session_t *find_or_add(cc_t *c, uint32_t sid)
{
    cc_session_t *free_slot = NULL, *oldest = &c->s[0];
    for (int i = 0; i < CC_MAX_SESSIONS; i++) {
        cc_session_t *s = &c->s[i];
        if (s->used && s->sid == sid) return s;
        if (!s->used && !free_slot) free_slot = s;
        if (s->last < oldest->last) oldest = s;
    }
    cc_session_t *s = free_slot ? free_slot : oldest;
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->sid = sid;
    return s;
}

static void copy_detail(char *dst, const char *src)
{
    size_t n = 0;
    for (; *src && n < CC_DETAIL_LEN - 1; src++) {
        const unsigned char ch = (unsigned char)*src;
        if (ch == '\r' || ch == '\n') break;
        dst[n++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
    }
    while (n && dst[n - 1] == ' ') n--;
    dst[n] = 0;
}

bool cc_apply(cc_t *c, const char *line, double now, cc_reaction_t *out)
{
    memset(out, 0, sizeof(*out));
    unsigned sid = 0, ts = 0;
    char ev[16];
    int off = 0;
    if (sscanf(line, "cc %8x %u %15s %n", &sid, &ts, ev, &off) != 3) {
        if (!strncmp(line, "cc ", 3)) c->lines_bad++;
        return false;
    }
    char d[CC_DETAIL_LEN];
    copy_detail(d, off > 0 ? line + off : "");

    uint32_t hash = 2166136261u; /* FNV-1a */
    for (const char *p = line; *p; p++) hash = (hash ^ (unsigned char)*p) * 16777619u;

    cc_session_t *s = find_or_add(c, sid);
    const bool fresh = s->last == 0 && s->last_ts == 0;
    if (!fresh && ((int32_t)(ts - s->last_ts) < 0 || (ts == s->last_ts && hash == s->last_hash))) {
        c->lines_stale++;
        return false;
    }
    s->last_ts = ts;
    s->last_hash = hash;
    s->last = now;
    c->lines_ok++;

    if (!strcmp(ev, "start")) {
        s->st = CC_IDLE;
        s->tools = 0;
        s->detail[0] = 0;
        out->fx = CC_FX_HELLO;
    } else if (!strcmp(ev, "prompt")) {
        s->st = CC_THINK;
        s->tools = 0;
        s->detail[0] = 0;
    } else if (!strcmp(ev, "tool")) {
        s->st = CC_WORK;
        s->tools++;
        strcpy(s->detail, d);
    } else if (!strcmp(ev, "tool_ok") || !strcmp(ev, "denied")) {
        if (s->st != CC_IDLE) s->st = CC_THINK;
    } else if (!strcmp(ev, "tool_fail")) {
        if (s->st != CC_IDLE) s->st = CC_THINK;
        out->fx = CC_FX_FAIL;
        strcpy(out->detail, d);
    } else if (!strcmp(ev, "ask")) {
        if (s->st != CC_WAIT) out->fx = CC_FX_ASK;
        s->st = CC_WAIT;
        if (d[0]) strcpy(s->detail, d);
        strcpy(out->detail, s->detail);
    } else if (!strcmp(ev, "stop")) {
        if (s->st != CC_IDLE) {
            out->fx = CC_FX_DONE;
            out->tools = s->tools;
        }
        s->st = CC_IDLE;
        s->detail[0] = 0;
    } else if (!strcmp(ev, "fail")) {
        s->st = CC_IDLE;
        out->fx = CC_FX_FAIL;
        strcpy(out->detail, d);
    } else if (!strcmp(ev, "interrupt")) {
        s->st = CC_IDLE;
        s->detail[0] = 0;
    } else if (!strcmp(ev, "compact")) {
        out->fx = CC_FX_COMPACT;
    } else if (!strcmp(ev, "end")) {
        s->used = false;
    } else {
        c->lines_bad++;
        return false;
    }
    return true;
}

cc_status_t cc_status(cc_t *c, double now, const char **detail, int *n_busy)
{
    cc_status_t best = CC_IDLE;
    const cc_session_t *bs = NULL;
    int busy = 0;
    for (int i = 0; i < CC_MAX_SESSIONS; i++) {
        cc_session_t *s = &c->s[i];
        if (!s->used) continue;
        const double age = now - s->last;
        if (age > FORGET_S) {
            s->used = false;
            continue;
        }
        if (s->st != CC_IDLE && age > (s->st == CC_THINK ? THINK_EXPIRE_S : BUSY_EXPIRE_S)) s->st = CC_IDLE;
        if (s->st == CC_IDLE) continue;
        busy++;
        if (s->st > best || (s->st == best && bs && s->last > bs->last)) {
            best = s->st;
            bs = s;
        }
    }
    if (detail) *detail = bs ? bs->detail : "";
    if (n_busy) *n_busy = busy;
    return best;
}
