/*
 * Claude Code session tracker. Pure C (no IDF), so host/ can unit-test it.
 *
 * The Mac-side hook (bridge/slime_hook.py) sends one line per hook event:
 *
 *     cc <sid> <ts> <event> [detail]
 *
 * sid    8 hex chars of the Claude Code session id
 * ts     hook start time in ms (mod 2^32); hooks run async, so lines can arrive
 *        out of order and stale ones are dropped per session
 * event  start | prompt | tool | tool_ok | tool_fail | ask | denied | stop | fail | compact | end
 *        | sub | sub_end
 * detail printable ASCII, already shortened by the hook (tool + file/command);
 *        for sub / sub_end: "<agent id, 8 hex> [agent type]"
 *
 * Subagents are tracked by id, so a repeated or late line cannot leak a count: a start after
 * its own end is ignored (the end leaves a tombstone), and sub lines are applied even when they
 * arrive out of order. Tool calls inside a subagent carry the parent session id, so a live
 * subagent keeps its session fresh; one that goes quiet for 10 minutes is dropped.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CC_MAX_SESSIONS 6
#define CC_DETAIL_LEN 48
#define CC_MAX_SUBS 8 /* subagents remembered per session (live ones and tombstones) */

/* Ordered by display priority: the busiest session wins. */
typedef enum { CC_IDLE = 0, CC_THINK, CC_WORK, CC_WAIT } cc_status_t;

/* One-shot reactions for the pet. */
typedef enum { CC_FX_NONE = 0, CC_FX_HELLO, CC_FX_DONE, CC_FX_FAIL, CC_FX_COMPACT, CC_FX_ASK, CC_FX_SPAWN } cc_fx_t;

typedef enum { CC_SUB_FREE = 0, CC_SUB_LIVE, CC_SUB_DONE } cc_sub_st_t;

typedef struct {
    uint32_t id;
    cc_sub_st_t st;
    double t; /* local time of its start / end */
} cc_sub_t;

typedef struct {
    bool used;
    uint32_t sid;
    uint32_t last_ts;
    uint32_t last_hash; /* of the last applied line: drops a duplicate sent over both transports */
    cc_status_t st;
    double last; /* local time of the last applied event */
    int tools;   /* tool calls since the last prompt */
    char detail[CC_DETAIL_LEN];
    cc_sub_t sub[CC_MAX_SUBS];
} cc_session_t;

typedef struct {
    cc_session_t s[CC_MAX_SESSIONS];
    uint32_t lines_ok, lines_bad, lines_stale;
} cc_t;

typedef struct {
    cc_fx_t fx;
    int tools;                  /* CC_FX_DONE: tool calls in the finished turn */
    char detail[CC_DETAIL_LEN]; /* CC_FX_FAIL / CC_FX_ASK / CC_FX_SPAWN: what failed / is asked / agent type */
} cc_reaction_t;

void cc_init(cc_t *c);
/* Apply one line. Returns false for lines that are not "cc ..." or are malformed/stale. */
bool cc_apply(cc_t *c, const char *line, double now, cc_reaction_t *out);
/* Highest-priority status over live sessions (expiring forgotten ones), its detail and the
 * number of sessions that are not idle. */
cc_status_t cc_status(cc_t *c, double now, const char **detail, int *n_busy);
/* Live subagents over all sessions (dropping ones whose session went quiet). */
int cc_subagents(cc_t *c, double now);
/* Live subagents of one session, without expiring anything. */
int cc_session_subs(const cc_session_t *s);

#ifdef __cplusplus
}
#endif
