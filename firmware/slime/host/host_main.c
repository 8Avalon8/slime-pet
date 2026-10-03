/*
 * Host harness: runs slime_core on macOS or Linux and writes BMP frames.
 *
 *   host_sim frame <state> <t> <out.bmp> [dialog]  dialog text, "\\n" = second line
 *   host_sim sheet <out.bmp>              13 states, raw poses, 4-column contact sheet
 *   host_sim frame <state> <t> <out.bmp>  one full frame (HUD + dialog), raw pose
 *   host_sim helpers <n> <t> <state> <out.bmp>  n subagent helpers, t s after they were sent out
 *   host_sim bench                        time 600 animated frames with springs
 *   host_sim menu <page> <out.bmp> [flash_row]   settings screen
 *   host_sim scene <dawn|day|dusk|night> <state> <t> <out.bmp>  full frame over a background
 *
 * SLIME_LANG=en renders the English UI.
 */
#define _POSIX_C_SOURCE 200112L /* clock_gettime under -std=c11 on Linux; 2001 so that macOS still declares snprintf */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "slime_anim.h"
#include "slime_bg.h"
#include "slime_render.h"
#include "slime_text.h"
#include "slime_menu.h"

#define S SL_SCREEN

static uint16_t fb[S * S];

static void write_bmp(const char *path, const uint16_t *px, int w, int h, int stride)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    const int row = (w * 3 + 3) & ~3, size = 54 + row * h;
    unsigned char hd[54] = {'B', 'M'};
    *(int *)&hd[2] = size;
    *(int *)&hd[10] = 54;
    *(int *)&hd[14] = 40;
    *(int *)&hd[18] = w;
    *(int *)&hd[22] = -h;
    *(short *)&hd[26] = 1;
    *(short *)&hd[28] = 24;
    fwrite(hd, 1, 54, f);
    unsigned char *line = calloc(1, row);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint16_t v = px[y * stride + x];
            const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            line[3 * x + 0] = (unsigned char)((b << 3) | (b >> 2));
            line[3 * x + 1] = (unsigned char)((g << 2) | (g >> 4));
            line[3 * x + 2] = (unsigned char)((r << 3) | (r >> 2));
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}

static sl_state_t parse_state(const char *n)
{
    for (int i = 0; i < SL_STATE_COUNT; i++) {
        if (!strcmp(n, sl_state_name((sl_state_t)i))) return (sl_state_t)i;
    }
    fprintf(stderr, "unknown state %s\n", n);
    exit(1);
}

static void render_raw(sg_canvas_t *cv, sl_state_t st, float t)
{
    sl_anim_t a;
    sl_anim_init(&a, 0);
    sl_pose_t p;
    sl_anim_pose_raw(&a, st, t, t, 0.016f, &p);
    p.rip = 0;
    sg_fill_rect(cv, 0, 0, S, S, 0);
    sl_render_slime(cv, &p);
    if (p.dim < 1) sg_dim_rect(cv, 0, 0, S, S, p.dim);
}

