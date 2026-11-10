# Plinky Groovebox — Research & Implementation Plan

Status: **plan approved.** Implementation is on branch `groovebox`.

Review decisions:
1. Files moved back to the repo root.
2. `LAST_IDX` 136 → 160 approved (groove patterns use idx 136–159).
3. Track model approved: 8 kit lanes now, melodic lane later.
4. Control map approved.
5. `test_jig()` removed from the normal build (it can be restored with `-DFACTORY_TEST_JIG`). Borrowing `rampattern` RAM is allowed as a fallback.

> Paths below are relative to the repo root.

---

## 0. Before we start: two things to settle

1. **The repo layout changed after cloning.** All tracked files now sit under `plinky documents/`, so
   `git status` shows every file as deleted at the root and `plinky documents/` as untracked.
   Before creating the `groovebox` branch we should either move the files back to the root, or commit
   the move as its own change. Recommendation: move them back, so upstream merges stay clean.
2. **The build needs a toolchain override.** `sw/nocube_makefile/Makefile` hard-codes an STM32CubeIDE
   GCC 12.3 path. I built with the installed GCC 10.3.1:
   `make -j8 BUILD_TYPE=RELEASE TOOLCHAIN_LOCATION=`, which gave 0 warnings and 0 errors.
   Sizes from GCC 12.3 (the official release toolchain) will differ slightly. All later size comparisons
   will use the same toolchain as this baseline.

---

## 1. Architecture

### 1.1 Build structure
- **Unity build.** `sw/Core/Src/plinky.c` `#include`s nearly everything: `core.h`, `oled.h`,
  `codec.h`, `leds.h`, `adc.h`, `dac.h`, `gfx.h`, `spi.h`, `tables.h`, `flash.h`, `params.h`,
  `touch.h`, `calib.h`, `arp.h`, `edit.h`, `webusb.h`, `wavetable.h`, and finally `ui.h` at the very end.
  Almost all globals are non-static and live in that one translation unit, which is compiled `-O3`.
- `main.c` handles CubeMX init. `stm32l4xx_it.c` holds the IRQ handlers. tinyusb and the HAL are separate translation units.
- Other build targets also compile `plinky.c`: the desktop emulator (`sw/emu`) and the WASM build
  (`wasmbuild.sh`). A new `groove.c` must be added to them, or stubbed.

### 1.2 Execution model (the most important fact)
| Context | What runs | Where |
|---|---|---|
| **SAI DMA ISR** (half and full transfer, prio 1), every 64 samples ≈ **2.048 ms** at 31.25 kHz | `uitick()` → `touch_update()` (capacitive scan state machine), `led_update()`, **`DoAudio()`** | `codec.h:318-322`, `plinky.c:2517` |
| inside `DoAudio()` | MIDI in/out pump, `update_clock()` + sequencer step, `finger_synth_update()` (4 of the 8 strings per block, so each string every 4.1 ms), arp, `update_params()` (LFOs and mod matrix), pitch calculation, `RunVoice()` ×8, grain DMA scheduling, HPF, compressor, delay and reverb | `plinky.c:1876` |
| inside `touch_update()` → `update_finger()` | **all UI input logic**: shift-row handling (`on_shift_down/up/hold`), pad editing (`finger_editing`), encoder editing, long-press | `touch.h:1055`, `edit.h:116` |
| **Main loop** (`main.c:305`) | `plinky_frame()` → OLED drawing and LED-brightness calculation (`editmode_ui()` / `samplemode_ui()`), then `PumpFlashWrites()` (the only place internal flash is erased or programmed) | `ui.h:1183`, `params.h:904` |

So the whole UI state machine and the sequencer already run in the audio interrupt. Groove code
must follow the same pattern: cheap per-tick work in the ISR, with drawing and flash writes in the main loop.

### 1.3 Voices and voice allocation
- **8 voices, hard-wired to the 8 columns ("strings").** Voice `fi` reads only finger `fi`
  (`touch_synth_getlatest(fi)`). There is no dynamic allocation for touch or the sequencer. MIDI notes
  choose a string with `find_free_midi_string()` (`touch.h:629`).
- `RunVoice()` (`plinky.c:718`) plays **either** 4 PolyBLEP saws/wavetable oscillators **or** 2 grain
  pairs from a sample. The choice is **global**: `if (ramsample.samplelen)`. All 8 voices are synth, or all 8 are sampler.
- The envelope (`UpdateEnvelope`, `plinky.c:571`) is driven by finger pressure. A **trigger** is a 0→>0
  pressure edge: `synthfingertrigger = synthfingerdown & ~prevsynthfingerdown` (`plinky.c:1962`).
- The sampler, non-pitched ("tape") mode: voice `fi` plays **slice `fi`**, starting at
  `pos/256` (one of 8 sub-positions inside the slice) (`plinky.c:818-827`). **An 8-slice sample is already an 8-pad drum kit.**
- **Only one sample is active at a time**: `cur_sample1` comes from preset param `P_SAMPLE`, there is a single `ramsample`
  (`SampleInfo`: splitpoints and waveform preview), and one base address per block
  (`sampleaddr = (cur_sample1-1)*MAX_SAMPLE_LEN`, `plinky.c:2119`).
- **At most 6 voices stream sample data per block** (`MAX_SAMPLE_VOICES 6`, `plinky.c:2145`), prioritised
  by volume, inside a 4352-byte grain buffer that 32 SPI DMA reads per block fill (`spi.h:381`).
- Parameters are evaluated per voice with `param_eval_finger(paramidx, fi, f)` (`plinky.c:318`) using the
  single `rampreset`. **There is one preset at a time, and every voice shares its settings.**

### 1.4 Existing sequencer and arpeggiator (`arp.h`, `touch.h`)
- **Clock.** `update_clock()` (`arp.h:462`) runs once per block (one "tick" = 2.048 ms). The internal BPM clock or the external
  clock (CV clock, USB/serial MIDI 24ppqn ÷ 6) produces **16th-note pulses**, plus a half-way pulse, so
  `seq_step()` sees 32nd-note pulses and `P_SEQDIV` divides them.
- **Pattern storage in RAM.** `PatternQuarter rampattern[4]` (7168 B). Each of the 64 steps × 8 strings holds a
  `FingerRecord { u8 pos[4]; u8 pressure[8]; }`, which is 8 sub-steps of recorded pressure and position. Patterns record
  *gestures*, not note events. There is also `autoknob[16*8][2]` per quarter for knob A/B automation.
- **Playback.** In `finger_synth_update()` (`touch.h:711`), if there is no touch or latch pressure, the recorded pressure and position
  for the current sub-step is substituted (`touch.h:884-896`). The voice cannot tell sequencer input from a real finger.
  **This is the natural injection point for groove triggers.**
- **Step and loop logic.** `seq_step()` (`arp.h:297`) handles the direction modes, Euclid/probability (`euclidstuff`, `arp.h:87`) and
  `OnLoop()` (`touch.h:126`). `OnLoop()` applies queued preset, pattern and sample changes and the loop start "on next loop".
- **Loop window.** `rampreset.loopstart_step_no_offset`, `rampreset.looplen_step` (1–64), `cur_step`.
- **Arp.** `update_arp()` and `arptrig()` produce `arpbits`. `UpdateEnvelope` silences voices that are not in `arpbits`.

