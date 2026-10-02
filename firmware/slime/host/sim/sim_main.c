/*
 * Slime simulator: the real firmware (main/main.c and friends) on a computer.
 *
 *   ./slime_sim [--port 8080] [--bind 127.0.0.1] [--nvs FILE] [--scale 1|2] [--headless] [--quiet]
 *               [--exit-after SECS [--screenshot FILE.bmp]]
 *
 * The window is the 480x480 panel with the Interaction module's LEDs below it. The mouse is
 * the touch screen; keys are listed at start-up. The pet's HTTP API (web panel, /api/cmd for the
 * Claude Code hook) is on --port, plus /sim/input and /sim/screen.bmp for scripted tests.
 * Without SDL2 (or with --headless) there is no window: drive it over HTTP or stdin.
 */
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "sim.h"

#ifdef SIM_SDL
#include <SDL.h>
#endif

sim_opt_t sim_opt = {.port = 8080, .bind = "127.0.0.1"};

static void *app_thread(void *arg)
{
    app_main();
    return NULL;
}

static void usage(void)
{
    fprintf(stderr, "usage: slime_sim [--port N] [--bind ADDR] [--nvs FILE] [--scale 1|2] [--headless] [--quiet]\n"
                    "                 [--exit-after SECS [--screenshot FILE.bmp]]\n");
    exit(2);
}

#ifdef SIM_SDL
static const char KEYS[] =
    "window keys:\n"
    "  mouse            touch (click = poke, hold 1 s = settings)\n"
    "  A / shift+A      AI button: click (mute) / long press (help)\n"
    "  B / shift+B      BOOT button: click (focus timer) / long press (settings)\n"
    "  Z / X            Interaction module left / right button\n"
    "  S                shake          F   face down on/off\n"
    "  left / right     hold to tilt   shift+left / right   bump\n"
    "  C / shift+C      clap / double clap      M  motion (PIR)     L  lights off/on\n"
    "  N / H / W        nod / shake head / wave (camera gestures)\n"
    "  P                battery: 90% -> 10% -> charging -> no gauge\n"
    "  F12              screenshot into the current directory      Q / Esc  quit\n";

static void cmd(const char *c)
{
    char err[96];
    if (!sim_input(c, err, sizeof err)) fprintf(stderr, "sim: %s\n", err);
}

