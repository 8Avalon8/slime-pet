#include "settings_ui.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "slime_menu.h"

#define IDLE_CLOSE_S 30

enum { I_HELP, I_DEMO, I_FOCUS, I_FOCUSMIN, I_NIGHTS, I_NIGHTE, I_SCREEN, I_SLEEPB, I_LED, I_SLEEPM, I_VOL, I_CLAP, I_SOUND, I_BLIP, I_BREATH, I_MOTOR, I_MIC, I_DANCE, I_TILT, I_FPS,
       I_CAMERA, I_SIT, I_CAMVIEW, I_WIFI, I_BGM, I_BGMPLAY, I_CAMPIP, I_AICOMMENT, I_VOICE, I_COUNT };

static sl_menu_item_t s_items[I_COUNT] = {
    [I_HELP] = {"玩法说明", SL_MI_ACTION},
    [I_DEMO] = {"功能演示", SL_MI_ACTION},
    [I_FOCUS] = {"开始 / 结束专注", SL_MI_ACTION},
    [I_FOCUSMIN] = {"专注时长", SL_MI_NUM, 0, 5, 90, 5, "分"},
    [I_NIGHTS] = {"勿扰开始", SL_MI_NUM, 0, 0, 23, 1, "点"},
    [I_NIGHTE] = {"勿扰结束", SL_MI_NUM, 0, 0, 23, 1, "点"},
    [I_SCREEN] = {"屏幕亮度", SL_MI_NUM, 0, 10, 100, 10, "%"},
    [I_SLEEPB] = {"睡觉时亮度", SL_MI_NUM, 0, 0, 100, 5, "%"},
    [I_LED] = {"彩灯亮度", SL_MI_NUM, 0, 0, 100, 10, "%"},
    [I_SLEEPM] = {"自动睡眠", SL_MI_NUM, 0, 5, 60, 5, "分"},
    [I_VOL] = {"音量", SL_MI_NUM, 0, 0, 100, 10, "%"},
    [I_CLAP] = {"拍手灵敏度", SL_MI_NUM, 0, 1, 10, 1, ""},
    [I_SOUND] = {"音效", SL_MI_BOOL},
    [I_BLIP] = {"对话打字音", SL_MI_BOOL},
    [I_BREATH] = {"空闲呼吸灯", SL_MI_BOOL},
    [I_MOTOR] = {"震动", SL_MI_BOOL},
    [I_MIC] = {"麦克风", SL_MI_BOOL},
    [I_DANCE] = {"跟着声音晃", SL_MI_BOOL},
    [I_TILT] = {"倾斜滑动", SL_MI_BOOL},
    [I_FPS] = {"显示帧率", SL_MI_BOOL},
    [I_CAMERA] = {"摄像头", SL_MI_BOOL},
    [I_SIT] = {"久坐提醒", SL_MI_NUM, 0, 0, 120, 10, "分"},
    [I_CAMVIEW] = {"摄像头画面", SL_MI_ACTION},
    [I_WIFI] = {"Wi-Fi", SL_MI_INFO},
    [I_BGM] = {"空闲时哼歌", SL_MI_BOOL},
    [I_BGMPLAY] = {"现在唱一首", SL_MI_ACTION},
    [I_CAMPIP] = {"摄像头小窗常开", SL_MI_BOOL},
    [I_AICOMMENT] = {"AI 点评", SL_MI_BOOL},
    [I_VOICE] = {"长按 AI 键说话", SL_MI_BOOL},
};

static sl_menu_t s_menu = {.items = s_items, .n = I_COUNT, .flash_row = -1};
static bool s_open, s_dirty;
static int s_action = -1;
static double s_last_tap;

static void from_cfg(const slime_cfg_t *c)
{
    s_items[I_FOCUSMIN].value = c->focus_min;
    s_items[I_NIGHTS].value = c->night_start;
    s_items[I_NIGHTE].value = c->night_end;
    s_items[I_SCREEN].value = c->screen_bright;
    s_items[I_SLEEPB].value = c->sleep_bright;
    s_items[I_LED].value = (c->led_bright * 100 + 127) / 255;
    s_items[I_SLEEPM].value = c->sleep_min < 5 ? 5 : c->sleep_min;
    s_items[I_VOL].value = c->volume;
    s_items[I_CLAP].value = c->clap_sens;
    s_items[I_SOUND].value = c->sound;
    s_items[I_BLIP].value = c->text_blip;
    s_items[I_BREATH].value = c->idle_breath;
    s_items[I_MOTOR].value = c->motor;
    s_items[I_MIC].value = c->mic;
    s_items[I_DANCE].value = c->dance;
    s_items[I_TILT].value = c->tilt;
    s_items[I_FPS].value = c->show_fps;
    s_items[I_CAMERA].value = c->camera;
    s_items[I_SIT].value = c->sit_min;
    s_items[I_BGM].value = c->bgm;
    s_items[I_CAMPIP].value = c->cam_pip;
    s_items[I_AICOMMENT].value = c->ai_comment;
    s_items[I_VOICE].value = c->voice;
}

