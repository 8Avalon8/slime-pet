/* Unit test for main/cc_track.c: host_sim's sibling, run with `make test`. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cc_track.h"

static cc_t c;
static cc_reaction_t r;

static bool feed(const char *line, double now) { return cc_apply(&c, line, now, &r); }

int main(void)
{
    const char *d;
    int busy;
    cc_init(&c);

    assert(feed("cc 0000abcd 100 start", 1) && r.fx == CC_FX_HELLO);
    assert(cc_status(&c, 1, &d, &busy) == CC_IDLE && busy == 0);

    assert(feed("cc 0000abcd 200 prompt", 2) && r.fx == CC_FX_NONE);
    assert(cc_status(&c, 2, &d, &busy) == CC_THINK && busy == 1);

    assert(feed("cc 0000abcd 300 tool Bash: idf.py build", 3));
    assert(cc_status(&c, 3, &d, NULL) == CC_WORK && !strcmp(d, "Bash: idf.py build"));

    /* async hooks: a late line from before the tool started is dropped */
    assert(!feed("cc 0000abcd 250 tool_ok", 3.1) && c.lines_stale == 1);
    assert(cc_status(&c, 3.1, NULL, NULL) == CC_WORK);

    /* the same line delivered twice (Wi-Fi and USB) counts once */
    assert(feed("cc 0000abcd 350 tool Read: x.c", 3.2) && !feed("cc 0000abcd 350 tool Read: x.c", 3.2) && c.lines_stale == 2);

    assert(feed("cc 0000abcd 400 tool_fail Bash: exit 2", 4) && r.fx == CC_FX_FAIL && !strcmp(r.detail, "Bash: exit 2"));
    assert(cc_status(&c, 4, NULL, NULL) == CC_THINK);

    /* second session asks for permission: WAIT beats THINK */
    assert(feed("cc 12345678 10 prompt", 5));
    assert(feed("cc 12345678 20 tool Edit: main.c", 5));
    assert(feed("cc 12345678 30 ask Edit: main.c", 6) && r.fx == CC_FX_ASK);
    assert(feed("cc 12345678 31 ask", 6) && r.fx == CC_FX_NONE); /* already waiting: no second nudge */
    assert(cc_status(&c, 6, &d, &busy) == CC_WAIT && busy == 2 && !strcmp(d, "Edit: main.c"));
    assert(feed("cc 12345678 40 tool_ok", 7));
    assert(cc_status(&c, 7, NULL, NULL) == CC_THINK);

    /* first session finishes: DONE with its tool count */
    assert(feed("cc 0000abcd 500 tool Read: a.c", 8));
    assert(feed("cc 0000abcd 600 stop", 9) && r.fx == CC_FX_DONE && r.tools == 3);
    assert(feed("cc 0000abcd 601 stop", 9) && r.fx == CC_FX_NONE); /* idle stop: no double reward */

    /* forgotten busy session expires to idle */
    assert(cc_status(&c, 7 + 11 * 60, NULL, &busy) == CC_IDLE && busy == 0);

    /* end removes the session; garbage is rejected */
    assert(feed("cc 12345678 50 end", 900));
    assert(!feed("hello world", 900) && !feed("cc zz", 900) && !feed("cc 00000001 1 bogus", 900));
    assert(feed("cc 0000beef 1 tool caf\xc3\xa9 \xe2\x9c\x93 ok", 901) && cc_status(&c, 901, &d, NULL) == CC_WORK);
    printf("detail sanitized: \"%s\"\n", d);
    assert(!strchr(d, '\xc3'));

    /* subagents: counted by id, idempotent, robust to late and out-of-order lines */
    cc_init(&c);
    assert(feed("cc 0000a001 100 prompt", 1000));
    assert(feed("cc 0000a001 110 sub 1111aaaa Explore", 1000) && r.fx == CC_FX_SPAWN && !strcmp(r.detail, "Explore"));
    assert(feed("cc 0000a001 111 sub 2222bbbb general-purpose", 1000) && r.fx == CC_FX_SPAWN);
    assert(feed("cc 0000a001 112 sub 2222bbbb general-purpose", 1000) && r.fx == CC_FX_NONE); /* repeat */
    assert(cc_subagents(&c, 1000) == 2);
    /* a sub line older than the session's last line still applies, and does not rewind it */
    assert(feed("cc 0000a001 200 tool Task: look around", 1001));
    assert(feed("cc 0000a001 150 sub_end 1111aaaa", 1001) && cc_subagents(&c, 1001) == 1);
    assert(!feed("cc 0000a001 190 tool_ok", 1001)); /* normal lines are still ordered */
    /* a start arriving after its own end stays dead; an end without a start leaves no count */
    assert(feed("cc 0000a001 201 sub_end 3333cccc", 1002) && cc_subagents(&c, 1002) == 1);
    assert(feed("cc 0000a001 202 sub 3333cccc Plan", 1002) && r.fx == CC_FX_NONE && cc_subagents(&c, 1002) == 1);
    assert(feed("cc 0000a001 203 sub_end 2222bbbb", 1003) && cc_subagents(&c, 1003) == 0);
    assert(!feed("cc 0000a001 204 sub", 1003) && !feed("cc 0000a001 205 sub_end zz", 1003)); /* no id */
    /* more than CC_MAX_SUBS live ones: the oldest is evicted, the count stays bounded */
    for (int i = 0; i < CC_MAX_SUBS + 3; i++) {
        char l[64];
        snprintf(l, sizeof l, "cc 0000a001 %d sub %08x", 300 + i, 0xb000 + i);
        assert(feed(l, 1004 + i));
    }
    assert(cc_subagents(&c, 1020) == CC_MAX_SUBS);
    /* other sessions add up; a session that goes quiet drops its subagents */
    assert(feed("cc 0000a002 10 sub 4444dddd", 1100) && cc_subagents(&c, 1100) == CC_MAX_SUBS + 1);
    assert(feed("cc 0000a002 11 tool Read: x", 1100 + 9 * 60)); /* tool calls keep it alive */
    assert(cc_subagents(&c, 1100 + 15 * 60) == 1);
    assert(cc_subagents(&c, 1100 + 20 * 60) == 0);
    /* a new session start clears everything */
    assert(feed("cc 0000a002 20 sub 5555eeee", 3000) && feed("cc 0000a002 30 start", 3001) && cc_subagents(&c, 3001) == 0);
    /* the first line of a session may be a sub line with a large timestamp */
    assert(feed("cc 0000a003 4000000000 sub 6666ffff", 3002) && feed("cc 0000a003 4000000001 prompt", 3002));
    assert(cc_status(&c, 3002, NULL, NULL) == CC_THINK);

    printf("cc_test: all passed (ok=%u bad=%u stale=%u)\n", c.lines_ok, c.lines_bad, c.lines_stale);
    return 0;
}
