#include "vision.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"
#include "linux/videodev2.h"
#include "bsp/subboard.h"
#include "mosaico_module_camera.h"
#include "mosaico_module_mgr.h"
#include "sensors.h"

static const char *TAG = "vision";

/* The camera faces the user, so its picture is the mirror image of what the slime "sees".
 * Flip if the eyes look away from you instead of at you. */
#define MIRROR_X 1
#define FACE_PERIOD_MS 400  /* face detection rate while nobody is there (~2.5 Hz) */
#define FACE_FAST_MS 120    /* ... and while a face is (nods and shakes are ~2 Hz) */
#define FACE_FAST_HOLD_MS 2500
#define MOTION_PERIOD_MS 70 /* wave detection sampling (~14 Hz) */
#define RETRY_MS 5000
#define MIN_INTERNAL_FREE (20 * 1024) /* below this the LCD DMA starts failing: give the camera up */
#define ALIGN 64

#define FACE_SCALE 0.25f   /* 720x1280 (rotated) -> 180x320 for the detector */
#define TINY_SCALE 0.125f /* -> 90x160 grey for frame differencing (1/16 gives odd widths) */
#define TINY_MAX (160 * 160)

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vision_state_t s_st;
static volatile bool s_enabled = true;
/* latest detector input, for the web panel's debug snapshot */
static uint8_t *s_snap;
static int s_snap_w, s_snap_h;
static SemaphoreHandle_t s_snap_mutex;
/* live preview (camera view mode): double-buffered RGB565, mirrored like a mirror */
#define PREVIEW_SCALE 0.375f /* 720x1280 (rotated) -> 270x480: fills the screen height */
#define PIP_SCALE 0.125f     /* -> 90x160 thumbnail */
static volatile int s_preview;
static uint16_t *s_pv[2];
static size_t s_pv_size;
static int s_pv_front = -1, s_pv_w, s_pv_h;
static volatile uint32_t s_pv_seq;

/* One axis of head motion (yaw for shakes, pitch for nods). */
typedef struct {
    float ext;   /* resting value, then the running extremum of the current swing */
    int dir;     /* +1 rising, -1 falling, 0 at rest */
    int flips;
    uint32_t start_t, first_t, last_t; /* swing left rest / first reversal / last event */
    float amp;   /* biggest swing so far */
} swing_t;

/* Head signal filter: one-sample spike hold, light low-pass, running noise level. */
typedef struct {
    float f, spike, noise;
    bool init, held;
} axis_f;

typedef struct {
    mosaico_camera_handle_t cam;
    mosaico_camera_handle_t zombie; /* a camera whose delete failed (unplugged mid-stream): retried */
    int slot_fails;                 /* consecutive attempts with an I2C error on the left slot */
    ppa_client_handle_t ppa;
    HumanFaceDetect *model;
    uint8_t *face_buf, *tiny_buf;
    size_t face_size, tiny_size;
    uint8_t prev[TINY_MAX];
    bool has_prev;
    /* wave tracker */
    uint32_t w_first_t, w_last_t, w_cool_until;
    float w_first_x, w_last_x;
    int w_samples;
    /* cover detector */
    float luma_avg, luma_prev;
    uint32_t dark_since;
    bool covered;
    uint32_t last_face_ms; /* waves are off while a face is in view: a head shake looks just like one */
    swing_t sw_yaw, sw_pitch; /* nod / shake trackers */
    axis_f ax_yaw, ax_nod; /* head signal filters */
    /* head motion: the face's region (raw picture coords 0..1) from the last detection, and its
     * row/column brightness profiles from the previous frame */
    float roi[4];
    uint32_t roi_ms;
    float prof_r[160], prof_c[160];
    int prof_rn, prof_cn;
    bool prof_ok;
    float pos_x, pos_y; /* accumulated motion, in face widths / heights (slowly leaking to 0) */
    float kp_yaw, kp_nod; /* latest keypoint signals, for the trace only */
    uint32_t head_cool_until;
} ctx_t;

static size_t align_up(size_t v) { return (v + ALIGN - 1) & ~(size_t)(ALIGN - 1); }

static void set_err(esp_err_t e)
{
    portENTER_CRITICAL(&s_lock);
    s_st.ok = false;
    s_st.err = e;
    s_st.face = false;
    s_st.faces = 0;
    portEXIT_CRITICAL(&s_lock);
}

/* keep_model: retries only reopen the camera; the model stays loaded (it is the PSRAM-hungry part) */
static void teardown(ctx_t *c, bool keep_model)
{
    if (c->cam) {
        mosaico_camera_stop_stream(c->cam);
        mosaico_camera_close(c->cam);
        /* if this fails the slot stays claimed and no later attempt could ever get it back */
        if (mosaico_camera_del(c->cam) != ESP_OK) c->zombie = c->cam;
        c->cam = NULL;
    }
    if (c->ppa) {
        ppa_unregister_client(c->ppa);
        c->ppa = NULL;
    }
    heap_caps_free(c->face_buf);
    heap_caps_free(c->tiny_buf);
    c->face_buf = c->tiny_buf = NULL;
    if (!keep_model) {
        delete c->model;
        c->model = NULL;
    }
    c->has_prev = false;
    c->w_samples = 0;
}

