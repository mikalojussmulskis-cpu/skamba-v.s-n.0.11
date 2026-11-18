# skamba — Groovebox firmware for Plinky

**skamba** is an unofficial, open-source firmware for the [Plinky](https://plinkysynth.com) touch synth. It adds a **Groovebox mode**: an 8-lane drum machine with a 64-step sequencer that runs alongside the Plinky synth.

- 8 drum lanes, 24 synthesised drum engines (kicks, snares, hats, FM, glitch, LXR-based drums, a 303-style Acid line and a Bass that follow Plinky's scale, sample playback)
- per-lane loop lengths, velocity, step locks, trigger conditions
- whole-drum-mix filter, compressor, drive, bit-crush, delay and reverb sends
- modulation from Plinky's LFOs, knobs, CV, accelerometer, velocity, envelope and randomness
- 24 Groove patterns, saved automatically, separate from Plinky's presets

**Manual:** [docs/skamba/MANUAL.md](docs/skamba/MANUAL.md)

**Install:** download the `.uf2` from the [Releases](../../releases) page and copy it to the Plinky's UF2 drive, like any Plinky firmware update. The start-up screen shows the version (for example `v.s-n.0.11`). To go back, flash an official Plinky firmware the same way.

**Build:** `cd sw/nocube_makefile && make -j8` (needs `arm-none-eabi-gcc`), then convert `RELEASE/plinkyblack.bin` with `uf2conv.py -b 0x8010000 -f 0xff6919`. The Groovebox code lives in `sw/Core/Src/groove.c`, `groove.h`, `drums.c` and `drums.h`; changes to the original Plinky files are marked `GROOVE HOOK`.

### Licence and credits

- skamba is based on the open-source Plinky firmware (MIT) by mmalex and the Plinky community; see the original README below.
- The LXR Drum, LXR Snare, LXR Hat and LXR Cymbal models are ported from algorithms in the Erica Synths LXR-02 firmware (GPL-3.0). Because of this, **the skamba firmware as a whole is distributed under the GNU GPL v3**. See [LICENSE.md](LICENSE.md) and [sw/lxr_license/](sw/lxr_license/).
- skamba is **not made, approved or supported** by the makers of Plinky or by Erica Synths. Please don't ask them for support with it; open an issue here instead. "Plinky", "LXR" and "LXR-02" are used only to describe compatibility and the origin of the algorithms.

---

*The original Plinky README follows.*

# plinky

an 8 voice touch synth - https://plinkysynth.com

Plinky started life as a hobby project that spiralled out of control, was sold for a while on Thonk as a kit, went out of stock during the post-covid chip shortage, and was then rescued by various wonderful people in the Plinky community. Alogn the way, it got open sourced - both hardware and software. You are in the right place for that!

## OPENSOURCE SOFTWARE CREDITS

You can see that the software has largely been stagnant since original release; however in early 2025, a hero by the name of **RJ** stepped up and contributed some incredible improvements, rewriting large parts of the Sequencer and MIDI implementation. Plinky as of version 0.B0 (to be released shortly at the time of writing) is going to be by far the best version of plinky Ever, by far. HUGE thanks to RJ for really pushing plinky closer to its full potential.

Release Notes are available in the [Release Notes](release_notes.md) document for the newer firmware versions.

If you want to get in on the action, join the Plinky Discord (see details at [plinkysynth.com](https://plinkysynth.com) and check out the `#beta-firwmare` channel) or check out the [sw/](/sw/) folder here. Special shout out to **@whocano** in that channel who beta-tested the new fixes from RJ. See, plinky is a team effort? It is truly a special community. Thankyou from the bottom of my heart. 

## OPENSOURCE HARDWARE CREDITS
But plinky is not just software; It's a physical thing!

Many thanks to the people who worked so hard to make opensource plinky a reality, far better than the original version (V1) and (V2) that I stumbled through creating. V3 was a product of the Plinky Discord community, especially:

* **Stijn** from [This Is Not Rocket Science](https://www.thisisnotrocketscience.nl/) routed the original Version 3.
* **Steve / Okyreon** from [denki oto](https://www.okyeron.com/denki_oto/about.php) added the LPZW midi circuit to allow plinky to have on-board midi without the expander.
* **Kay** from [LPZW Modules](https://leipzigwest.org/)  then contributed a circuit to fix the input noise, and then polished everything into a 'Version 3.1', which is the current version linked here. Kay also was one of the original people who helped me with the hardware design of V1 and V2.

If you contributed to the open source hardware and I forgot you, please shout at me @mmalex on the Plinky Discord.
Thanks to all the Plinky Discord members for making Plinky ever better!

Although this repo is all about the OSS Plinky, it is, as ever, available from [Thonk](https://www.thonk.co.uk/shop/plinky-kit-001/) in both prebuilt and kit form, and from other retailers including [Schneidersladen](https://schneidersladen.de/en/plinky/). Thanks to [MakingSoundMachines](https://makingsoundmachines.com/) and [LPZW Modules](https://leipzigwest.org/) for manufacturing those for people who are not looking for the full OSS experience :)


## original release note

For posterity, here is what I wrote at Stijn's coffee table as I released plinky into the public, not knowing what would become of it....


I apologise, its been so long since I looked at this, I am releasing a snapshot of what I had on my harddrive after a couple of years of barely being touched. while I am not actively developing plinky, I hope that by open sourcing it I can stimulate the wonderful community who collaborate on the plinky discord. I also hope to help iterate on some of the more obvious bugs, both hardware and software. 

If you are interested in plinky, please feel free to join our discord community. More information at https://plinkysynth.com


License stuff
--
Please refer to the [license](LICENSE.md) for information including stuff about derived works.

Contributing stuff
--
please see [CONTRIBUTING.md](CONTRIBUTING.md) for information on contributing to this project.

Building and stuff
--
**UPDATE Jan 2025** it is now possible to build and debug plinky using the arm-cortex-debug extension in VSCode. notes continue below:

you need [STM32 cube ide](https://www.st.com/en/development-tools/stm32cubeide.html), its basically the default project so most of the code is in [/sw/Core/Src](/sw/Core/Src), most of the rest of the [/sw](/sw) folder is just the usual auto generated boilerplate mess.

It would be a useful project to update the firmware to build with a more minimal setup; the only significant middleware used relates to USB; while I lean on the STM default project a little, I imagine that the peripheral code could be modified fairly easily.

it should also compile as an emulator for mac and windows, using portaudio and dear imgui. it also compiles for wasm, somehow... I imagine that following the path of the WASM emulator, it would be relatively easy to port plinky to software plugin type platforms.

there is also a [bootloader](/bootloader/) project... I forget exactly how it relates to the final build process. Consider that a TODO to document :)

the hardware is mostly in the [hw/](/hw/) folder, but I was new to kicad so I am not sure if all the library stuff is in the right place.

if there is something missing or something in there that you think shouldn't be in there, let me know and I can sort it out



Thanks again!
--
Many thanks to the plinky community, who have helped plinky evolve and kept the dream alive over a few years. 

* Thanks to Steve from https://thonk.co.uk who encouraged plinky into existence and helped produce the kits; 
* to Kay from https://leipzigwest.org/ for tireless hardware help, encouragment and pre-made plinkys! 
* To Stijn from http://www2.thisisnotrocketscience.nl/ for support, hardware help, and the impetus to opensource this. 
* To MakingSoundMachines from https://makingsoundmachines.com/ for carrying the torch for the new Thonk kits and prebuilt versions, as well as representing Plinky at shows such as Superbooth. 
* to RJ for the incredible bugfixes and rewrites in the firmware that make plinky actually usable as a midi device and touch sequencer. <3
* to Okyeron for V3 hardware work
* to Miunau for the plinky website and continual encouragement, and the original wavetables
* to Meska, LPZW and Grain Blanc for preset packs
* to CrazyEmperor893, SwampFlux, ld, orangetronic, Zifor, tubbutec and many others on the plinky discord for keeping plinky alive and well while I was away.
* to all the people I have missed out, I have a terrible memory, please shout at me if you can see anyone missing from this list who deserves to be here.


Cheers all! 

-- [mmalex](https://twitter.com/mmalex)