static void to_cfg(slime_cfg_t *c)
{
    c->focus_min = s_items[I_FOCUSMIN].value;
    c->night_start = s_items[I_NIGHTS].value;
    c->night_end = s_items[I_NIGHTE].value;
    c->screen_bright = s_items[I_SCREEN].value;
    c->sleep_bright = s_items[I_SLEEPB].value;
    c->led_bright = s_items[I_LED].value * 255 / 100;
    c->sleep_min = s_items[I_SLEEPM].value;
    c->volume = s_items[I_VOL].value;
    c->clap_sens = s_items[I_CLAP].value;
    c->sound = s_items[I_SOUND].value;
    c->text_blip = s_items[I_BLIP].value;
    c->idle_breath = s_items[I_BREATH].value;
    c->motor = s_items[I_MOTOR].value;
    c->mic = s_items[I_MIC].value;
    c->dance = s_items[I_DANCE].value;
    c->tilt = s_items[I_TILT].value;
    c->show_fps = s_items[I_FPS].value;
    c->camera = s_items[I_CAMERA].value;
    c->sit_min = s_items[I_SIT].value;
    c->bgm = s_items[I_BGM].value;
    c->cam_pip = s_items[I_CAMPIP].value;
    c->ai_comment = s_items[I_AICOMMENT].value;
    c->voice = s_items[I_VOICE].value;
}

void sui_open(const char *wifi, const char *addr, int lv, int exp, int need, double now)
{
    slime_cfg_t c;
    cfg_get(&c);
    from_cfg(&c);
    strlcpy(s_items[I_WIFI].text, addr[0] && addr[0] != '-' ? addr : wifi, sizeof s_items[I_WIFI].text);
    (void)lv; /* level is on the HUD already */
    (void)exp;
    (void)need;
    s_menu.page = 0;
    s_menu.flash_row = -1;
    s_open = s_dirty = true;
    s_last_tap = now;
}

bool sui_active(void) { return s_open; }
bool sui_dirty(void) { return s_open && s_dirty; }

bool sui_tap(int x, int y, double now)
{
    s_last_tap = now;
    int changed = -1;
    const sl_menu_action_t act = sl_menu_tap(&s_menu, x, y, &changed);
    if (act != SL_MA_NONE) s_dirty = true; /* page flips and value changes must repaint */
    switch (act) {
    case SL_MA_CHANGED: {
        slime_cfg_t c;
        cfg_get(&c);
        to_cfg(&c);
        cfg_set_ex(&c, false); /* live preview; saved to flash on close */
        return true;
    }
    case SL_MA_PAGE:
        return true;
    case SL_MA_CLOSE:
        sui_close();
        return true;
    case SL_MA_ACTION:
        if (changed == I_CAMVIEW) s_action = SUI_ACT_CAMVIEW;
        else if (changed == I_BGMPLAY) s_action = SUI_ACT_BGM;
        else if (changed == I_FOCUS) s_action = SUI_ACT_FOCUS;
        else if (changed == I_HELP) s_action = SUI_ACT_HELP;
        else if (changed == I_DEMO) s_action = SUI_ACT_DEMO;
        sui_close();
        return true;
    default:
        return false;
    }
}

bool sui_tick(double now)
{
    if (s_open && now - s_last_tap > IDLE_CLOSE_S) {
        sui_close();
        return true;
    }
    if (s_open && s_menu.flash_row >= 0 && now - s_last_tap > 0.25) { /* fade the tap highlight */
        s_menu.flash_row = -1;
        s_dirty = true;
    }
    return false;
}

void sui_close(void)
{
    if (!s_open) return;
    s_open = false;
    slime_cfg_t c;
    cfg_get(&c);
    cfg_set_ex(&c, true);
}

void sui_render(sg_canvas_t *cv)
{
    sl_menu_render(cv, &s_menu, "设置");
    s_dirty = false;
}

int sui_take_action(void)
{
    const int a = s_action;
    s_action = -1;
    return a;
}