/* Helper slimes `t` seconds after `n` of them were asked for, around a raw pose at that time. */
static void pose_with_helpers(sl_anim_t *a, sl_pose_t *p, sl_state_t st, int n, float t)
{
    sl_anim_init(a, 0);
    a->state = st;
    a->ext_minis = n;
    for (int i = 0; i <= (int)(t * 60); i++) {
        sl_anim_pose_raw(a, st, i / 60.0f, i / 60.0f, 1 / 60.0f, p);
        sl_anim_minis(a, i / 60.0, p);
    }
    p->rip = 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    sl_render_init();
    const char *lang = getenv("SLIME_LANG");
    if (lang && !strcmp(lang, "en")) sl_lang = SL_LANG_EN;
    if (argc >= 3 && !strcmp(argv[1], "sheet")) {
        static const struct {
            sl_state_t s;
            float t;
        } T[] = {
            {SL_IDLE, 1.0f}, {SL_GREET, 0.33f}, {SL_POKE_L, 0.05f}, {SL_POKE_R, 0.7f}, {SL_DIZZY, 0.4f}, {SL_SLEEP, 1.0f}, {SL_THINK, 1.7f},
            {SL_WAIT, 0.2f}, {SL_LEVELUP, 0.6f}, {SL_HURT, 1.0f}, {SL_CHARGE, 0.5f}, {SL_MELT, 3.0f}, {SL_METAL, 1.2f},
        };
        const int n = sizeof(T) / sizeof(T[0]), cols = 4, rows = (n + cols - 1) / cols;
        uint16_t *sheet = calloc((size_t)S * cols * S * rows, 2);
        for (int i = 0; i < n; i++) {
            sg_canvas_t cv;
            sg_canvas_init(&cv, fb, S, S, S);
            render_raw(&cv, T[i].s, T[i].t);
            const int ox = (i % cols) * S, oy = (i / cols) * S;
            for (int y = 0; y < S; y++) memcpy(&sheet[(oy + y) * S * cols + ox], &fb[y * S], S * 2);
        }
        write_bmp(argv[2], sheet, S * cols, S * rows, S * cols);
        free(sheet);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "frame")) {
        sg_canvas_t cv;
        sg_canvas_init(&cv, fb, S, S, S);
        const sl_state_t st = parse_state(argv[2]);
        const float t = (float)atof(argv[3]);
        render_raw(&cv, st, t);
        sl_anim_t a;
        sl_anim_init(&a, 0);
        a.state = st;
        char msg[128];
        sl_anim_message(&a, msg, sizeof msg);
        if (argc >= 6) { /* optional dialog override; "\n" splits status/detail */
            snprintf(msg, sizeof msg, "%s", argv[5]);
            char *e = strstr(msg, "\\n");
            if (e) { *e = '\n'; memmove(e + 1, e + 2, strlen(e + 2) + 1); }
        }
        if (sl_text_missing(msg, SL_FONT_22)) fprintf(stderr, "missing glyphs in: %s\n", msg);
        sl_render_hud(&cv, a.lv, a.hp);
        sl_render_dialog(&cv, msg, -1, true);
        write_bmp(argv[4], fb, S, S, S);
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "helpers")) {
        sg_canvas_t cv;
        sg_canvas_init(&cv, fb, S, S, S);
        sl_anim_t a;
        sl_pose_t p;
        const int n = atoi(argv[2]);
        pose_with_helpers(&a, &p, parse_state(argv[4]), n, (float)atof(argv[3]));
        sg_fill_rect(&cv, 0, 0, S, S, 0);
        sl_render_slime(&cv, &p);
        if (p.dim < 1) sg_dim_rect(&cv, 0, 0, S, S, p.dim);
        char msg[128];
        snprintf(msg, sizeof msg, "史莱姆和 %d 个分身正在干活！\nTask: explore the repo", n);
        sl_render_hud(&cv, a.lv, a.hp);
        sl_render_dialog(&cv, msg, -1, true);
        write_bmp(argv[5], fb, S, S, S);
        printf("%d helpers drawn\n", p.nmini);
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "scene")) {
        int sc = 0;
        while (sc < SL_SCENE_COUNT && strcmp(argv[2], sl_scene_name((sl_scene_t)sc))) sc++;
        if (sc == SL_SCENE_COUNT) {
            fprintf(stderr, "unknown scene %s\n", argv[2]);
            return 1;
        }
        sg_canvas_t cv;
        sg_canvas_init(&cv, fb, S, S, S);
        sl_bg_draw(&cv, (sl_scene_t)sc);
        const sl_state_t st = parse_state(argv[3]);
        const float t = (float)atof(argv[4]);
        sl_anim_t a;
        sl_anim_init(&a, 0);
        sl_pose_t p;
        sl_anim_pose_raw(&a, st, t, t, 0.016f, &p);
        p.rip = 0;
        sl_render_slime(&cv, &p);
        if (p.dim < 1) sg_dim_rect(&cv, 0, 0, S, S, p.dim);
        a.state = st;
        char msg[128];
        sl_anim_message(&a, msg, sizeof msg);
        sl_render_hud(&cv, a.lv, a.hp);
        sl_render_dialog(&cv, msg, -1, true);
        write_bmp(argv[5], fb, S, S, S);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "menu")) { /* settings screen preview: menu <page> out.bmp */
        sg_canvas_t cv;
        sg_canvas_init(&cv, fb, S, S, S);
        static const char *const LANGS[] = {"中文", "English"};
        sl_menu_item_t it[] = {
            {"语言 / Language", SL_MI_CHOICE, sl_lang, 0, 1, 1, .opts = LANGS},
            {"屏幕亮度", SL_MI_NUM, 100, 10, 100, 10, "%"}, {"睡觉时亮度", SL_MI_NUM, 25, 0, 100, 5, "%"},
            {"彩灯亮度", SL_MI_NUM, 50, 0, 100, 10, "%"}, {"自动睡眠", SL_MI_NUM, 10, 5, 60, 5, "分"},
            {"音量", SL_MI_NUM, 40, 0, 100, 10, "%"}, {"拍手灵敏度", SL_MI_NUM, 5, 1, 10, 1, ""},
            {"音效", SL_MI_BOOL, 1}, {"对话打字音", SL_MI_BOOL, 0}, {"空闲呼吸灯", SL_MI_BOOL, 1},
            {"震动", SL_MI_BOOL, 1}, {"麦克风", SL_MI_BOOL, 1}, {"跟着声音晃", SL_MI_BOOL, 0},
            {"倾斜滑动", SL_MI_BOOL, 1}, {"显示帧率", SL_MI_BOOL, 0}, {"Wi-Fi", SL_MI_INFO},
            {"设置网页", SL_MI_INFO}, {"等级", SL_MI_INFO},
        };
        static const char *const EN[][2] = {
            {"Language / 语言"}, {"Brightness", "%"}, {"Asleep brightness", "%"}, {"LED brightness", "%"}, {"Auto sleep", "m"},
            {"Volume", "%"}, {"Clap sensitivity", ""}, {"Sound effects"}, {"Typing blips"}, {"Breathing LEDs when idle"},
            {"Vibration"}, {"Microphone"}, {"Bob to the sound"}, {"Tilt to slide"}, {"Show FPS"}, {"Wi-Fi"},
            {"Web panel"}, {"Level"},
        };
        const int n = (int)(sizeof it / sizeof it[0]);
        for (int i = 0; i < n && sl_lang == SL_LANG_EN; i++) {
            it[i].label = EN[i][0];
            it[i].unit = EN[i][1];
        }
        snprintf(it[15].text, sizeof it[15].text, "home_2.4G  -39 dBm");
        snprintf(it[16].text, sizeof it[16].text, "http://slime.local");
        snprintf(it[17].text, sizeof it[17].text, "Lv 2  (2/16)");
        sl_menu_t m = {.items = it, .n = n, .page = atoi(argv[2]), .flash_row = argc >= 5 ? atoi(argv[4]) : -1};
        sl_menu_render(&cv, &m, SL_TR("设置", "Settings"));
        write_bmp(argv[3], fb, S, S, S);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "bench")) {
        sg_canvas_t cv;
        sg_canvas_init(&cv, fb, S, S, S);
        sl_anim_t a;
        sl_anim_init(&a, 0);
        sl_pose_t p;
        double total = 0;
        long px = 0;
        const int N = 600;
        for (int i = 0; i < N; i++) {
            const double T = i / 60.0;
            if (i % 120 == 0) sl_anim_set_state(&a, (sl_state_t)((i / 120) % SL_STATE_COUNT), T);
            sl_anim_step(&a, T, 1 / 60.0f, &p);
            const sl_rect_t r = sl_render_bounds(&p);
            sg_fill_rect(&cv, r.x0, r.y0, r.x1, r.y1, 0);
            const double t0 = now_s();
            sl_render_slime(&cv, &p);
            total += now_s() - t0;
            px += (long)(r.x1 - r.x0) * (r.y1 - r.y0);
        }
        printf("avg render %.3f ms/frame, avg dirty area %ld px (%.1f%% of screen)\n", total / N * 1000, px / N,
               100.0 * px / N / (S * S));
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "splitcheck")) {
        /* two band-split canvases drawn one after the other must equal one full render */
        static uint16_t ref[S * S];
        static sg_scratch_t sc[2];
        int bad_frames = 0;
        for (int st = 0; st < SL_STATE_COUNT; st++) {
            for (int k = 0; k < 6; k++) {
                const float t = 0.17f + k * 0.41f;
                sl_anim_t a;
                sl_pose_t p;
                if (k % 2) { /* with helpers: some still in flight, some hopping */
                    pose_with_helpers(&a, &p, (sl_state_t)st, 4, 0.3f + k * 0.1f);
                } else {
                    sl_anim_init(&a, 0);
                    sl_anim_pose_raw(&a, (sl_state_t)st, t, t, 0.016f, &p);
                }
                p.rip = 0.02f;
                sg_canvas_t one;
                sg_canvas_init(&one, ref, S, S, S);
                sg_fill_rect(&one, 0, 0, S, S, 0);
                sl_render_slime(&one, &p);
                memset(fb, 0, sizeof fb);
                for (int o = 0; o < 2; o++) {
                    sg_canvas_t half;
                    sg_canvas_init(&half, fb, S, S, S);
                    sg_canvas_set_split(&half, &sc[o], o);
                    sl_render_slime(&half, &p);
                }
                if (memcmp(ref, fb, sizeof fb)) {
                    int diff = 0;
                    for (int i = 0; i < S * S; i++) diff += ref[i] != fb[i];
                    printf("MISMATCH state=%s t=%.2f: %d px differ\n", sl_state_name((sl_state_t)st), t, diff);
                    bad_frames++;
                }
            }
        }
        printf("splitcheck: %d/%d frames differ\n", bad_frames, SL_STATE_COUNT * 6);
        return bad_frames ? 1 : 0;
    }
    fprintf(stderr, "usage: host_sim sheet out.bmp | frame <state> <t> out.bmp | helpers <n> <t> <state> out.bmp | bench | splitcheck\n");
    return 2;
}