### 1.5 Presets, patterns and samples in flash (`params.h`, `flash.h`)
- **Internal flash bank 2** (0x08080000, 256 × 2 KB pages) is a **log-structured store**:
  - Every saved object is one 2 KB `FlashPage` = payload union (`Preset`, `PatternQuarter` or `SampleInfo`) + `SysParams` (16 B)
    + `PageFooter { u8 idx; u8 version(=2); u16 crc; u32 seq; }` (`params.h:437-548`).
  - Logical indices: presets 0–31, pattern quarters 32–127 (24 patterns × 4), sample info 128–135,
    `LAST_IDX = 136`. **Indices 136–253 are unused.**
  - `InitParamsOnBoot()` (`params.h:681`) scans all pages and keeps the highest `seq` per idx (`latestpagesidx[]`),
    plus a backup copy for presets (`backuppagesidx[]`, used for the "toggle" undo).
  - `ProgramPage()` (`params.h:765`) calls `AllocAndEraseFlashPage()` (round robin over pages 0–254, skipping live and
    backup pages, which wear-levels the store), then programs the data, sysparams and footer.
  - **Autosave**: edits set `ramtime[gen]`. `PumpFlashWrites()` (main loop) writes when the data has been idle for 5 s, is older than 60 s, or
    the user switches away from it. Reads go through memory-mapped flash pointers (`GetSavedPreset()` etc.).
    Copy-to-RAM is refused while `updating_bank2` is set, because reading bank 2 during an erase stalls the CPU.
- **Sample audio** lives in **two external SPI NOR chips**: 8 slots × 2 M samples × 16 bit = 32 MB
  (`spi.h`, `MAX_SAMPLE_LEN`). Recording uses the delay buffer as a staging area.
- **Calibration** sits in its own pages (`calib.h`). Page 255 of bank 2 is never allocated.
- **Preset version upgrades** are done in RAM in `CopyPresetToRam()` (`rampreset.version` 0→1→2). That is the existing migration idiom.

### 1.6 Touch pads and LEDs
- **Touch** (`touch.h:1180`, `touch_update`) is a 13-step TSC scan state machine run from the ISR. It produces 18 two-electrode
  sensors → 9 "fingers": 8 columns plus the **shift row (finger 8), which is a single slider**. Each finger has `pressure`
  (−2048…+2048) and `pos` (0–2047; `pos>>8` = row 0 at the top … 7 at the bottom). It keeps 8-frame history rings
  `fingers_ui_time` / `fingers_synth_time`.
- **Shift buttons** are decoded from finger 8's position into `shift_down` = 0…7 (`update_finger`, `touch.h:1119-1170`).
  There is hysteresis and ghost rejection. **Because the shift row is one slider, two shift buttons can never be read
  at the same time.** Shift chords are physically impossible.
- **LEDs**: `led_ram[9][8]` (8×8 pads + shift row). Brightness is computed in the main loop by `editmode_ui()` (`ui.h:977-1170`)
  and pushed out by `led_update()` (`leds.h:215`) from the ISR.

### 1.7 OLED UI (`ui.h`, `gfx.h`, `oled.h`)
- 128×32 1-bpp framebuffer (`vrambuf`), fonts F_8…F_32 with inline icon glyphs (`I_PLAY` …), `drawstr` /
  `fdrawstr` / `invertrectangle`, and `ShowMessage()` for transient pop-ups. `flip()` pushes over I²C (`oled.h`).
- `editmode_ui()` (`ui.h:702`) has one `switch(editmode)` (`EM_PLAY`, `EM_PARAMSA/B`, `EM_START`, `EM_END`, `EM_PRESET`)
  that draws the OLED **and** fills `led_ram`. `samplemode_ui()` handles `EM_SAMPLE`.

---

## 2. Current controls

Coordinates: columns **C1–C8** left→right (`fi` 0–7), rows **R1–R8** top→bottom (`pos>>8` 0–7), shift row **S1–S8**.
Parameter pads are C2–C7. In parameter mode C1 is the value slider and C8 selects the mod source.

### 2.1 Shift row (printed symbols)
| Button | Printed | Tap | Hold / gesture | In sample-edit mode |
|---|---|---|---|---|
| S1 | ⟩⟩ Params 1 | enter parameter mode (sticky if a param was chosen) | hold + pad = choose param; release → play mode with the C1 slider kept | short press toggles tape/pitched |
| S2 | ⟨⟨ Params 2 | same, page 2 | same | short press cycles loop mode |
| S3 | 🗋 Preset | sticky preset mode (tap again to leave) | hold + pad = select preset, pattern or sample | — |
| S4 | ⇤ Prev | stopped: step −1; playing: reset to loop start | hold → loop view; pad inside loop = jump, outside = new loop start (queued) | — |
| S5 | ⇥ Next | step +1 | hold → loop view; pad = loop end | — |
| S6 | ✕ Clear | clears latch | held while playing: mutes sequencer notes; stopped + record: clears the step and advances; in preset mode, long hold: initialise preset, clear pattern or clear sample | stop recording |
| S7 | ○ Record | toggles record (live when playing, step record when stopped) | hold + turn knob A/B = record knob automation | hold = level monitor → arm → record |
| S8 | ▷ Play | stopped → preview, released quickly → playing; playing → stop on next clock | hold = preview while held (released late → stops) | arm / start / stop recording |

### 2.2 Main pads
| Mode | Function |
|---|---|
| Play (no shift) | all 64 pads play notes (columns = strings, rows = scale steps). In tape-sample mode, column = slice and row = start offset. **No printed meaning.** |
| Params 1/2 (S1/S2 held or sticky) | C1 = value slider. C2–C7 × R1–R8 = parameters (table below). C8 = mod source: R1 Base, R2 Env2, R3 Pressure, R4 A, R5 B, R6 X, R7 Y, R8 Random. Tapping the selected param pad and sliding also edits it. Tapping Tempo repeatedly = tap tempo. Arp and Latch pads are toggles. |
| Preset (S3) | C1–C4 = presets 1–32, C5–C7 = patterns 1–24, C8 = samples 1–8 (tap again = sample off). **Long press** on preset/pattern = copy the current one there; on the current preset = toggle with its backup; on a sample = enter sample edit. Changes are queued to loop end while playing. |
| Prev / Next held | the 64 pads = 64 steps (step = row×8 + column); set loop start, jump, or set loop end |
| Sample edit | pads audition slices; sliding moves the slice start. While recording, tapping marks the next slice; long press stops. |

**Parameter grid** (page 1 / page 2; **bold** = no printed label on the panel):

| Row | C2 | C3 | C4 | C5 | C6 | C7 |
|---|---|---|---|---|---|---|
| R1 Sound | shape / noise | distortion / resonance | pitch / degree | octave / scale | glide / microtone | osc interval / column (stride) |
| R2 Env | sensitivity / env2 level | attack / attack2 | decay / decay2 | sustain / sustain2 | release / release2 | **blank pad: `P_ENV1_UNUSED` / `P_ENV2_UNUSED`** |
| R3 FX | delay send / reverb send | time / time | pingpong / shimmer | wobble / wobble | feedback / **`P_RVUNUSED`** | tempo / swing ⚠ |
| R4 Arp/Seq | arp on-off / latch on-off | order / order | clock div / clock div | chance / chance | euclid len / euclid len | arp octaves / gate len |
| R5 Sampler | scrub / scrub jitter | grain size / jitter | play speed / jitter | timestretch / **`P_JIT_PULSE`** | sample / **`P_JIT_UNUSED`** | pattern / step offset |
| R6 A/B | cv level / cv level | offset / offset | depth / depth | rate / rate | shape / shape | symmetry / symmetry |
| R7 X/Y | same for X / Y | | | | | |
| R8 Mixer | synth / input | wet-dry / input→FX | HPF / **`P_SYS_UNUSED1`** | **blank pad: MIDI in ch / MIDI out ch** (used since 0.A6) | cv quantise / **accel sens** (used) | volume / **stereo width** (used) |