static int run_window(int scale)
{
    if (SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL: %s; running headless\n", SDL_GetError());
        return -1;
    }
    enum { STRIP = 28 };
    const int W = SIM_SCREEN * scale, H = (SIM_SCREEN + STRIP) * scale;
    SDL_Window *win = SDL_CreateWindow("Slime simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, SIM_SCREEN, SIM_SCREEN) : NULL;
    if (!tex) {
        fprintf(stderr, "SDL: %s; running headless\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
    SDL_RenderSetLogicalSize(ren, SIM_SCREEN, SIM_SCREEN + STRIP);
    fputs(KEYS, stderr);
    static uint16_t px[SIM_SCREEN * SIM_SCREEN];
    uint32_t shown = UINT32_MAX;
    int bat = 0, shots = 0;
    bool face = false, dark = false, mouse = false;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return 0;
            if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) && e.button.button == SDL_BUTTON_LEFT) {
                mouse = e.type == SDL_MOUSEBUTTONDOWN && e.button.y < SIM_SCREEN; /* logical coordinates */
                sim_touch(mouse, e.button.x, e.button.y);
            } else if (e.type == SDL_MOUSEMOTION && mouse) {
                sim_touch(true, e.motion.x, e.motion.y);
            } else if (e.type == SDL_KEYUP && (e.key.keysym.sym == SDLK_LEFT || e.key.keysym.sym == SDLK_RIGHT)) {
                cmd("tilt 0");
            } else if (e.type == SDL_KEYDOWN) {
                const bool sh = e.key.keysym.mod & KMOD_SHIFT;
                const bool rep = e.key.repeat;
                switch (e.key.keysym.sym) {
                case SDLK_q:
                case SDLK_ESCAPE: return 0;
                case SDLK_a: if (!rep) cmd(sh ? "ai_long" : "ai"); break;
                case SDLK_b: if (!rep) cmd(sh ? "boot_long" : "boot"); break;
                case SDLK_z: if (!rep) cmd("left"); break;
                case SDLK_x: if (!rep) cmd("right"); break;
                case SDLK_s: if (!rep) cmd("shake"); break;
                case SDLK_f: if (!rep) cmd((face = !face) ? "facedown 1" : "facedown 0"); break;
                case SDLK_LEFT: if (sh) { if (!rep) cmd("bump L"); } else cmd("tilt -0.5"); break;
                case SDLK_RIGHT: if (sh) { if (!rep) cmd("bump R"); } else cmd("tilt 0.5"); break;
                case SDLK_c: if (!rep) cmd(sh ? "double_clap" : "clap"); break;
                case SDLK_m: if (!rep) cmd("motion"); break;
                case SDLK_l: if (!rep) cmd((dark = !dark) ? "light 0" : "light 60"); break;
                case SDLK_n: if (!rep) cmd("nod"); break;
                case SDLK_h: if (!rep) cmd("head_shake"); break;
                case SDLK_w: if (!rep) cmd("wave R"); break;
                case SDLK_p: {
                    static const char *const B[] = {"battery 10", "battery 50 300", "battery off", "battery 90"};
                    if (!rep) cmd(B[bat++ % 4]);
                    break;
                }
                case SDLK_F12: {
                    char path[64];
                    snprintf(path, sizeof path, "slime_sim_%d.bmp", ++shots);
                    if (sim_screenshot(path)) fprintf(stderr, "saved %s\n", path);
                    break;
                }
                default: break;
                }
            }
        }
        const uint32_t n = sim_screen_copy(px);
        if (n != shown) {
            SDL_UpdateTexture(tex, NULL, px, SIM_SCREEN * 2);
            shown = n;
        }
        const int k = sim_screen_brightness() * 255 / 100;
        SDL_SetTextureColorMod(tex, (Uint8)k, (Uint8)k, (Uint8)k);
        SDL_SetRenderDrawColor(ren, 18, 18, 22, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, &(SDL_Rect){0, 0, SIM_SCREEN, SIM_SCREEN});
        uint8_t c[3];
        sim_mood_rgb(sim_led_mood(), esp_timer_get_time() / 1e6, c);
        for (int i = 0; i < 6; i++) { /* the Interaction module's 6 LEDs */
            SDL_SetRenderDrawColor(ren, c[0], c[1], c[2], 255);
            SDL_RenderFillRect(ren, &(SDL_Rect){SIM_SCREEN / 2 - 150 + i * 54 + 16, SIM_SCREEN + 8, 22, 12});
        }
        SDL_RenderPresent(ren);
        SDL_Delay(8);
    }
}
#endif

int main(int argc, char **argv)
{
    int scale = 1;
    double exit_after = 0;
    const char *shot = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const bool more = i + 1 < argc;
        if (!strcmp(a, "--port") && more) sim_opt.port = atoi(argv[++i]);
        else if (!strcmp(a, "--bind") && more) sim_opt.bind = argv[++i];
        else if (!strcmp(a, "--nvs") && more) sim_opt.nvs_path = argv[++i];
        else if (!strcmp(a, "--scale") && more) scale = atoi(argv[++i]) > 1 ? 2 : 1;
        else if (!strcmp(a, "--exit-after") && more) exit_after = atof(argv[++i]);
        else if (!strcmp(a, "--screenshot") && more) shot = argv[++i];
        else if (!strcmp(a, "--headless")) sim_opt.headless = true;
        else if (!strcmp(a, "--quiet")) sim_opt.quiet = true;
        else usage();
    }
    sim_time_start();
    srand((unsigned)time(NULL));
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16 << 20);
    pthread_t th;
    pthread_create(&th, &at, app_thread, NULL);
    sim_stdin_start();
    fprintf(stderr, "slime simulator: web panel http://%s:%d/   hook: SLIME_HOST=%s:%d   inputs: GET /sim/input\n",
            sim_opt.bind, sim_opt.port, sim_opt.bind, sim_opt.port);

    if (exit_after > 0) { /* scripted run: let it play, optionally take a picture, quit */
        sim_sleep_ms((uint32_t)(exit_after * 1000));
        if (shot && !sim_screenshot(shot)) return 1;
        return 0;
    }
#ifdef SIM_SDL
    if (!sim_opt.headless && run_window(scale) >= 0) return 0;
#endif
    for (;;) sim_sleep_ms(1000);
}
