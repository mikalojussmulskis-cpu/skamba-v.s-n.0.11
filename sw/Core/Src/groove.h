// groove.h - Plinky Groovebox extension (see GROOVEBOX_PLAN.md in the repo root).
//
// Rules:
//  - All groove code lives in groove.c / groove.h.
//  - Existing code calls in only through one-line hooks marked "GROOVE HOOK".
//  - Every runtime hook is guarded by groove_active, so with Groove mode off
//    the normal Plinky code paths behave exactly as before.
//  - Building with -DGROOVE_ENABLE=0 compiles every hook out. Together with
//    -DFACTORY_TEST_JIG that build is byte-identical to upstream v0.B4.
#pragma once

#ifndef GROOVE_ENABLE
#define GROOVE_ENABLE 1
#endif

#if GROOVE_ENABLE

// 0 = normal Plinky (default at boot), 1 = Groove mode.
extern volatile unsigned char groove_active;

// Called once from plinky_init(), after InitParamsOnBoot().
void groove_init(void);

// Use for one-line hooks in existing code: GROOVE_HOOK(groove_foo());
#define GROOVE_HOOK(stmt) do { stmt; } while (0)

#else

#define GROOVE_HOOK(stmt) do { } while (0)

#endif // GROOVE_ENABLE

// ---------------------------------------------------------------------------
// Measurement build (make GROOVE_STATS=1). Never shipped as a release.
// Shows audio-ISR CPU load, core clock and stack high-water on the OLED.
#ifdef GROOVE_STATS
void groove_stats_init(void); // paint the free stack area; call first thing in plinky_init()
void groove_stats_draw(void); // overlay numbers on the OLED framebuffer; call before flip()
#endif