⚠ **Swing is printed, and is a preset param (`P_SWING`), but the engine never reads it.** Normal Plinky has no swing today.

### 2.3 Knobs, encoder, jacks, boot gestures
| Control | Function |
|---|---|
| Knob A / Knob B (printed A, B) | always added to mod sources A/B. In sample record: knob A = input gain. With S7 held: automation recording. |
| Encoder turn | edits the selected or last-edited param (play/params modes); in sample edit, the slice point or pitch. **Does nothing in preset, prev or next modes.** |
| Encoder click (release) | param → default ↔ 0 (or full); in play mode it acts on the *last edited* param; on the arp/latch pads it toggles |
| Encoder hold | ~10–50 frames "Clear Mod?" → clears all modulation of the param; ~250 "REBOOT?"; ~500 reboot |
| Boot: twist knob A / B / both | touch calibration / CV calibration / DFU reflash |
| Jacks | inputs: clock, gate, pitch, A, B, X, Y, audio. Outputs: L/R audio, clock, trigger, pitch hi/lo, gate, pressure. |

### 2.4 Unused or unprinted controls (the only places new functions may go)
| # | Control | Today | Printed? | Suitability |
|---|---|---|---|---|
| U1 | **Params 1 + R2C7 (blank pad)** | nothing (`P_ENV1_UNUSED`) | **no icon, no label** | ★★★ best free spot; the same idiom as the Arp/Latch toggle pads |
| U2 | **Params 2 + R2C7 (blank pad)** | nothing (`P_ENV2_UNUSED`) | no | ★★★ |
| U3 | Params 2 + R3C6 (feedback pad, page-2 side) | nothing, shows "<unused>" | page-2 label missing | ★★ |
| U4 | Params 2 + R5C5 (timestretch pad, page-2 side) | nothing, shows "<unused>" (MIDI CC 119 writes it) | missing | ★★ |
| U5 | Params 2 + R5C6 (sample pad, page-2 side) | nothing | missing | ★★ |
| U6 | Params 2 + R8C4 (HPF pad, page-2 side) | nothing | missing | ★★ |
| U7 | Encoder turn / click in Preset, Prev or Next mode | nothing | no | ★★ |
| U8 | Double-tap Preset (S3) | net no-op (enter, then leave preset mode) | — | ★ |
| U9 | Long press of S3 with no pad touched | same as tap | — | ★ |
| U10 | S6 / S7 / S8 held + main pad | pads just play (or record) notes; no dedicated function | buttons printed | ★ only inside Groove mode, keeping the button's printed sense |
| U11 | Main pads with no shift held | play notes; **no printed meaning** | no | can be redefined *inside Groove mode* |
| ✗ | Shift + shift chords | **impossible** (single slider sensor) | | |
| ✗ | Knob A/B, all printed param pads, the C1 slider, the C8 mod column, every shift button's printed meaning | must keep their meaning | | |

---

## 3. Resources

### 3.1 Measured: release build of the unchanged source (GCC 10.3.1, `-O3`)
| Region | Size | Used | Free |
|---|---|---|---|
| App flash (`FLASH`, 0x08010000, 412 KiB = 421,888 B) | | 408,540 B (.isr 392 + .text 147,488 + .rodata 260,160 + .data init 484 + misc 16) | **13,348 B (3.2 %)** ⚠ |
| Wavetable region (36 KiB) | | 35,060 B | 1,804 B (separate region, not usable for code) |
| SRAM1, linked part (32 KiB) | | .data 484 + .bss 27,140 = 27,624 B; +512 heap +1,024 stack minimum reserved | **3,608 B unreserved**; ~4.6 KB including the stack reserve |
| SRAM1, upper 64 KiB | | delay line `delaybuf` (fixed address 0x20008000) | 0 |
| SRAM2 32 KiB | | reverb buffer `reverbbuf` (fixed address 0x10000000) | 0 |
| Bank-2 flash store | 255 pages × 2 KB | at most 136 live + 32 backup = 168 pages | 87 pages of wear-levelling slack |
| External SPI flash | 32 MB | 8 sample slots | — |

The largest RAM users: `rampattern` 7168, `grainbuf` 4352, `voices` 1760, `rampreset` 1552, `ramsample` 1072,
`hpcd_USB_OTG_FS` 1032, `scope` 512, `surf` 512, `vrambuf` 513.
Largest static stack frames (`.su`): `editmode_ui` 360 (main loop), `RunVoice` 200 and `DoAudio` 184 (ISR).

**Conclusion: flash is the binding constraint, then RAM.** The whole groovebox must fit in roughly **9–10 KB of code** and about
**1.4 KB of RAM**, unless we reclaim space (§7).

### 3.2 CPU headroom: not measurable without hardware
- Per-block budget: 64 samples / 31,250 Hz = **2.048 ms**. Code comments assume an 80 MHz core (≈164 k cycles per block).
  The PLL config (HSE ×14 ÷2, with `HSE_VALUE` = 16 MHz in `stm32l4xx_hal_conf.h`) should be checked against DWT on the device.
- The firmware already has instrumentation: `TickCounter _tc_budget/_tc_all/_tc_audio/_tc_fx/_tc_osc` (DWT `CYCCNT`),
  logged only in DEBUG builds over UART. **Phase 0 shows these on the OLED in a measurement build.**
- There are hints that headroom is limited in sampler mode: the comment on `MAX_SAMPLE_VOICES 6` says it "also helps CPU
  load", and FX/HPF are disabled in DEBUG builds.
- Estimated groove cost: the per-tick step scheduler is under 1 k cycles (<1 %). Per-voice p-lock lookup with a
  96-bit "has lock" mask per voice costs about 1–3 k cycles per block (≈1–2 %). In normal mode every hook is one
  load plus one predicted branch.

### 3.3 Cost of a groove pattern
Format in §5. Per pattern (8 lanes × up to 64 steps):

| Variant | RAM / payload |
|---|---|
| Header + 8 lane headers + 8×64 step bytes (trig, velocity, chance), **no p-locks** | **656 B** |
| + 64 p-locks (4 B each) | 912 B |
| + 128 p-locks (recommended working size) | **1,168 B** |
| Full flash page (2,024 B payload) = 342 p-locks | 2,024 B (flash only) |

In flash, each pattern is always **1 page (2 KB)**. 24 patterns = 24 pages of the 255. For comparison, one Plinky pattern = 4 pages and 7 KB of RAM.
Only the **active** pattern lives in RAM. Queued and chained patterns are copied from flash at the bar boundary.

---

## 4. Proposed Groove-mode control map

### 4.1 Track model (shaped by the engine constraints in §1.3)
- **A kit = one sample slot with 8 slices. Lanes 1–8 = slices 1–8 = columns C1–C8 = voices 1–8.**
  Each lane is a full track: its own steps, length (1–64), mute, velocity and chance per step, p-locks, and
  4 lane-level sound settings (e.g. decay, pitch/rate, level, start). This covers features 1–5 with 8 tracks
  instead of the 4 you targeted.
- All lanes share the active preset's sound and FX. Lane sound settings and p-locks override the per-voice parameters
  through one hook in `param_eval_finger`.
- **Melodic track**: see §7. It cannot run *inside* the engine without hot-path changes, so it is a later, optional phase.