/* Snapshot of the left slot, and a nudge when it does not see the camera: a fresh scan, and a
 * bus recovery if the probes keep failing (plugging a module in can leave the I2C bus stuck). */
static void check_slot(ctx_t *c, esp_err_t attempt)
{
    mosaico_module_mgr_info_t m = {};
    const bool ok = mosaico_module_mgr_get_info(MOSAICO_MODULE_MGR_SLOT_LEFT, &m) == ESP_OK;
    portENTER_CRITICAL(&s_lock);
    s_st.slot_presence = ok ? (int)m.presence : -1;
    s_st.slot_desc = ok ? (int)m.descriptor_state : -1;
    s_st.slot_owner = ok ? (int)m.owner_state : -1;
    s_st.slot_type = ok && m.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID ? (int)m.eeprom.board_type : -1;
    s_st.slot_err = ok ? m.last_error : ESP_FAIL;
    portEXIT_CRITICAL(&s_lock);
    if (attempt == ESP_OK || !ok) {
        c->slot_fails = 0;
        return;
    }
    mosaico_module_mgr_request_rescan(MOSAICO_MODULE_MGR_SLOT_LEFT);
    const bool bus_error = m.last_error != ESP_OK && m.last_error != ESP_ERR_NOT_FOUND;
    if (bus_error && ++c->slot_fails >= 3) {
        c->slot_fails = 0;
        const esp_err_t r = i2c_master_bus_reset(bsp_subboard_get_i2c_bus());
        ESP_LOGW(TAG, "left slot probe keeps failing (%s): I2C bus reset %s", esp_err_to_name(m.last_error), esp_err_to_name(r));
        portENTER_CRITICAL(&s_lock);
        s_st.bus_resets++;
        portEXIT_CRITICAL(&s_lock);
    }
    ESP_LOGI(TAG, "left slot: presence %d descriptor %d owner %d type %d err %s", (int)m.presence, (int)m.descriptor_state,
             (int)m.owner_state, s_st.slot_type, esp_err_to_name(m.last_error));
}

static esp_err_t bring_up(ctx_t *c)
{
    if (c->zombie) { /* finish releasing the last one first */
        if (mosaico_camera_del(c->zombie) != ESP_OK) return ESP_ERR_INVALID_STATE;
        c->zombie = NULL;
    }
    /* model first: camera buffers would otherwise fragment PSRAM (BSP ai_model_gallery note) */
    if (!c->model) c->model = new HumanFaceDetect(static_cast<HumanFaceDetect::model_type_t>(CONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL), false);
    ESP_RETURN_ON_FALSE(c->model, ESP_ERR_NO_MEM, TAG, "face model");

    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    mosaico_camera_config_t cfg = MOSAICO_CAMERA_DEFAULT_CONFIG();
    cfg.buffer_count = 2;
    ESP_RETURN_ON_ERROR(mosaico_camera_new(&cfg, &c->cam), TAG, "camera new");
    ESP_RETURN_ON_ERROR(mosaico_camera_open(c->cam), TAG, "camera open");
    mosaico_camera_info_t info = {};
    ESP_RETURN_ON_ERROR(mosaico_camera_get_info(c->cam, &info), TAG, "camera info");
    ESP_RETURN_ON_FALSE(info.pixel_format == V4L2_PIX_FMT_UYVY, ESP_ERR_NOT_SUPPORTED, TAG, "camera format");

    c->face_size = align_up((size_t)(info.width * FACE_SCALE + 1) * (size_t)(info.height * FACE_SCALE + 1) * 3);
    c->tiny_size = align_up((size_t)(info.width * TINY_SCALE + 1) * (size_t)(info.height * TINY_SCALE + 1) * 3);
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    c->face_buf = (uint8_t *)heap_caps_aligned_calloc(ALIGN, 1, c->face_size, caps);
    c->tiny_buf = (uint8_t *)heap_caps_aligned_calloc(ALIGN, 1, c->tiny_size, caps);
    ESP_RETURN_ON_FALSE(c->face_buf && c->tiny_buf, ESP_ERR_NO_MEM, TAG, "buffers");

    ppa_client_config_t pc = {};
    pc.oper_type = PPA_OPERATION_SRM;
    pc.max_pending_trans_num = 1;
    ESP_RETURN_ON_ERROR(ppa_register_client(&pc, &c->ppa), TAG, "ppa");
    ESP_RETURN_ON_ERROR(mosaico_camera_start_stream(c->cam), TAG, "stream");

    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "camera %" PRIu32 "x%" PRIu32 " @ %" PRIu32 " fps up; internal RAM free %u KB (camera took %d KB)", info.width,
             info.height, info.frame_rate, (unsigned)(internal / 1024), (int)((long)before - (long)internal) / 1024);
    ESP_RETURN_ON_FALSE(internal >= MIN_INTERNAL_FREE, ESP_ERR_NO_MEM, TAG, "internal RAM too low for the camera");
    return ESP_OK;
}

