/* The pet's own jingles and background tunes: all original compositions, MIT like the rest of
 * the project. Scores are in the MML dialect described in audio.c; pitch-glide noises use N().
 * To use other tunes on your own device, put the same two tables in main/tunes_local.h
 * (ignored by git): it is picked up instead of this file. */
#pragma once

static const sound_t SOUNDS[SFX_COUNT] = {
    [SFX_LEVELUP] = {.mml = {"t168 @1 v13 q6 o5 l16 c e g >c8 <g8 a b >c d e8 c8 g4. r8 e g c2",
                             "t168 @3 v15 q7 o4 l8 c4 e4 f4 g4 c4. r8 <g4 >c2",
                             "t168 @4 v5 q2 o6 l8 r2 r2 c16 c16 r8 r4 r4 r8 c4"}},
    [SFX_DONE] = {.mml = {"t150 @0 v10 q5 o5 l16 g >c e g8 r16 e g2", "t150 @3 v14 q7 o4 l8 c g >c4 <c2"}},
    [SFX_HURT] = {.glide = {N(420, 110, 180, W_SQ, 80), N(3000, 3000, 70, W_NOISE, 45)}},
    [SFX_ASK] = {.mml = {"t180 @1 v12 q5 o5 l16 e f e f e f g+8 r8 b4", "t180 @3 v14 q4 o4 l16 e e e e e e e8 r8 e4",
                         "t180 @4 v6 q2 o6 l16 c r c r c r c8 r8 c4"}},
    [SFX_POKE] = {.glide = {N(300, 900, 60, W_TRI, 100), N(900, 500, 100, W_TRI, 100)}},
    [SFX_GREET] = {.mml = {"t160 @1 v11 q6 o5 l16 c d e g8 e g >c4", "t160 @3 v13 q6 o4 l8 c g >c4"}},
    [SFX_DIZZY] = {.glide = {N(740, 620, 70, W_SQ25, 60), N(620, 700, 70, W_SQ25, 60), N(700, 560, 70, W_SQ25, 60),
                             N(560, 640, 70, W_SQ25, 60), N(640, 400, 160, W_SQ25, 60)}},
    [SFX_STARTLE] = {.glide = {N(600, 1700, 80, W_SQ25, 75), N(1700, 1500, 40, W_SQ25, 50)}},
    /* inn-style lullaby, then a sunrise arpeggio */
    [SFX_SLEEP] = {.mml = {"t96 @3 v13 q8 o5 l8 g e c e d4 <b4 >c2", "t96 @3 v8 q8 o4 l4 c <g a g >c2"}},
    [SFX_WAKE] = {.mml = {"t150 @1 v11 q6 o5 l16 c e g >c e8 d8 e4", "t150 @3 v13 q7 o4 l8 c g >c4 <c4"}},
    [SFX_HELLO] = {.mml = {"t200 @1 v10 q5 o5 l16 g >c e8"}},
    [SFX_BLIP] = {.glide = {N(1250, 1250, 16, W_SQ25, 28)}},
    /* title-screen style opening */
    [SFX_BOOT] = {.mml = {"t132 @0 v11 q7 o5 l8 e4. g16 >c16 <b4 g4 a8. b16 >c8. d16 e2 r8 d16 c16 <b4 >d4 c8. <b16 a8. b16 >c2",
                          "t132 @3 v13 q7 o4 l4 c c <g g a a >c c <f f g g >c2",
                          "t132 @4 v4 q2 o6 l8 r1 r1 r1 r2 c4"}},
    [SFX_SULK] = {.mml = {"t110 @3 v12 q8 o5 l8 e d+ d c+4. r8", "t110 @1 v4 q3 o4 l8 r4 c r c r"}},
    [SFX_SHY] = {.mml = {"t180 @2 v10 q4 o6 l32 c e g >c r16 <g >c8", "t180 @3 v10 q6 o5 l16 r8 e g"}},
};

/* Background tunes: hummed now and then while idle, at a lower level. */
static const sound_t BGM[BGM_COUNT] = {
    /* town: an easy-going walk in C */
    [BGM_TOWN] = {.mml = {"t118 @1 v9 q7 l8 "
                          "o5 c4 e g a4 g e | o5 f4 a >c <b4. r | o5 a4 g f e4 d c | o5 d4 e f g2 "
                          "o5 c4 e g >c4 <b a | o5 a g f a g4 e4 | o5 f e d f e d c <b | o5 c2. r4 "
                          "o5 e4 e f g4 a g | o5 f4 f e d4. r | o5 d4 d e f4 g f | o5 e4 d c o4 b2 "
                          "o5 c4 e g >c4 d c | o5 b a g f e4 a4 | o5 g f e d c4 d e | o5 c2. r4",
                          "t118 @3 v12 q6 l4 "
                          "o3 c g e g | o3 f o4 c o3 g d | o3 f o4 c o3 a o4 c | o3 g o4 d o3 b g "
                          "o3 c g e g | o3 f a c e | o3 g b g f | o3 c g c2 "
                          "o3 a o4 e o3 a o4 e | o3 d a f a | o3 d a g b | o3 c g g f "
                          "o3 c g e g | o3 g b f a | o3 c g g b | o3 c2 c r",
                          "t118 @4 v3 q1 o7 l8 [r c]64"}},
    /* road: a brisk march in G */
    [BGM_ROAD] = {.mml = {"t132 @0 v8 q6 l8 "
                          "o4 g b o5 d4 d8. c16 o4 b g | o4 a b o5 c4 o4 a4 r4 | o4 f+ a o5 d4 c8. o4 b16 a f+ | o4 g a b o5 c d2 "
                          "o5 e d c o4 b o5 c4 e4 | o5 d c o4 b a b4 g4 | o4 a b o5 c d o4 a4 f+4 | o4 g2. r4 "
                          "o5 d4 d8. e16 d c o4 b o5 c | o5 e4 e8. f+16 e d c4 | o5 c4 c8. d16 c o4 b a b | o5 d2 o4 b4 r4 "
                          "o4 g b o5 d g f+4 e d | o5 c e d c o4 b4 a4 | o4 b o5 c d e o4 a4 f+4 | o4 g2 r2",
                          "t132 @3 v12 q5 l4 "
                          "o2 g o3 d o2 g o3 d | o2 a o3 e o2 a o3 e | o3 d a d a | o2 g o3 d o2 g o3 d "
                          "o3 c g c g | o2 g o3 d o2 g o3 d | o3 d a d a | o2 g o3 d o2 g o3 d "
                          "o2 g o3 d o2 g o3 d | o3 c g c g | o2 a o3 e o2 a o3 e | o3 d a d a "
                          "o2 g o3 d o2 g o3 d | o3 c g c g | o3 d a d a | o2 g2 r2",
                          "t132 @4 v5 q2 o5 l4 [r c r c]16"}},
};