### 4.2 Enter and exit
**Params 1 + blank pad R2C7 (U1)** toggles Groove mode, exactly like the Arp and Latch toggle pads. The OLED shows
"GROOVE ON/OFF". In params mode the pad LED is lit while Groove is active.
- Groove state is **RAM-only in v1**: Plinky always boots in normal mode.
- Params 2 + R2C7 (U2) = **"Kit"** virtual param: which sample slot is the drum kit (default = the preset's sample).

### 4.3 Groove main view (no shift held)
```
R1 ┐
R2 │  DRUM PADS: column = lane (slice). Pressure = velocity,
R3 │  row = slice start offset (like tape mode today).
R4 │  Playing a column also selects that lane for editing.
R5 ┘
R6    MUTE ROW: tap = mute/unmute lane. Hold ≥0.5 s = select the lane silently.
R7    STEPS 1–8  ┐ selected lane, current 16-step page
R8    STEPS 9–16 ┘ tap = toggle (tap pressure = step velocity)
```
LEDs: lane activity flashes on hits, the selected lane is dimly lit, muted lanes are dark. In the step rows: empty = off,
set = mid, accent = bright, playhead = moving glow, steps past the lane length = off.
OLED: `G03 ▶ 120  Lane 3/8  pg 1/4`, plus a bar of the lane playheads.

### 4.4 Shift buttons inside Groove mode (printed meanings kept)
| Button | In Groove mode |
|---|---|
| S1/S2 Params | **unchanged** (selecting a param also sets the p-lock target) |
| S3 Preset | presets C1–C4 unchanged. **C5–C7 "pattern" pads = groove patterns 1–24**, queued to the bar when playing. Tapping several pattern pads in order while S3 is held = **chain** (OLED shows `1›3›2`). Long press = copy the groove pattern; S3 + hold S6 = clear it. C8 "sample" pads = choose the kit. |
| S4 Prev | tap = previous 16-step page (view only while playing; moves the cursor when stopped). Hold → **64-step view of the selected lane** (today's loop-view idiom): tap = jump or lane start. |
| S5 Next | tap = next page. Hold → 64-step view: tap = **lane end, i.e. lane length 1–64**. |
| S6 Clear | hold + step pad = clear that step's p-locks, then the step itself. Hold + mute-row pad (~1 s) = clear the lane. Held while playing = mute all groove hits (as it mutes the sequencer today). |
| S7 Record | tap = record on/off. **Record + Play = live record**: finger drumming on R1–R5 is quantised to the nearest step, with velocity. Record + stopped = step-entry mode: tapping a drum pad writes the hit at the cursor and advances. Long hold (no knob moved) while playing = **Fill** (proposal). |
| S8 Play | unchanged transport (shared with Plinky: same clock, external sync, MIDI start/stop) |

### 4.5 Step edit and p-locks
- **Hold a step pad + turn the encoder** = lock the currently selected parameter (last param chosen with S1/S2) on that step.
  OLED: `LOCK s5 Decay 34`. Click the encoder while holding the step = remove that lock.
- Holding a step shows its velocity, chance and lock count. Steps with locks blink faintly.
- **Virtual step params**, active only in Groove mode, on the unprinted page-2 slots:
  - U4 (R5C5) **Step Velocity**
  - U5 (R5C6) **Step Chance / condition** (100…0 %, then 1:2, 2:2, Fill, !Fill)
  - U6 (R8C4) **Note Repeat rate**
  - U3 (R3C6) **Lane Level**

  Select one with S2, then hold a step and turn the encoder. In normal mode these slots stay exactly as they are today.
- Lockable parameters are the per-voice ones: pitch, octave, play speed (rate), scrub (start), grain size, timestretch, attack,
  decay, sustain, release, sensitivity (level), distortion, resonance, noise, glide, gate len, env2. Global FX (delay, reverb, HPF, mix) are
  **not** lockable per lane in v1.
- **Swing** = the existing printed **swing** pad (Params 2 + R3C7, `P_SWING`). In Groove mode it delays odd 16ths by up to
  75 % of half a step. Normal mode keeps ignoring it, so normal behaviour is unchanged.
- Tempo, tap tempo, clock div and gate length keep their existing pads.

### 4.6 Things to decide
- Lane select by playing (proposed) versus a silent select only.
- Fill gesture: long hold of S7, or a virtual param toggle.
- Whether the drum rows (R1–R5) should map row → velocity layers instead of start offset.

---

## 5. Data format and storage

### 5.1 Layout (fits one `FlashPage` payload of 2,024 B; packed, little-endian)
```c
#define GROOVE_MAGIC   0x31565247u  // "GRV1"
#define GROOVE_VERSION 1
#define GROOVE_LANES   8
#define GROOVE_STEPS   64
#define GROOVE_MAX_LOCKS 128        // RAM working size; format allows up to 342

typedef struct {            // 16 B
  u8 length;                // 1..64
  u8 start;                 // 0..63
  u8 flags;                 // b0 mute, b1-7 reserved
  u8 level;                 // 0..255
  u8 nsound; u8 rsv[3];
  struct { u8 param; s8 val; } sound[4];   // lane-level overrides (val × 8 = param units)
} GrooveLane;

typedef struct {            // 4 B, sorted by (lane, step)
  u32 step:6, lane:3, param:7, val:16;      // val = s16 in param units (±1024)
} GrooveLock;

typedef struct {
  u32 magic; u8 version; u8 kit;           // kit: 0 = preset's sample, 1..8
  u8 preset_link;                          // 255 = none
  u8 chain_next;                           // 255 = none
  u8 nlocks; u8 swing_override;            // 255 = use P_SWING
  u8 rsv[6];                               // 16 B header
  GrooveLane lane[GROOVE_LANES];           // 128 B
  u8 step[GROOVE_LANES][GROOVE_STEPS];     // 512 B: b7 trig | b4-6 vel(0=default) | b0-3 chance/cond
  GrooveLock lock[GROOVE_MAX_LOCKS];       // 512 B
} GroovePattern;                           // 1,168 B
_Static_assert(sizeof(GroovePattern) <= 2048 - 16 - 8, "fits a FlashPage");
```

### 5.2 Where it goes: new logical indices in the existing store
- Groove patterns 1–24 → **logical idx 136–159** (`FIRST_GROOVE_IDX`). They use the same `PageFooter` (version 2, CRC, seq)
  and the same wear-levelled allocator, with autosave from the main loop.
- The **Preset, PatternQuarter and SampleInfo layouts, their indices 0–135, and page contents are untouched.**
- ⚠ **The one change to existing storage code** (needs your approval): `LAST_IDX` goes from 136 to 160, so the boot scan indexes groove
  pages and `AllocAndEraseFlashPage()` stops erasing live groove pages. That costs +24 B of RAM for `latestpagesidx`.
  - *Alternative with no change to `params.h` constants*: a separate groove page table, plus a hook in `AllocAndEraseFlashPage()`
    so it skips groove pages, plus a groove-specific `ProgramPage` variant. That means more new code (flash is tight) for the same effect.
- Capacity after the change: up to 136 + 24 live + 32 backup = 192 of 255 pages, leaving 63 pages of wear slack.

### 5.3 Versioning and migration
- An empty, unknown or CRC-bad page reads as an **empty pattern** (the same rule `GetSaved*()` uses today).
- `magic` plus `version` sit inside the payload. Loading upgrades older versions in RAM (the `CopyPresetToRam` idiom). A pattern
  with a **newer** version is shown as "read-only, newer firmware" and is never overwritten automatically.
- **Downgrading to 0.B4**: old firmware skips idx ≥136 at boot, so all presets, patterns and samples load normally. It treats groove
  pages as free and may eventually erase them, so groove data can be lost after a downgrade, but nothing else is affected.
  Minor quirk: boot-time `sysparams` (current preset, headphone volume) might come from a slightly older page.
- **Upgrading from 0.B4**: there are no groove pages, so every slot is empty. Nothing to migrate.
- Groove mode on/off is **not** persisted in v1. Persisting it would use a `SysParams.pad` byte, which is a storage change, so ask first.

---

## 6. Phased plan
Every phase builds with 0 warnings, can be flashed, and is committed on `groovebox`. All new code goes in `sw/Core/Src/groove.c` and `groove.h`.
Hooks are one-liners marked `// GROOVE HOOK` and guarded by `if (groove_active)`. A compile-time switch
`GROOVE_ENABLE` (default on) lets us build **with all hooks compiled out**, and that build must produce a binary byte-identical to the baseline.
That gives an automatic proof that normal-mode code paths are untouched. `groove.c` is compiled with `-Os` to save flash.

**Planned hooks** (all phases):
| # | Location | Purpose |
|---|---|---|
| H1 | `plinky.c` `plinky_init` after `InitParamsOnBoot()` | `groove_init()` |
| H2 | `plinky.c` `DoAudio` after `update_clock()` | `groove_tick(gotclock)`: step scheduler |
| H3 | `touch.h` `finger_synth_update` before the MIDI section | inject lane triggers as pressure/position; skip normal seq/latch while active |
| H4 | `plinky.c` `param_eval_finger` | lane sound settings and p-lock override |
| H5 | `plinky.c` `DoAudio` after `cur_sample1 = …` | kit sample override |
| H6 | `edit.h` `is_finger_an_edit_operation` / `finger_editing` | route pads to the groove UI |
| H7 | `touch.h` `on_shift_down/up/hold` | groove meanings of S3–S7 |
| H8 | `ui.h` `editmode_ui` | groove OLED and LEDs |
| H9 | `arp.h` `update_arp` | arp treated as off while Groove is active (the preset flag is untouched) |
| H10 | `params.h` `GetParam` / `EditParamNoQuant` | virtual params on U2–U6 (as `P_HEADPHONE` already works) |
| H11 | `params.h` `PumpFlashWrites` (+ `LAST_IDX`) | groove autosave |
| H12 | `nocube_makefile/Makefile` | add `groove.c` with `-Os` |

| Phase | Content | Hardware test |
|---|---|---|
| **0 Baseline & measurement** | branch; Makefile entry for `groove.c` (empty); `GROOVE_ENABLE`; byte-identical check; a *measurement build* showing CPU % (avg/peak per block) and stack high-water (stack painting) on the OLED | Run a heavy preset (8 voices, sampler, long reverb and delay, arp) for 5 min and note the peak CPU and stack. Confirm normal Plinky behaves the same (regression checklist R). |
| **1 Toggle + drum pads** | H1, H6, H8 (minimal), H9, U1 toggle; groove view: R1–R5 trigger slices C1–C8, mute row, lane select; OLED groove screen | Toggle in and out from Params 1 + R2C7. Drums play from pads. Mute works. Leaving restores exact normal play. Run R. |
| **2 16-step sequencer (RAM only)** | H2, H3; 8 lanes × 16 steps; step toggle on R7–R8 with velocity from pressure; playhead; shared transport, external clock, MIDI start/stop | Program a beat, play/stop, sync to MIDI clock and CV clock, mute live, check timing by ear and on CV clock out. R. |
| **3 Storage** (needs approval of §5.2) | H11; 24 groove patterns on the S3 pattern pads; autosave; copy (long press); clear (S3 + S6) | Save, power-cycle, recall. Copy and clear. **Downgrade to 0.B4 and confirm presets, patterns and samples are intact**, then upgrade again. R. |
| **4 Length, paging, swing, velocity, chance** | lane length 1–64 via S5 hold, S4 hold view, page tap, polymeter, `P_SWING`, virtual Velocity/Chance (H10) | Polymeter 16 vs 12 vs 7, swing 0–75 %, probability, accent. R. |
| **5 P-locks & lane sounds** | H4, H5; hold step + encoder; lane sound settings; lock indicators; clear locks; kit param | Lock pitch, decay and start per step. Check that normal-mode param evaluation is unchanged (R plus a CPU check against Phase 0). |
| **6 Pattern queue & chain** | switch on bar boundary (copy from flash in the ISR only when `!updating_bank2`, otherwise defer one bar); chaining | Switch patterns while playing (in time), chain 3 patterns, switch while an autosave is in progress. |
| **7 Live record, repeat, fill** | quantised live record, step-entry, note repeat (U6), fill conditions | Record finger drumming at 90/120/160 BPM and check the quantisation. Repeat rates. Fill on and off. |
| **8 (optional) Melodic lane** | 8a: MIDI-out lane (notes to an external synth). 8b (research spike, only if Phase 0 shows headroom): split voices into a sampler group and a synth group | Separate plan after Phase 7. |

**Regression checklist R** (every phase, Groove off): presets 1–32 load and sound identical, preset copy and toggle, pattern live and step
record and playback, loop start/end, arp and latch, knob automation record, sample record, slice and pitched/loop modes, MIDI in/out/clock and
program change, CV clock in/out and gate, encoder click / clear-mod / reboot, boot calibration gestures, autosave after 5 s. Each phase report
will include flash and RAM numbers against the baseline (408,540 B flash / 27,624 B static RAM).

---

## 7. Risks, limits and simplifications

### Not feasible as specified (and the proposed substitute)
| Request | Why not | Substitute |
|---|---|---|
| 3 sample tracks + 1 **synth** melodic track playing at the same time | Synth vs sampler is a global engine switch; all voices share one preset; per-voice presets would need ~1.5 KB of RAM each plus a rewrite of the param system | 8 kit lanes now. Melodic via the MIDI-out lane (8a) or the voice-split spike (8b). Pitched melodies can come from the kit by p-locking play speed or pitch on a lane. |
| Each drum track from a different sample slot | One `ramsample` and one base address per block | One kit = one 8-slice sample (record or slice a kit). A later option is per-lane slot+slice, which changes `RunVoice` and grain scheduling (hot path). |
| Per-step locks of delay/reverb/HPF per lane | Global FX | Later option: global "FX lock" steps |
| 8 simultaneous sample hits always audible | Only 6 voices stream sample data per block (SPI/grain budget) | Accept: the quietest voices drop (as today) |
| Sample-accurate timing | Strings update every 4.1 ms; ticks are 2 ms | ≤4 ms trigger quantisation; swing resolution 2 ms (inaudible for most grooves) |
| Shift + shift combinations | The shift row is a single slider sensor | Not used |

### Risks
1. **Flash: only 13.3 KB free.** This is the biggest risk. Mitigations: `-Os` for `groove.c`; reuse the existing gfx and UI helpers; no new
   fonts or tables; measure every phase. If we still run out, options that **need your approval**: build `test_jig()` (7 KB of factory test
   code) only in a separate factory build, or trim other non-user code.
2. **RAM: about 3.6 KB unreserved, and the real stack peak is unknown.** Groove budget ≈1.4 KB (pattern 1,168 + runtime ~250). Phase 0 measures the stack.
   Fallback (approval needed): borrow the step area of `rampattern` (6 KB) while Groove is active. That needs guards in
   `CopyPatternToRam` and `update_params` (autoknob), plus a reload on exit.
3. **Voices are tied to columns.** Touching a column while its lane plays shares that voice (as with today's sequencer). Acceptable and familiar.
4. **The preset shapes the drums.** A pad preset with slow attack gives mushy drums. Mitigation: default lane sound settings
   (attack 0, sustain 0, short decay) applied while in Groove.
5. **Side effects that need explicit decisions in Groove**: arp forced off (H9), latch cleared on entry, normal sequencer playback suppressed,
   and MIDI-out / CV trigger / gate / pitch outputs will follow the drum hits (as they follow the sequencer today). That is arguably a feature.
6. **Everything runs in the audio ISR.** Groove work per tick is O(8). No flash access, allocation or blocking in the ISR. Pattern copies at the
   bar only when `!updating_bank2`, as the existing `CopyPatternToRam` does.
7. **Unity build.** `groove.c` cannot see `static inline` helpers (`param_eval_int`, …). The hooks pass values in, and `groove.h` declares the needed globals `extern`.
8. **Other builds.** The emulator, WASM and STM32CubeIDE (`.cproject`) builds need `groove.c` added. CubeIDE needs a per-file `-Os` setting.
9. **CPU headroom unknown** until Phase 0. Phase 8b is only attempted with clear headroom.
10. **Toolchain difference** (GCC 10.3 here vs 12.3 upstream) may shift sizes by a few hundred bytes.

### Suggested simplifications if space runs out (in order)
1. Lock pool 128 → 64 (−256 B RAM).
2. Drop lane sound settings and use p-locks only.
3. Drop chaining; keep queued switching.
4. Drop note repeat and fill.
5. 32-step maximum instead of 64 (−256 B RAM per pattern).

---

## 8. Questions for review
1. Repo layout (§0): move the files back to the root, or commit the move?
2. Approve **`LAST_IDX` 136 → 160** (groove patterns as logical idx 136–159 in the existing store)?
3. Approve the track model (8 kit lanes now, melodic lane later)?
4. Approve the control map, in particular the **U1 toggle**, **U3–U6 virtual params**, the **mute row**, and the **S7 long-hold = Fill** proposal?
5. Allowed fallbacks if flash or RAM runs out: factory-only `test_jig`, and borrowing `rampattern` RAM in Groove mode?

---

## 9. Progress log

### Phase 0: baseline & measurement (done)
- Moved the repo back to the root and created branch `groovebox`.
- `sw/Core/Src/groove.c` and `groove.h` added (`GROOVE_ENABLE`, `GROOVE_HOOK()`, runtime flag `groove_active`, always 0 at boot).
- Hooks so far: `groove_init()` after `InitParamsOnBoot()`. The measurement build also hooks the start of `plinky_init()` and just before `flip()` in `editmode_ui()`.
- `test_jig()`, `set_test_mux()` and `set_test_rgb()` are behind `FACTORY_TEST_JIG` (approved).
- Makefile:
  - falls back to `arm-none-eabi-gcc` in PATH;
  - `groove.c` is compiled `-Os` in release;
  - variants `GROOVE_STATS=1`, `GROOVE=0` and `FACTORY_TEST_JIG=1` each build into their own folder.
- `touch.h`: a DEBUG-only initialiser silences an upstream `-Og` warning. Release code is unchanged.
- **Verification**: `make GROOVE=0 FACTORY_TEST_JIG=1` is **byte-identical** to the unmodified v0.B4 source build (SHA-1 `55f8556d…`).

| Build (GCC 10.3.1, -O3) | App flash used | Flash free | Static RAM (.data+.bss) |
|---|---|---|---|
| Baseline v0.B4 | 408,540 | 13,348 | 27,624 |
| Phase 0 default | 403,288 (−5,252) | **18,600** | 27,624 (±0) |
| Phase 0 `GROOVE_STATS=1` | 404,128 | 17,760 | 27,648 |

**Phase 0 hardware measurements** (stats build, synth presets):
- Core clock **112 MHz** (one block ≈ 229 k cycles).
- Audio ISR average **72–73 %** (idle, all notes held, sequencer with long delay and reverb). Peak 74–77 % normally, **96 % on a pattern switch** (7 KB pattern copy). Peak since boot 113 %, probably a start-up spike (to confirm).
- Minimum free stack **2,784 B**, so real stack use is about 1.6 KB.
- Not yet measured: sample (tape) mode, the mode Groove uses.

Consequences:
- Groove ISR work must stay well under ~2 %.
- Groove pattern switches must be cheap (≤1.2 KB copy).
- The RAM budget of ~1.4 KB still leaves ≥1.3 KB stack margin.

### Phases 1 + 2: toggle, drum pads, mute row, 16-step sequencer (RAM only) (done)
- **Toggle**: Params 1 + blank pad R2C7 (or encoder click while it's selected). The OLED shows "Groove On/Off" and the pad LED is lit while active.
- **Groove view** (play mode only):
  - R1–R5: drum pads (live touch, unchanged sampler behaviour); playing one selects its lane.
  - R6: mute row (tap = mute; hold 0.4 s = select the lane silently).
  - R7–R8: 16 steps of the selected lane (tap toggles; tap pressure = velocity).
- **Sequencer**: shared transport and clock (Play, external CV/MIDI clock, MIDI start/stop, Prev = restart). One 16th note per clock. Hits are injected as finger pressure in `finger_synth_update`, with a gate of half a step.
- **While Groove is on**: arp forced off, latch off, normal-pattern playback and recording suppressed. The preset and pattern data are untouched.
- Hooks added: `edit.h` (edit-op routing, pad input, pad up, toggle), `touch.h` (no-stick, readpattern, latch, record, hit injection), `arp.h` (arp off, reset), `plinky.c` (tick, step-record advance), `ui.h` (OLED, LEDs, toggle LED, value text), `params.h` (param name).
- `GROOVE=0 FACTORY_TEST_JIG=1` is still byte-identical to v0.B4.

| Build | App flash used | Flash free | Static RAM |
|---|---|---|---|
| Baseline v0.B4 | 408,540 | 13,348 | 27,624 |
| Phases 1+2 default | 406,536 | **15,352** | 28,364 (+740) |
| Phases 1+2 `GROOVE_STATS=1` | 407,184 | 14,704 | 28,396 |

### Phase 3 (re-planned): synthesised drums + melody alongside (done)
Decision (user, after phases 1+2): drums are synthesised instead of borrowing Plinky voices, so Plinky's 8 voices stay free for melody.
- **`drums.c` / `drums.h`**: 8 drum voices (one per lane), rendered in `DoAudio` right after the 8 Plinky voices (before HPF, compressor and FX).
  - Models: Kick, Snare, Clap, C.Hat (chokes O.Hat), O.Hat, Tom, Perc (cowbell), and **Sample** (the old behaviour: the lane plays its slice of the preset's sample on a Plinky voice).
  - One generic voice per lane: sine or square-pair oscillator with a pitch envelope, plus filtered noise. No libm, only sounding voices are computed.
  - Checked on the Mac by rendering every model to WAV (levels, lengths, no NaNs).
- **Default kit**: 1 Kick, 2 Snare, 3 Clap, 4 C.Hat, 5 O.Hat, 6 low Tom, 7 high Tom, 8 Perc.
- **Lane sound** = 8 bytes in `GrooveLane` (model, tune, sweep, decay, snap, tone, drive, level). In the drum grid, the printed pads edit the selected lane instead of the preset:
  - Shape = model, Pitch = tune (±1 oct), Glide = sweep, Decay = decay
  - Noise = snap, Resonance = tone, Distortion = drive, Sensitivity = level
- **Views**: Params 2 + blank pad R2C7 switches the pads between the drum grid and normal Plinky playing while the drums keep running.
  - In the Plinky view, everything is normal Plinky, including the sequencer, arp, latch and recording (the melody track).
  - The arp and normal-sequencer suppression from phases 1+2 is removed. Latch writes and pattern recording are only blocked for drum-grid touches.
- **Live drum pads** (R1–R5): trigger one string update (4 ms) after touch-down, so velocity reflects the hit. The touch is swallowed, so the Plinky voice isn't played.
- `GROOVE=0 FACTORY_TEST_JIG=1` is still byte-identical to v0.B4.

| Build | App flash used | Flash free | Static RAM |
|---|---|---|---|
| Baseline v0.B4 | 408,540 | 13,348 | 27,624 |
| Phase 3 default | 410,236 | **11,652** | 29,052 (+1,428 vs baseline) |
| Phase 3 `GROOVE_STATS=1` | 410,908 | 10,980 | 29,084 |

Estimated free stack is now about 1.35 KB (measured 2,784 B minus the added RAM). Verify with the stats build.

**Phase 3 hardware feedback**:
- Drums too loud and distorting the mix (they went through the synth compressor and soft clip).
- Stats with all 8 lanes: average CPU 89 % (drums ≈ 17 %), peak 95 %, free stack 1,448–1,456 B.
- The melody alongside the drums works.

### Phase 4: drum bus, CPU, velocity rows, live record, pattern storage (done)
- **Drum CPU**:
  - no per-sample division (cubic soft clip);
  - all loop state in registers;
  - noise, tone, drive and sends are only computed when in use (decided per block);
  - voices stop at about −66 dB.
- **Separate drum bus** (`drum_bus_dry/dl/rv`):
  - dry drums are added in the FX loop *after* the synth compressor;
  - per-lane **delay send** and **reverb send** feed Plinky's delay (`delaysend`) and reverb (a 2-line hook in `Reverb2`), independent of the synth's send levels;
  - in the drum grid, the printed delay-send and reverb-send pads edit the selected lane.
- **Drum Volume**: master level stored per pattern, on the unprinted pad Params 2 + HPF pad (`P_SYS_UNUSED1`). Active whenever Groove is on.
- **Velocity rows**: drum pads R1–R5 trigger immediately with velocities 100/71/57/43/29 %.
- **Live record**: Record + Play writes hits to the nearest step. A hit in the second half of a step goes to the next step, which is then skipped once so it doesn't sound twice.
- **CPU save**: while Groove is on, Plinky voices that are silent (pressure ≤ 0, level < 1e-4, no trigger) skip `RunVoice`. Normal mode is untouched.
- **Storage** (approved): `LAST_IDX` is 160 when `GROOVE_ENABLE`. Groove patterns 1–24 use logical idx 136–159, one 656-byte page each, through the existing `ProgramPage`.
  - **Autosave** from `PumpFlashWrites`: 5 s idle, at most 60 s old, or immediately when another slot is waiting.
  - **Preset mode** (S3) in the drum grid: the 24 pattern pads select groove patterns. Changes are queued to the bar while playing and immediate when stopped. Long press = copy the current pattern there. S3 + hold S6 = clear.
  - Slot 1 loads at boot. Patterns are only loaded in the ISR when no flash write is running and nothing is unsaved.
- `GROOVE=0 FACTORY_TEST_JIG=1` is still byte-identical to v0.B4.

| Build | App flash used | Flash free | Static RAM |
|---|---|---|---|
| Baseline v0.B4 | 408,540 | 13,348 | 27,624 |
| Phase 4 default | 413,076 | **8,812** | 29,416 (+1,792 vs baseline) |
| Phase 4 `GROOVE_STATS=1` | 413,716 | 8,172 | 29,448 |

Estimated free stack is now about 1.08 KB (1,448 measured minus 364 added). Verify with the stats build before adding more RAM.

**Phase 4 hardware feedback**:
- Drums can be 2× louder (done in `6d7a75a`).
- Live recording, sends and velocity rows work. Saving works; Groove itself isn't remembered across power-off, which is intended and accepted.
- Stats:

  | Situation | Average | Peak | Free stack |
  |---|---|---|---|
  | Drums only | 57 % | 66 % | 1,080 B |
  | Drums + 3 Plinky notes | 70 % | 80 % | 1,080 B |
  | Drums + all 8 Plinky voices | 92 % | **101 %** (overrun) | 1,080 B |

### Phase 5: voice limits, new drum engine with resonant filter (done)
- **Plinky limited to 4 sounding voices in Groove mode** (`groove_voice_alloc`, called from `DoAudio`):
  - A new note steals the oldest: released notes first, then held ones, never one triggered in the same block. Arp retriggers count as triggers.
  - A stolen voice fades out in ~20 ms (hook in `UpdateEnvelope`), is skipped once silent, and stays silent until its string is played again.
- **Drums limited to 6 voices**:
  - A lane retriggers its own voice. Otherwise a free voice is used, otherwise the quietest non-kick voice is stolen. Kicks are never stolen.
- **New engine** (original code, MIT):
  - Tone: sine, two sines or 2-op FM, with two pitch envelopes.
  - Excitation: noise or a bank of square oscillators ("metal"), through a one-pole HP and a 2-pole SVF (branch-free LP/BP/HP mix).
  - A **resonant SVF per lane**: low-pass / off / high-pass with resonance, read live every block (sweepable while playing). Level, sends and drive are also live.
  - Rendering uses branch-free passes over 32-sample sub-blocks. The Cortex-M4 has no branch predictor, and the sub-blocks keep the ISR stack small.
  - `drums.c` is compiled `-Os`; its hot passes are `optimize("O2")`.
- **Models** (stored numbers are stable, new ones appended):
  - Kick, Kick2 (punch: fast snap + body sweep + saturation), Snare (two head modes + band-passed wires)
  - Clap (4 bursts, band-pass), C.Hat / O.Hat (6-square metal + noise, steep HP, choke), Cymbal
  - Tom (with fifth overtone), Rim (rimshot↔clave), Cowbell (2 squares, band-pass), FM (8 ratios), Zap
  - Sample (Plinky voice)
- **Lane sound in the drum grid**:

  | Pad | Lane setting |
  |---|---|
  | Shape | type |
  | Pitch | tune |
  | Glide | sweep |
  | Decay | decay |
  | Noise | snap |
  | Osc interval | colour |
  | Distortion | drive |
  | Sensitivity | level |
  | Delay / reverb send | sends |
  | **HPF** | **filter** (LP ← off → HP) |
  | **Resonance** | **filter resonance** (was "tone") |

  The OLED shows drum names ("Pitch Sweep", "Filter LP 40", …) and "Drum N" as the page.
- **Pattern format v2**: the lane's 2 reserved bytes became filter and reso. v1 patterns are migrated on load (filter off).
- **Default kit**: Kick, Snare, Clap, C.Hat, O.Hat, Tom, Rim, FM bell.
- **Mac sound lab** (scratchpad): every model rendered and measured for peak/RMS, length, spectral centroid, pitch trajectory and DC. Levels balanced (kick −12 dBFS peak, hats ≈ −17, …). An extreme-settings sweep shows no instability. Demo beats rendered.
- `GROOVE=0 FACTORY_TEST_JIG=1` is still byte-identical to v0.B4.

| Build | App flash used | Flash free | Static RAM |
|---|---|---|---|
| Baseline v0.B4 | 408,540 | 13,348 | 27,624 |
| Phase 5 default | 416,780 | **5,108** | 29,804 (+2,180 vs baseline) |
| Phase 5 `GROOVE_STATS=1` | 417,428 | 4,460 | 29,836 |

Stack estimate: about 550 B free (1,080 measured − 388 static − 144 deeper `drums_render` frame). Must be verified with the stats build. If it's under 400 B, shrink `DrumVoice` or move the render buffer.

### Phase 6: parameter locks, drum modulation, drum-view param filtering (done)
- **Parameter locks** (format v3: `GrooveLock {step, lane, field, value}` × 64 per pattern):
  - Hold a step pad and turn the encoder: the selected drum param is locked on that step. Locking an empty step places a hit there.
  - Releasing a step without having locked anything toggles it, so a tap still works as before.
  - A lock set back to the lane value is removed. Clear tapped while holding a step removes its locks; Clear + step deletes the step and its locks.
  - Locked steps pulse on the LEDs. The OLED shows "Step N / k locks", and "Drum L step N" as the page while editing.
- **Drum modulation** (`GrooveMod {lane, field, source, amount}` × 24 per pattern): in the drum grid, the mod-source column (C8) edits per-lane amounts for:
  - knob/CV/LFO A, B, X, Y (`mod_cur`);
  - Pressure (velocity of the hit);
  - Random (unipolar for +, bipolar for −). Env has no effect on drums.

  Modulation is applied at each hit. Hits modulated by A/B/X/Y also follow live every block. Clear-mod (long encoder press) works.
- **Hit parameters**: `lane_eff[l]` = lane + step locks + modulation, used only when locks or mods apply. Plain hits still read the lane live.
- **Drum view filters Plinky-only params**: sound, env, arp/seq, sampler and jitter rows (except the drum-mapped pads and the two Groove pads) can't be selected; the pop-up says "Plinky pads only". The encoder and slider can't edit them either. FX, tempo/swing, LFO/CV rows and the mixer stay.
- **Encoder fix**: drum values (0–255) and mod amounts are stored coarser than the encoder's 1-unit step. Values now round in the direction of the turn, so slow turns change them.
- **Levels**: drum gain ×2 (`OUT_GAIN` 36000). Send scaling halved so sends keep their absolute level. Default kit reverb sends ÷3.
- **Stats build**: the stack is now measured from `_end` + 64 B. The only heap user is newlib's `rand()` state (~30 B), so the old 512 B offset under-reported free stack by about 470 B.
- **Pattern switching (user report)**: logic verified by a host test (save, switch, load, migrate). In the *Plinky pads* view the Preset-mode pattern pads choose Plinky's melody patterns by design.
- **Host test** `groovetest/t.c` (scratchpad): 25 checks of locks, mods, encoder rounding, storage and migration, all passing. A low-level filter sweep over 1,944 configs is stable.
- `GROOVE=0 FACTORY_TEST_JIG=1` is still byte-identical to v0.B4.

| Build | App flash used | Flash free | Static RAM |
|---|---|---|---|
| Baseline v0.B4 | 408,540 | 13,348 | 27,624 |
| Phase 6 default | 419,324 | **2,564** | 30,240 (+2,616 vs baseline) |

**Flash is nearly full.** Next step before new features: compile third-party code (tinyusb, ST HAL) with `-Os`, only when `GROOVE_ENABLE`, so the `GROOVE=0` binary stays identical. Expected gain: several KB.

**Phase 6 feedback**:
- Parameter locks were awkward: with a step held you can't also hold a shift button to choose the param. The user asked to remove them; a simpler approach will come later.
- Drum modulation works well. Drum volume is now good.
- A Plinky-only param (microtone) was still reachable by switching Params pages on the Glide pad.
- Bugs: random LED blinks (top row, left 4 columns), and step presses sometimes fired the drum of that column.

### Phase 7: lock mode removed, touch fixes, flash recovered (done)
- **Parameter locks removed**. Format v4 drops the lock pool (pattern 752 B); v3 patterns get their mod slots moved from offset 912 on load. Step pads toggle on press again.
- **Drum pad hits need confirmation**: pressure > 120, and the same drum row on 2 consecutive string updates (4 ms).
  - Fixes the random blinks: sensor-noise pressure blips at the last-known position, mostly row 1, fired full-velocity hits.
  - Fixes the false drums on step presses: the first reading of a step press can land on a drum row.
  - Light contact never sounds the Plinky voice on drum-model lanes either.
- **Params page switch**: switching Params 1↔2 on a drum pad no longer lands on a Plinky-only param (e.g. Glide → Microtone).
- **Flash**: in Groove builds, ST HAL and tinyusb are compiled `-Os` (not time critical; `GROOVE=0` keeps the original flags and binary). Together with the lock removal this frees ~6 KB.
- Host test `groovetest/t2.c`: 10 checks (toggle, noise rejection, wrong-row first reading, row velocity, mods, v3→v4 migration, save/reload), all passing.
- `GROOVE=0 FACTORY_TEST_JIG=1` is still byte-identical to v0.B4.

| Build | App flash used | Flash free | Static RAM |
|---|---|---|---|
| Baseline v0.B4 | 408,540 | 13,348 | 27,624 |
| Phase 7 default | 413,280 | **8,608** | 29,960 (+2,336 vs baseline) |

**Phase 7 feedback**: stats v7 = cpu 82 % average, peak 92 % (109 % since boot = start-up spike), free stack 872 B (corrected measure). Touch on the steps is much better; some random LED blinks remain.

### Phase 8: decay ranges, drum filters, sample rate reduction, LED fix (done)
- **LED blinks**: `editmode_ui` drew Plinky's play-mode LEDs (including the normal sequencer's loop and playhead on the top row) before the drum grid overwrote them. The LED refresh interrupt sometimes showed the in-between state. Those writes are now skipped while the drum grid shows.
- **Decay ranges** are wider on every model (shorter at 0, a bit longer at max). Snare is less noisy and shorter. Default decays were re-tuned (the kick keeps its length).
- **Lane filter** moved to the **Env2 Level** pad. The **HPF pad** is now a **global drum DJ filter** (`pat.djfilter`, LP ← off → HP, mild resonance) on the dry drums and both sends. An encoder click resets either filter to off.
- **Sample rate reduction** per lane (`DrumParams.srr`, sample & hold down to 1/64 rate) on the **Column** pad. Live, and can be modulated.
- **Format v5**: `DrumParams` is 13 bytes (srr), the lane sound starts at lane offset 3, and the header has `djfilter`. v1–v4 patterns are migrated on load.
- Host test `t2.c`: 14 checks, all passing. Filter stability sweep OK.

### Phase 9: experimental drum models, drum bus compressor (done)
- New models (appended to the stored numbering):
  - **Glitch**: random per hit (resonant noise band, blip pitch and direction, lo-fi stutter); colour = randomness.
  - **Fold**: sine with pitch drop through a sine wavefolder; colour = fold, snap = ring modulation.
  - **Clang**: metallic FM with inharmonic ratios plus a metal layer.
  - **Chirp**: upward laser sweep; colour = speed, snap = FM buzz.
  - **Crush**: folded ring-mod tone and noise, bit-crushed (colour) and rate-reduced.
- New engine pieces: `OSC_FOLD` pass (ring mod + wavefolder), per-voice bit crush, built-in minimum sample rate (`srr0`).
- **Drum bus compressor** (`pat.comp`, formerly a reserved header byte, so old patterns = off):
  - Unprinted pad Params 2 + FX-row feedback pad (`P_RVUNUSED`), "Drum Comp", active whenever Groove is on.
  - Peak detector per block, instant attack (no overshoot), ~100 ms release, gain ramped up smoothly, make-up 85 % of the threshold drop (threshold 0 … −24 dB).
  - Applied to dry and sends.
  - Offline: no clipping at any setting; ~+2 dB RMS at mid settings.
- Filter stability sweep (2,754 configs incl. new models) OK; host test `t2.c` 14/14.
