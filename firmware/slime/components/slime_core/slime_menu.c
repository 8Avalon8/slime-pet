#include "slime_menu.h"

#include <stdio.h>

#include "slime_text.h"

/* Layout (480x480) */
#define WX 16
#define WY 16
#define WW 448
#define WH 448
#define ROW_Y0 84
#define ROW_H 52
#define MINUS_X0 262
#define VALUE_X0 312
#define PLUS_X0 392
#define BTN_W 50
#define TOGGLE_X0 358
#define TOGGLE_W 88
#define BAR_Y0 404
#define BAR_Y1 450

static const sg_rgb_t WH_ = {1, 1, 1}, BK_ = {0, 0, 0};

int sl_menu_pages(const sl_menu_t *m) { return (m->n + SL_MENU_ROWS - 1) / SL_MENU_ROWS; }

static void button(sg_canvas_t *cv, float x, float y, float w, float h, const char *label, bool enabled)
{
    const sg_rgb_t c = enabled ? WH_ : sg_hex(0x4a5266);
    sg_stroke_round_rect(cv, x, y, w, h, 8, 2.5f, c, 1);
    const int tw = sl_text_width(label, SL_FONT_22);
    sl_text_draw(cv, label, x + (w - tw) / 2, y + h / 2 + 8, SL_FONT_22, c, -1);
}

void sl_menu_render(sg_canvas_t *cv, const sl_menu_t *m, const char *title)
{
    sg_fill_rect(cv, 0, 0, 480, 480, 0);
    sg_fill_round_rect(cv, WX, WY, WW, WH, 10, BK_, 1);
    sg_stroke_round_rect(cv, WX, WY, WW, WH, 10, 3, WH_, 1);
    sl_text_draw(cv, title, 40, 60, SL_FONT_22, WH_, -1);
    char pg[32];
    snprintf(pg, sizeof pg, "%d/%d", m->page + 1, sl_menu_pages(m));
    sl_text_draw(cv, pg, 440 - sl_text_width(pg, SL_FONT_18), 58, SL_FONT_18, sg_hex(0x8d97ad), -1);
    const float sep[4] = {36, 74, 444, 74};
    sg_stroke_poly(cv, sep, 2, false, 1.5f, sg_hex(0x3a4050), 1);

    for (int r = 0; r < SL_MENU_ROWS; r++) {
        const int i = m->page * SL_MENU_ROWS + r;
        if (i >= m->n) break;
        const sl_menu_item_t *it = &m->items[i];
        const float y = ROW_Y0 + r * ROW_H;
        if (r == m->flash_row) sg_fill_round_rect(cv, 30, y + 2, 420, ROW_H - 4, 8, sg_hex(0x1a2440), 1);
        sl_text_draw(cv, it->label, 44, y + 34, SL_FONT_22, WH_, -1);
        char v[48];
        switch (it->kind) {
        case SL_MI_NUM: {
            button(cv, MINUS_X0, y + 7, BTN_W - 6, ROW_H - 14, "-", it->value > it->min);
            button(cv, PLUS_X0, y + 7, BTN_W - 6, ROW_H - 14, "+", it->value < it->max);
            snprintf(v, sizeof v, "%d%s", it->value, it->unit ? it->unit : "");
            const int tw = sl_text_width(v, SL_FONT_22);
            sl_text_draw(cv, v, VALUE_X0 + (PLUS_X0 - VALUE_X0 - 6 - tw) / 2, y + 34, SL_FONT_22, WH_, -1);
            break;
        }
        case SL_MI_BOOL: {
            const bool on = it->value != 0;
            const sg_rgb_t fill = on ? sg_hex(0x2f9e4f) : sg_hex(0x262c3a);
            sg_fill_round_rect(cv, TOGGLE_X0, y + 9, TOGGLE_W, ROW_H - 18, (ROW_H - 18) / 2.0f, fill, 1);
            const float kx = on ? TOGGLE_X0 + TOGGLE_W - (ROW_H - 18) / 2.0f : TOGGLE_X0 + (ROW_H - 18) / 2.0f;
            sg_paint_t knob = sg_solid(WH_);
            sg_fill_ellipse(cv, kx, y + ROW_H / 2.0f, 13, 13, &knob, 1);
            const char *t = on ? "开" : "关";
            sl_text_draw(cv, t, on ? TOGGLE_X0 + 14 : TOGGLE_X0 + TOGGLE_W - 34, y + 34, SL_FONT_18, WH_, -1);
            break;
        }
        case SL_MI_INFO:
            sl_text_draw(cv, it->text, 444 - sl_text_width(it->text, SL_FONT_18), y + 33, SL_FONT_18, sg_hex(0xa9c9ff), -1);
            break;
        case SL_MI_ACTION:
            button(cv, TOGGLE_X0, y + 7, TOGGLE_W, ROW_H - 14, "打开", true);
            break;
        }
    }
    const int pages = sl_menu_pages(m);
    button(cv, 36, BAR_Y0, 126, BAR_Y1 - BAR_Y0, "上一页", m->page > 0);
    button(cv, 177, BAR_Y0, 126, BAR_Y1 - BAR_Y0, "返回", true);
    button(cv, 318, BAR_Y0, 126, BAR_Y1 - BAR_Y0, "下一页", m->page < pages - 1);
}

sl_menu_action_t sl_menu_tap(sl_menu_t *m, int x, int y, int *changed)
{
    if (y >= BAR_Y0 - 6 && y <= BAR_Y1 + 10) {
        if (x >= 177 && x < 303) return SL_MA_CLOSE;
        if (x < 170 && m->page > 0) {
            m->page--;
            return SL_MA_PAGE;
        }
        if (x >= 310 && m->page < sl_menu_pages(m) - 1) {
            m->page++;
            return SL_MA_PAGE;
        }
        return SL_MA_NONE;
    }
    if (y < ROW_Y0 || y >= ROW_Y0 + SL_MENU_ROWS * ROW_H) return SL_MA_NONE;
    const int r = (y - ROW_Y0) / ROW_H, i = m->page * SL_MENU_ROWS + r;
    if (i >= m->n) return SL_MA_NONE;
    sl_menu_item_t *it = &m->items[i];
    int v = it->value;
    if (it->kind == SL_MI_BOOL) {
        v = !v; /* anywhere on the row */
    } else if (it->kind == SL_MI_NUM) {
        if (x >= MINUS_X0 - 10 && x < VALUE_X0) v -= it->step;
        else if (x >= PLUS_X0 - 6) v += it->step;
        else return SL_MA_NONE;
        if (v < it->min) v = it->min;
        if (v > it->max) v = it->max;
    } else if (it->kind == SL_MI_ACTION) {
        m->flash_row = r;
        *changed = i;
        return SL_MA_ACTION;
    } else {
        return SL_MA_NONE;
    }
    m->flash_row = r;
    if (v == it->value) return SL_MA_PAGE; /* redraw the highlight only */
    it->value = v;
    *changed = i;
    return SL_MA_CHANGED;
}