/* UYVY frame -> rotated (upright), scaled BGR888 via the PPA. */
static esp_err_t shrink(ctx_t *c, const mosaico_camera_frame_t *f, float scale, uint8_t *out, size_t out_size, int *w, int *h)
{
    const uint32_t bpl = f->bytes_per_line ? f->bytes_per_line : f->width * 2U;
    const int ow = (int)ceilf(f->height * scale), oh = (int)ceilf(f->width * scale); /* must hold the whole scaled block */
    ESP_RETURN_ON_FALSE((size_t)ow * oh * 3 <= out_size, ESP_ERR_INVALID_SIZE, TAG, "shrink buffer");
    esp_cache_msync((void *)f->data, f->size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    ppa_srm_oper_config_t op = {};
    op.in.buffer = f->data;
    op.in.pic_w = bpl / 2U;
    op.in.pic_h = f->height;
    op.in.block_w = f->width;
    op.in.block_h = f->height;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY;
    op.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    op.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    op.out.buffer = out;
    op.out.buffer_size = out_size;
    op.out.pic_w = ow;
    op.out.pic_h = oh;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90; /* camera is mounted sideways */
    op.scale_x = scale;
    op.scale_y = scale;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t e = ppa_do_scale_rotate_mirror(c->ppa, &op);
    if (e != ESP_OK) {
        static uint32_t logged;
        if (logged++ % 50 == 0) ESP_LOGW(TAG, "ppa %dx%d -> %dx%d: %s", (int)f->width, (int)f->height, ow, oh, esp_err_to_name(e));
        return e;
    }
    esp_cache_msync(out, out_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    *w = ow;
    *h = oh;
    return ESP_OK;
}

/* Reversal counter for one axis: a swing counts once the value comes back `thr` from its
 * extremum. Returns true when there were 2 reversals (there-back-there) within 1.5 s. */
/* need = reversals that make a gesture: a nod is one dip and back (1), a shake is
 * left-right-left (2). A single dip must come back within 0.9 s, or it was just looking down. */
static bool swing_step(swing_t *s, float v, float thr, int need, uint32_t now)
{
    if (s->dir && now - s->last_t > 900) { /* stalled, or a slow turn: back to rest */
        s->dir = 0;
        s->flips = 0;
    }
    if (!s->dir) {
        if (isnan(s->ext)) {
            s->ext = v;
            return false;
        }
        if (v > s->ext + thr) s->dir = 1;
        else if (v < s->ext - thr) s->dir = -1;
        else {
            s->ext += (v - s->ext) * 0.3f; /* follow the resting pose */
            return false;
        }
        s->start_t = s->first_t = s->last_t = now;
        s->ext = v;
        s->amp = thr;
        return false;
    }
    if ((s->dir > 0 && v > s->ext) || (s->dir < 0 && v < s->ext)) {
        s->ext = v; /* still going the same way */
        return false;
    }
    const float back = fabsf(v - s->ext);
    if (back < thr) return false;
    if (!s->flips) s->first_t = now;
    s->flips++;
    s->dir = -s->dir;
    s->ext = v;
    s->last_t = now;
    if (back > s->amp) s->amp = back;
    if (s->flips >= need && (need > 1 ? now - s->first_t < 1500 : now - s->start_t < 900)) {
        s->flips = 0;
        s->dir = 0;
        s->ext = NAN;
        return true;
    }
    return false;
}

static void swing_reset(swing_t *s)
{
    s->dir = 0;
    s->flips = 0;
    s->ext = NAN; /* re-learn the resting pose */
}

/* Sorts the 5 landmarks (upright detector image, pixels) by role, independent of the model's
 * output order: the two highest are the eyes, the two lowest the mouth corners, the middle
 * one the nose. Holds up to ~40 degrees of roll. Output mirrored and normalised. */
static void classify_landmarks(const std::vector<int> &k, int w, int h, float out[10])
{
    int idx[5] = {0, 1, 2, 3, 4};
    for (int i = 1; i < 5; i++) /* insertion sort by y */
        for (int j = i; j > 0 && k[idx[j] * 2 + 1] < k[idx[j - 1] * 2 + 1]; j--) {
            const int t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    float px[5], py[5];
    for (int i = 0; i < 5; i++) {
        const float x = (float)k[idx[i] * 2] / w;
        px[i] = MIRROR_X ? 1 - x : x;
        py[i] = (float)k[idx[i] * 2 + 1] / h;
    }
    /* order the pairs left to right on screen */
    const int e0 = px[0] <= px[1] ? 0 : 1, m0 = px[3] <= px[4] ? 3 : 4;
    const int order[5] = {e0, 1 - e0, 2, m0, 7 - m0};
    for (int i = 0; i < 5; i++) {
        out[i * 2] = px[order[i]];
        out[i * 2 + 1] = py[order[i]];
    }
}

/* Detections come ~7 times a second, and a nod's low point lasts one or two of them, so no
 * median or heavy smoothing (an earlier median-of-3 erased real nods). Instead: a lone jump far
 * from the track is held one sample and only accepted if the next one agrees; then a light
 * low-pass; and the swing threshold follows the measured jitter of a still head. */
static float axis_step(axis_f *a, float x, float spike, bool swinging)
{
    if (!a->init) {
        a->init = true;
        a->f = x;
        a->noise = 0.02f; /* x4 -> a moderate starting threshold; it adapts within seconds */
        return a->f;
    }
    if (fabsf(x - a->f) > spike && !a->held) {
        a->held = true; /* wait for a second opinion */
        a->spike = x;
        return a->f;
    }
    if (a->held && fabsf(x - a->spike) > spike) x = a->f; /* the jump did not repeat: ignore it */
    a->held = false;
    const float prev = a->f;
    a->f += (x - a->f) * 0.75f;
    if (!swinging) a->noise += (fabsf(a->f - prev) - a->noise) * 0.03f;
    return a->f;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* trace of the head signals, for tuning from the web API (/api/head) */
#define TRACE_N 96
static vision_head_sample_t s_trace[TRACE_N];
static uint32_t s_trace_n;

/* Measured (2026-10-02): the 5 landmarks barely move when you nod or shake (the regressor works
 * on a ~48 px face and stays close to an average face), so the keypoint yaw/nod drown in jitter.
 * The face box does move: down/up with a nod, sideways with a shake. Position is measured in face
 * sizes, so it does not matter how close you sit. Keypoint signals stay in the trace. */
static void track_head(ctx_t *c, float bx, float by, float yaw, float nod, uint32_t now)
{
    const float fy = axis_step(&c->ax_yaw, bx, 0.6f, c->sw_yaw.dir != 0);
    const float fn = axis_step(&c->ax_nod, by, 0.6f, c->sw_pitch.dir != 0);
    /* a swing must stand well clear of the still-head jitter, within sane bounds */
    /* floors from the recorded test: real nods swing 0.08-0.12, shakes 0.06-0.08, while settling
     * after a big move wobbles 0.04-0.05 */
    /* ceilings below a real gesture's swing, so flickering light cannot raise the bar past it */
    const float ty = clampf(c->ax_yaw.noise * 4.0f, 0.05f, 0.09f);
    const float tn = clampf(c->ax_nod.noise * 4.0f, 0.07f, 0.10f);
    const bool y = swing_step(&c->sw_yaw, fy, ty, 2, now);
    const bool p = swing_step(&c->sw_pitch, fn, tn, 1, now);
    portENTER_CRITICAL(&s_lock);
    vision_head_sample_t *t = &s_trace[s_trace_n++ % TRACE_N];
    *t = (vision_head_sample_t){now, yaw, nod, bx, by, fy, fn, ty, tn, (uint8_t)((y ? 1 : 0) | (p ? 2 : 0))};
    portEXIT_CRITICAL(&s_lock);
    if ((int32_t)(now - c->head_cool_until) < 0) return;
    sensor_ev_kind_t ev;
    /* compare swings relative to each axis' own threshold: the two signals have different scales */
    const float ry = c->sw_yaw.amp / ty, rp = c->sw_pitch.amp / tn;
    if (y && (!p || ry > rp * 1.2f)) ev = SEV_HEAD_SHAKE;
    else if (p && rp > ry * 0.8f) ev = SEV_NOD; /* nodding twists the head a little too */
    else return;
    ESP_LOGI(TAG, "%s (yaw amp %.2f/%.2f, nod amp %.2f/%.2f)", ev == SEV_NOD ? "nod" : "head shake", c->sw_yaw.amp, ty,
             c->sw_pitch.amp, tn);
    portENTER_CRITICAL(&s_lock);
    s_trace[(s_trace_n - 1) % TRACE_N].flags |= ev == SEV_NOD ? 8 : 4; /* fired */
    portEXIT_CRITICAL(&s_lock);
    sensors_post(ev, 0, 0);
    c->head_cool_until = now + 1500;
    swing_reset(&c->sw_yaw);
    swing_reset(&c->sw_pitch);
}

/* Shift (in samples) of profile b relative to a: content at a[i] is at b[i + shift]. Mean-removed
 * SAD over the overlap, with a parabola through the best three for sub-sample precision. */
static float profile_shift(const float *a, const float *b, int n, int maxs)
{
    float ma = 0, mb = 0;
    for (int i = 0; i < n; i++) {
        ma += a[i];
        mb += b[i];
    }
    ma /= n;
    mb /= n;
    float cost[64];
    int best = 0;
    for (int s = -maxs; s <= maxs; s++) {
        float sum = 0;
        int cnt = 0;
        for (int i = 0; i < n; i++) {
            const int j = i + s;
            if (j < 0 || j >= n) continue;
            sum += fabsf((b[j] - mb) - (a[i] - ma));
            cnt++;
        }
        cost[s + maxs] = cnt ? sum / cnt : 1e9f;
        if (cost[s + maxs] < cost[best]) best = s + maxs;
    }
    float sub = 0;
    if (best > 0 && best < 2 * maxs) {
        const float l = cost[best - 1], m = cost[best], r = cost[best + 1], d = l - 2 * m + r;
        if (d > 1e-6f) sub = 0.5f * (l - r) / d;
    }
    return best - maxs + sub;
}

/* Head motion between consecutive frames inside the face's region: works at the full frame
 * rate and needs no face detection on this frame (detection drops out while you move). */
static void head_motion(ctx_t *c, int w, int h, uint32_t now)
{
    int x0 = (int)(c->roi[0] * w), x1 = (int)(c->roi[2] * w), y0 = (int)(c->roi[1] * h), y1 = (int)(c->roi[3] * h);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > w ? w : x1;
    y1 = y1 > h ? h : y1;
    const int cn = x1 - x0, rn = y1 - y0;
    if (cn < 12 || rn < 12 || cn > 160 || rn > 160) {
        c->prof_ok = false;
        return;
    }
    float pr[160], pc[160];
    memset(pc, 0, sizeof(float) * cn);
    for (int y = 0; y < rn; y++) {
        const uint8_t *row = &c->tiny_buf[((y0 + y) * w + x0) * 3];
        float acc = 0;
        for (int x = 0; x < cn; x++) {
            const float g = (float)((row[x * 3] + 2 * row[x * 3 + 1] + row[x * 3 + 2]) >> 2);
            acc += g;
            pc[x] += g;
        }
        pr[y] = acc / cn;
    }
    for (int x = 0; x < cn; x++) pc[x] /= rn;
    if (c->prof_ok && c->prof_rn == rn && c->prof_cn == cn) {
        const float dy = profile_shift(c->prof_r, pr, rn, rn / 5 < 31 ? rn / 5 : 31);
        const float dx = profile_shift(c->prof_c, pc, cn, cn / 5 < 31 ? cn / 5 : 31);
        c->pos_x = (c->pos_x + (MIRROR_X ? -dx : dx) / cn) * 0.95f; /* leak: only quick back-and-forth counts */
        c->pos_y = (c->pos_y + dy / rn) * 0.95f;
        track_head(c, c->pos_x, c->pos_y, c->kp_yaw, c->kp_nod, now);
    }
    memcpy(c->prof_r, pr, sizeof(float) * rn);
    memcpy(c->prof_c, pc, sizeof(float) * cn);
    c->prof_rn = rn;
    c->prof_cn = cn;
    c->prof_ok = true;
}

static void detect_faces(ctx_t *c, const mosaico_camera_frame_t *f)
{
    int w, h;
    if (shrink(c, f, FACE_SCALE, c->face_buf, c->face_size, &w, &h) != ESP_OK) return;
    if (s_snap_mutex && xSemaphoreTake(s_snap_mutex, 0) == pdTRUE) {
        if (!s_snap) s_snap = (uint8_t *)heap_caps_malloc(c->face_size, MALLOC_CAP_SPIRAM);
        if (s_snap) {
            memcpy(s_snap, c->face_buf, (size_t)w * h * 3);
            s_snap_w = w;
            s_snap_h = h;
        }
        xSemaphoreGive(s_snap_mutex);
    }
    dl::image::img_t img = {};
    img.data = c->face_buf;
    img.width = (uint16_t)w;
    img.height = (uint16_t)h;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_BGR888;
    const int64_t t0 = esp_timer_get_time();
    auto &res = c->model->run(img);
    const float ms = (esp_timer_get_time() - t0) / 1000.0f;

    int n = 0;
    float best = 0, fx = 0, fy = 0, fsize = 0, rx = 0, ry = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    float kp[10] = {};
    bool kp_ok = false;
    for (const auto &r : res) {
        n++;
        const float bw = (float)(r.box[2] - r.box[0]);
        if (bw > best) { /* the biggest face is the nearest one */
            best = bw;
            kp_ok = r.keypoint.size() >= 10;
            if (kp_ok) classify_landmarks(r.keypoint, w, h, kp);
            fx = ((r.box[0] + r.box[2]) * 0.5f / w) * 2 - 1;
            fy = ((r.box[1] + r.box[3]) * 0.5f / h) * 2 - 1;
            fsize = bw / w;
            b0 = (float)r.box[0] / w;
            b1 = (float)r.box[1] / h;
            b2 = (float)r.box[2] / w;
            b3 = (float)r.box[3] / h;
        }
    }
    rx = fx; /* raw, before mirroring: for checking MIRROR_X / rotation on the device */
    ry = fy;
    if (MIRROR_X) fx = -fx;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    float roll = 0, yaw = 0, pitch = 0, nod = 0;
    if (kp_ok) {
        /* in true pixel units: the detector image is not square */
        const float ex = (kp[2] - kp[0]) * w, ey = (kp[3] - kp[1]) * h;
        const float ed = sqrtf(ex * ex + ey * ey);
        kp_ok = ed > 4; /* degenerate landmarks */
        if (kp_ok) {
            roll = atan2f(ey, ex);
            const float mx = (kp[0] + kp[2]) * 0.5f * w, my = (kp[1] + kp[3]) * 0.5f * h;
            const float nx = kp[4] * w - mx, ny = kp[5] * h - my;
            /* project onto the eye line and its normal, so roll does not leak into yaw/pitch */
            const float ux = ex / ed, uy = ey / ed;
            yaw = (nx * ux + ny * uy) / ed;
            pitch = (-nx * uy + ny * ux) / ed;
            /* nod: where the nose sits between the eye line and the mouth line (0 = eyes, 1 = mouth).
             * Tipping the head squashes the face vertically, which this ratio cancels; what is left
             * is the nose sticking out, moving toward the mouth as you look down. */
            const float qx = (kp[6] + kp[8]) * 0.5f * w - mx, qy = (kp[7] + kp[9]) * 0.5f * h - my;
            const float mouth_n = -qx * uy + qy * ux;
            nod = mouth_n > ed * 0.3f ? pitch * ed / mouth_n : pitch;
        }
    }
    const bool gone = !n && now - c->last_face_ms > 700; /* a frame or two lost mid-nod is normal */
    if (n) c->last_face_ms = now;
    if (n && b2 > b0 && b3 > b1) { /* where to watch for head motion */
        const float bw = b2 - b0, ow = c->roi[2] - c->roi[0];
        const bool active = now - c->roi_ms < 1500;
        const float dc = fabsf((b0 + b2) - (c->roi[0] + c->roi[2])) * 0.5f;
        /* keep the region still while tracking: moving it would look like motion itself */
        if (!active || dc > bw * 0.35f || fabsf(bw - ow) > ow * 0.25f) {
            c->roi[0] = b0;
            c->roi[1] = b1;
            c->roi[2] = b2;
            c->roi[3] = b3;
            c->prof_ok = false;
        }
        c->roi_ms = now;
        if (kp_ok) {
            c->kp_yaw = yaw;
            c->kp_nod = nod;
        }
    } else if (gone) {
        swing_reset(&c->sw_yaw);
        swing_reset(&c->sw_pitch);
        c->ax_yaw.init = c->ax_nod.init = false;
    }
    portENTER_CRITICAL(&s_lock);
    s_st.face = n > 0;
    s_st.faces = n;
    if (n) {
        s_st.fx = fx;
        s_st.fy = fy;
        s_st.fsize = fsize;
        s_st.raw_x = rx;
        s_st.raw_y = ry;
        /* box in the mirrored (preview) picture */
        s_st.box[0] = MIRROR_X ? 1 - b2 : b0;
        s_st.box[1] = b1;
        s_st.box[2] = MIRROR_X ? 1 - b0 : b2;
        s_st.box[3] = b3;
        s_st.face_seq++;
    }
    s_st.kp_ok = kp_ok;
    if (kp_ok) {
        memcpy(s_st.kp, kp, sizeof kp);
        s_st.roll = roll;
        s_st.yaw = yaw;
        s_st.pitch = pitch;
    }
    s_st.infer_ms = ms;
    s_st.fps = s_st.fps * 0.8f + 0.2f * (1000.0f / (now - c->last_face_ms < FACE_FAST_HOLD_MS ? FACE_FAST_MS : FACE_PERIOD_MS));
    portEXIT_CRITICAL(&s_lock);
}

/* Wave: motion alternates between the left and right side of the picture (see below).
 * Cover: the picture goes dark (a hand over the lens) for > 0.5 s; uncover when it comes back. */
static void detect_motion(ctx_t *c, int w, int h, uint32_t now)
{
    uint32_t &first_t = c->w_first_t, &last_t = c->w_last_t, &cool_until = c->w_cool_until;
    float &first_x = c->w_first_x, &last_x = c->w_last_x;
    int &samples = c->w_samples;
    if (w * h > TINY_MAX) return;
    int moving = 0, left = 0, right = 0;
    long sx = 0, sum = 0;
    for (int i = 0; i < w * h; i++) {
        const uint8_t *p = &c->tiny_buf[i * 3];
        const uint8_t g = (uint8_t)((p[0] + 2 * p[1] + p[2]) >> 2);
        sum += g;
        if (c->has_prev && abs(g - c->prev[i]) > 28) {
            const int col = i % w;
            moving++;
            sx += col;
            if (col < w * 2 / 5) left++;
            else if (col >= w * 3 / 5) right++;
        }
        c->prev[i] = g;
    }
    if (MIRROR_X) {
        const int t = left;
        left = right;
        right = t;
    }
    const float luma = (float)sum / (w * h);
    const bool had = c->has_prev;
    c->has_prev = true;
    if (!c->luma_avg) c->luma_avg = luma;

    /* cover / uncover, against the slow average of a normal picture */
    if (!c->covered) {
        if (luma < c->luma_avg * 0.5f) { /* auto-exposure brightens a covered lens back up within ~0.5 s */
            if (!c->dark_since) c->dark_since = now | 1;
            if (now - c->dark_since > 150) {
                c->covered = true;
                sensors_post(SEV_COVER, 0, 0);
                ESP_LOGI(TAG, "lens covered (luma %.0f vs %.0f)", luma, c->luma_avg);
            }
        } else {
            c->dark_since = 0;
            c->luma_avg += (luma - c->luma_avg) * 0.02f;
        }
    } else if (luma > c->luma_avg * 0.75f && now - c->dark_since > 600) {
        c->covered = false;
        c->dark_since = 0;
        c->has_prev = false; /* the next difference would be the whole picture */
        sensors_post(SEV_UNCOVER, 0, 0);
        ESP_LOGI(TAG, "lens uncovered");
    }

    const float frac = (float)moving / (w * h);
    float x = moving ? (float)sx / moving / w : 0.5f; /* 0..1 */
    if (MIRROR_X) x = 1 - x;
    portENTER_CRITICAL(&s_lock);
    s_st.motion = frac;
    s_st.motion_x = x;
    s_st.luma = luma;
    portEXIT_CRITICAL(&s_lock);
    const bool lighting = fabsf(luma - c->luma_prev) > 25; /* exposure jump, not a hand */
    c->luma_prev = luma;
    if (!had || c->covered) return;
    if (now - c->last_face_ms < 2000) { /* a face in view: any side-to-side motion is the head */
        samples = 0;
        return;
    }
    /* a wave is back-and-forth: the busier side flips left <-> right. (A single sweep's
     * centroid barely moves: the hand's old and new positions both show up as motion.) */
    if (frac < 0.05f || lighting || left + right == 0) return;
    const int dom = abs(left - right) < (left + right) * 0.3f ? 0 : (right > left ? 1 : -1);
    if (!dom) return;
    if (now - last_t > 1500) samples = 0; /* stale: start over */
    if (!samples) {
        first_t = now;
        first_x = (float)dom;
        last_x = (float)dom;
        samples = 1;
    } else if ((float)dom != last_x) {
        samples++; /* one more flip */
        last_x = (float)dom;
    }
    last_t = now;
    if (samples >= 3 && now - first_t < 1500 && (int32_t)(now - cool_until) >= 0) {
        sensors_post(last_x > 0 ? SEV_WAVE_R : SEV_WAVE_L, 0, 0);
        ESP_LOGI(TAG, "wave (%d flips in %u ms)", samples - 1, (unsigned)(now - first_t));
        cool_until = now + 1500;
        samples = 0;
    }
}

static void render_preview(ctx_t *c, const mosaico_camera_frame_t *f)
{
    const float scale = s_preview == VISION_PV_PIP ? PIP_SCALE : PREVIEW_SCALE;
    const int ow = (int)ceilf(f->height * scale), oh = (int)ceilf(f->width * scale);
    const size_t need = (size_t)ow * oh * 2;
    if (!s_pv[0]) { /* sized for the full view, whichever mode comes first */
        const size_t full = (size_t)ceilf(f->height * PREVIEW_SCALE) * (size_t)ceilf(f->width * PREVIEW_SCALE) * 2;
        s_pv_size = (full + ALIGN - 1) & ~(size_t)(ALIGN - 1);
        for (int i = 0; i < 2; i++) {
            s_pv[i] = (uint16_t *)heap_caps_aligned_calloc(ALIGN, 1, s_pv_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        }
        if (!s_pv[0] || !s_pv[1]) return;
    }
    if (need > s_pv_size) return;
    const int back = s_pv_front == 0 ? 1 : 0;
    const uint32_t bpl = f->bytes_per_line ? f->bytes_per_line : f->width * 2U;
    esp_cache_msync((void *)f->data, f->size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    ppa_srm_oper_config_t op = {};
    op.in.buffer = f->data;
    op.in.pic_w = bpl / 2U;
    op.in.pic_h = f->height;
    op.in.block_w = f->width;
    op.in.block_h = f->height;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY;
    op.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    op.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    op.out.buffer = s_pv[back];
    op.out.buffer_size = s_pv_size;
    op.out.pic_w = ow;
    op.out.pic_h = oh;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
    op.scale_x = scale;
    op.scale_y = scale;
    op.mirror_x = MIRROR_X;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    if (ppa_do_scale_rotate_mirror(c->ppa, &op) != ESP_OK) return;
    esp_cache_msync(s_pv[back], s_pv_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
    s_pv_front = back;
    s_pv_w = ow;
    s_pv_h = oh;
    s_pv_seq = s_pv_seq + 1;
    xSemaphoreGive(s_snap_mutex);
}

static void vision_task(void *arg)
{
    /* PSRAM: internal RAM is reserved for LCD DMA */
    ctx_t &c = *static_cast<ctx_t *>(heap_caps_calloc(1, sizeof(ctx_t), MALLOC_CAP_SPIRAM));
    swing_reset(&c.sw_yaw);
    swing_reset(&c.sw_pitch);
    uint32_t next_face = 0, next_motion = 0;
    for (;;) {
        if (!s_enabled) {
            if (c.cam || c.model) teardown(&c, false);
            portENTER_CRITICAL(&s_lock);
            s_st.ok = s_st.face = false;
            portEXIT_CRITICAL(&s_lock);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!c.cam) {
            portENTER_CRITICAL(&s_lock);
            s_st.tries++;
            portEXIT_CRITICAL(&s_lock);
            const esp_err_t e = bring_up(&c);
            check_slot(&c, e);
            if (e != ESP_OK) {
                teardown(&c, e != ESP_ERR_NO_MEM);
                set_err(e);
                if (e == ESP_ERR_NO_MEM) { /* not worth retrying: it would starve the display */
                    ESP_LOGW(TAG, "camera disabled until restart: not enough internal RAM");
                    s_enabled = false;
                }
                vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
                continue;
            }
            portENTER_CRITICAL(&s_lock);
            s_st.ok = true;
            s_st.err = ESP_OK;
            portEXIT_CRITICAL(&s_lock);
        }
        mosaico_camera_frame_t f = {};
        const esp_err_t ge = mosaico_camera_get_frame(c.cam, &f);
        if (ge != ESP_OK) { /* unplugged or stalled: start over */
            ESP_LOGW(TAG, "frame: %s", esp_err_to_name(ge));
            teardown(&c, true);
            set_err(ge);
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            continue;
        }
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        bool face_frame = false;
        if ((int32_t)(now - next_face) >= 0) {
            next_face = now + (now - c.last_face_ms < FACE_FAST_HOLD_MS ? FACE_FAST_MS : FACE_PERIOD_MS);
            detect_faces(&c, &f);
            face_frame = true;
        } else {
            /* the thumbnail only has to show whether you are in frame: 5 fps spares the PSRAM bandwidth
             * the renderer needs; the full camera view gets every frame */
            static uint32_t next_pip;
            if (s_preview == VISION_PV_FULL) render_preview(&c, &f);
            else if (s_preview == VISION_PV_PIP && (int32_t)(now - next_pip) >= 0) {
                next_pip = now + 200;
                render_preview(&c, &f);
            }
        }
        /* small grey picture: the wave/cover detector, and head motion on every frame */
        const bool head_on = now - c.roi_ms < 1500;
        const bool motion_due = !face_frame && (int32_t)(now - next_motion) >= 0;
        int tw, th;
        if ((head_on || motion_due) && shrink(&c, &f, TINY_SCALE, c.tiny_buf, c.tiny_size, &tw, &th) == ESP_OK) {
            if (motion_due) {
                next_motion = now + MOTION_PERIOD_MS;
                detect_motion(&c, tw, th, now);
            }
            if (head_on) head_motion(&c, tw, th, now);
            else c.prof_ok = false;
        }
        mosaico_camera_return_frame(c.cam, &f);
        vTaskDelay(1); /* let the idle task run */
    }
}

extern "C" void vision_start(void)
{
    s_snap_mutex = xSemaphoreCreateMutex();
    /* core 1 at the lowest priority: it soaks up the time the render worker leaves idle */
    if (xTaskCreatePinnedToCoreWithCaps(vision_task, "vision", 8192, NULL, 1, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "vision task");
    }
}

extern "C" void vision_enable(bool on)
{
    s_enabled = on;
    portENTER_CRITICAL(&s_lock);
    s_st.enabled = on;
    portEXIT_CRITICAL(&s_lock);
}

extern "C" void vision_get(vision_state_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    out->enabled = s_enabled;
    portEXIT_CRITICAL(&s_lock);
}

extern "C" bool vision_snapshot(uint8_t *dst, size_t cap, int *w, int *h)
{
    bool ok = false;
    if (s_snap_mutex && xSemaphoreTake(s_snap_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        const size_t n = (size_t)s_snap_w * s_snap_h * 3;
        if (s_snap && n && n <= cap) {
            memcpy(dst, s_snap, n);
            *w = s_snap_w;
            *h = s_snap_h;
            ok = true;
        }
        xSemaphoreGive(s_snap_mutex);
    }
    return ok;
}

extern "C" int vision_head_trace(vision_head_sample_t *out, int max, uint32_t since)
{
    int n = 0;
    portENTER_CRITICAL(&s_lock);
    const uint32_t first = s_trace_n > TRACE_N ? s_trace_n - TRACE_N : 0;
    for (uint32_t i = first; i < s_trace_n && n < max; i++)
        if (s_trace[i % TRACE_N].t > since) out[n++] = s_trace[i % TRACE_N];
    portEXIT_CRITICAL(&s_lock);
    return n;
}

extern "C" void vision_set_preview(int mode) { s_preview = mode; }

extern "C" bool vision_preview_copy(uint16_t *dst, int stride, int x0, int max_w, int max_h, uint32_t *seq, int *w, int *h)
{
    bool ok = false;
    if (!s_snap_mutex || xSemaphoreTake(s_snap_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    if (s_pv_front >= 0 && s_pv_seq != *seq && s_pv_w <= max_w && s_pv_h <= max_h) {
        const uint16_t *src = s_pv[s_pv_front];
        for (int y = 0; y < s_pv_h; y++) memcpy(dst + (size_t)y * stride + x0, src + (size_t)y * s_pv_w, s_pv_w * 2);
        *seq = s_pv_seq;
        *w = s_pv_w;
        *h = s_pv_h;
        ok = true;
    }
    xSemaphoreGive(s_snap_mutex);
    return ok;
}
