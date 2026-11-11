# Groove Drums for macOS

The Plinky groove sequencer and drum engine running in a small Mac window, to try
sounds and patterns without flashing the Plinky. It compiles the firmware sources
`sw/Core/Src/groove.c` and `drums.c` unchanged (same 31.25 kHz rate), so the
drums, Acid lines, step locks, conditions, lane loops and Clear behave as on the
hardware.

Build (needs only the Xcode command line tools) and run:

    ./build.sh
    open build/GrooveDrums.app

Patterns (24 slots) are saved to `~/Library/Application Support/GrooveDrums/patterns.bin`.

Not like the hardware: no touch pads, shift combos, OLED or LEDs, no CPU limit,
the scale is fixed to major, and the drum delay / reverb sends go to simple
stand-ins instead of Plinky's own effects.

Developer check: `build/GrooveDrums.app/Contents/MacOS/GrooveDrums --snapshot out.png [play]`
draws the window into a PNG and quits (`play` loads a demo beat first).
