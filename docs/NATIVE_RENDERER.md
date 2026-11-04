# Native VU1 programs

In a race, the GS worker thread spends most of its time running the game's VU1 microprograms: the compiled programs reproduce every instruction pair and its timing (about 1.6 million pairs a frame). A native program would do the work of one microprogram directly: read the same VU1 data memory (the vertices VIF1 unpacked, the constants the game uploaded) and send the same GIF packets to the GS, without stepping through the microcode. The GS emulation, the game code and everything before the MSCAL stay as they are.

Burnout 3 projects showed that the world and prop programs, which Burnout 3 shares with Dominator, can be drawn this way (UwUCammie/Burn3, MIT, "native command stream"; shervinemp/burnout3-Wii, GPL-3.0, docs/native-renderer.md). Their code targets their own runtimes; what carries over is the knowledge of the data layouts (VU1 constants 1005 to 1023, vertex formats per material kind) and the method.

## Rules

- A native program replaces a microprogram only where its output is identical: the GIF stream it produces must be byte for byte the one of the compiled microprogram, for the same inputs. Anything it does not handle (another image, another entry, a flag it does not know) falls back to the compiled microprogram.
- Every native program first runs in shadow mode: the microprogram still runs and draws, the native one runs on a copy of the inputs, and the two GIF streams are compared. Only programs with zero mismatches over the reference runs (race, car select, crash replays) may draw.
- Off by default behind a run-time switch until it has been verified on every reference run; it never changes what players get before that.

## Steps

1. Measure which programs cost the most in a race, how often they start and how much work each start does (this page, below).
2. Shadow decoders: for the most expensive programs, read their inputs at each start (vertex count, formats, constants) and check that a decoder understands every start, without drawing anything.
3. Native programs, one at a time, most expensive first, each through shadow mode before it draws.

## Step 1: cost of each program in a race

`PS2_VU_PROGRAM_STATS=1` together with `BDR_STOP_AT_VSYNC=from,to` prints, at the end of the window, one line per VU1 program: its microcode image (FNV-1a of the 16 KiB, as `run --vu-capture` names the `vu1-<hash>.code` recordings), its entry address, how many times it started, the runs and instruction pairs it took, the share the interpreter ran, its VU cycles, the host time spent in it and the GIF bytes it kicked. The host time includes the XGKICK submissions.

Burnout's VU1 programs are coroutines: an MSCAL enters a two-pair stub (`JR vi15` and its delay slot) placed near the end of the program, which jumps back to where the previous batch stopped, nearly always the program's main loop at 0x0020. The report therefore names such a start by its stub and its jump target (`1548>0020`), and the routine column is the compiled routine at the target, which is shared by every image holding the same program.

Measured on November 4, 2026 over VSyncs 60,300 to 61,800 of the deterministic six-car race (`cbench.sh`, `urace60000`), on a quiet machine: 1.63 million program starts (1,084 a frame), 2.47 billion instruction pairs, 5.8 s of host time in VU1 programs, 3.7 GiB sent to the GS. All of it ran compiled. Collecting the statistics adds 10 to 20% to the GS worker's CPU time, mostly hashing each microcode upload, which falls outside the timed runs.

| Program | Share of VU1 host time | Starts per frame | Pairs per start | KiB sent to the GS per start |
|---|---|---|---|---|
| Vehicles (`B4VehicleRendererVU1`) | 19.6% | 350 | 1,184 | 2.6 |
| World, `G3_BFC_ADC_S` (Burnout 3's p3) | 16.4% | 149 | 1,725 | 1.5 |
| World, `G3_BFC_ADC` (Burnout 3's p10) | 15.2% | 162 | 1,436 | 1.8 |
| Sky dome | 12.6% | 8 | 22,417 | 7.8 |
| Props, `G3_Instanced` (Burnout 3's p14) | 9.5% | 50 | 2,791 | 12.9 |
| World, `G3_Gen_ADC` (Burnout 3's p11) | 9.3% | 101 | 1,364 | 1.7 |
| Sparks | 9.2% | 18 | 9,927 | 11.6 |
| World, `G3_Gen_ADC_S` (Burnout 3's p4) | 2.5% | 18 | 1,631 | 1.6 |
| Trails (`TrailRender`) | 1.5% | 79 | 284 | 0.1 |
| `GtIm3d` | 1.3% | 27 | 433 | 0.4 |
| World, `G3_Gen_Line` | 0.5% | 6 | 893 | 2.1 |
| Others | 2.6% | 117 | 244 | 0.2 |

How the programs were named (all names are likely, not proven):

- The four world programs and the prop program run in microcode images byte-identical to Burnout 3's, which Burn3 names (p3, p4, p10, p11, p14, from its census hash of the 16 KiB image).
- A stub sits 5 to 8 pairs before the end of its program. The stub addresses give program lengths that match, within that margin, the sizes of the VU1 labels of a Burnout Revenge prototype (July 14, 2005; symbol export in u0rhs/burnout3-symbols, read as reference only): `G3_BFC_ADC` 688 instructions (stub at 0x1548, pair 681), `G3_Gen_ADC` 618 (0x1318), `G3_BFC_ADC_S` 810 (0x1910), `G3_Gen_ADC_S` 738 (0x16E0), `G3_Gen_Line` 528 (0x1048), `G3_Instanced` 482 (0x0EE8), `TrailRender` 534 (0x1070), `GtIm3d` 1234 (0x2650), `B4VehicleRendererVU1` 674 (0x14E0). Burn3's Burnout 3 vehicle program also has its entry at pair 0x29C, byte 0x14E0.
- The sky dome program reads two blocks of 9 quadwords at 11 and 111 (the prototype's `CB4SkyDomeHemisphereParams` is 0x90 bytes) and produces about 22,000 pairs of work from them each start: its geometry is generated, not unpacked. The spark program reads records of 6 quadwords (`CSparkFrameData`).
- The first GIF tag each program kicks: the world, prop, vehicle and trail programs draw Gouraud-shaded, textured, alpha-blended triangle strips with ST, RGBAQ and XYZF2 per vertex; the sparks draw textured strips with ST and XYZF2 only; `G3_Gen_Line` draws lines; the sky dome starts with A+D register writes.

## What to replace first

1. The sky dome (12.6%): one image, eight starts a frame, and no vertex stream to decode, since it builds its geometry from two small parameter blocks and the view constants. It is the shortest way to build and prove the shadow-mode comparison.
2. The world programs (43.9% together): five variants of one design in images identical to Burnout 3's, whose inputs Burn3 documents (`native_cmd_protocol.hpp`, `slus_21050_strip_geometry`).
3. The vehicles (19.6%): Burn3 (MIT) ports Burnout 3's vehicle program bit for bit with a verify mode; Dominator's is the Revenge-engine variant, with its own constant layout (it reads quadwords 918 to 926 and 1013 to 1019).
4. Props (9.5%) and sparks (9.2%).
