# Native VU1 programs

In a race, the GS worker thread spends most of its time running the game's VU1 microprograms: the compiled programs reproduce every instruction pair and its timing (about 1.6 million pairs a frame). A native program would do the work of one microprogram directly: read the same VU1 data memory (the vertices VIF1 unpacked, the constants the game uploaded) and send the same GIF packets to the GS, without stepping through the microcode. The GS emulation, the game code and everything before the MSCAL stay as they are.

Burnout 3 projects showed that the world and prop programs, which Burnout 3 shares with Dominator, can be drawn this way (UwUCammie/Burn3, MIT, "native command stream"; shervinemp/burnout3-Wii, GPL-3.0, docs/native-renderer.md). Their code targets their own runtimes; what carries over is the knowledge of the data layouts (VU1 constants 1005 to 1023, vertex formats per material kind) and the method.

## Rules

- A native program replaces a microprogram only where its output is identical: the GIF stream it produces must be byte for byte the one of the compiled microprogram, for the same inputs. Anything it does not handle (another image, another entry, a flag it does not know) falls back to the compiled microprogram.
- Every native program first runs in shadow mode: the microprogram still runs and draws, the native one runs on a copy of the inputs, and the two GIF streams are compared. Only programs with zero mismatches over the reference runs (race, car select, crash replays) may draw.
- Off by default behind a run-time switch until it has been verified on every reference run; it never changes what players get before that.

## Steps

1. Measure which programs cost the most in a race, how often they start and how much work each start does (this page, below).
2. Shadow mode: run a native program beside the microcode on copies of its inputs and compare the results, without drawing anything.
3. Native programs, one at a time, most expensive first, each through shadow mode before it draws.

Steps 2 and 3 were first planned as hand-written decoders of each program's inputs. A translator of whole microprograms (below) gives native programs that are exact by construction, without decoding any data layout, so it came first; decoders stay the way to go further where a translation is not fast enough.

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

## Steps 2 and 3: whole programs translated to C++

`tools/translate_vu_programs.py` turns a VU1 microprogram, from the MSCAL address that starts it to its E bit, into one C++ function (`runtime/ps2_vu1_native.h`). It follows `VU1Interpreter` cycle by cycle while it translates: a pair waits until the VF and VI values it reads are written, so the registers take their values in program order and the C++ writes each result at once; what does not wait is modelled where it is visible: MAC and clip flags land 4 cycles after their instruction, Q 7 cycles after a DIV or SQRT and 13 after an RSQRT, P 12 or 18 cycles after its EFU instruction (`ESQRT`, `ERCPR`, `ERSQRT`; the EFU takes the next one a cycle before), and a conditional branch right after an integer instruction that wrote its register reads the old value. A block of code is translated once for each pipeline state it can be entered with. The one timing known only when the program runs is how long an XGKICK waits for the previous packet: the C++ counts its cycles and takes the block made for the wait it finds. MAC flags are computed only for the instructions whose flags something reads (7 of the sky dome's 385).

The arithmetic is the interpreter's: fused product sums, rounding toward zero, denormal operands and results as zeros (the MXCSR does the three), operands with exponent 255 read as FLT_MAX. The translator stops on what it does not model rather than guess: the D and T bits, status flag instructions, a branch in a delay slot, a register jump whose targets it cannot bound.

The ops (`runtime/ps2_vu1_native_ops.h`) need SSE4.1 for blends and FMA3 for the fused product sums, on x86-64; elsewhere the native programs are compiled out. GCC and Clang compile only the translations with `-msse4.1 -mfma`: given to the whole runtime, `-mfma` would let GCC's default `-ffp-contract=fast` fuse its float math and change the interpreter's results. The header holds only always-inline functions and intrinsics, since an inline function a translation instantiated would be compiled for FMA3 and the linker could keep that copy for every file. MSVC needs no option for the intrinsics (its Release build is compiled for AVX2 anyway). On a CPU without SSE4.1 and FMA3, `find` gives no translation and the microcode runs. On November 5, 2026 the 198 recorded starts of the first ten programs (the `vu1_native_tests` captures; the starts at 0x0020 have none) gave, through translations built by g++ 13 on Linux and by MSVC, the same GIF packets, VU1 data memory, registers and cycles.

Register jumps (`JR`, `JALR`): the translator gives each VI register, and each integer word at a fixed VU1 address, the set of values it can hold at each pair, in program order: constants, link addresses, and what the register or word held when the run started. A jump through a register the start provides, such as the `JR vi15` stub that resumes a coroutine, gets the values the program leaves there for the next start: the registers at its E bits, and at the E bit just before the entry, where the game continues with MSCNT; the words its stores leave at that address. The C++ checks those start values before anything else and returns false when one is not in its set: nothing has changed, and the microcode runs that start (shadow mode counts it as left to the microcode). A check holds only on the paths that read the value: an `IBEQ` or `IBNE` of a start value against vi0 tells the paths after it whether that value is zero. The world programs read the variant they jumped to last time from quadword 1013 unless the game set the flag in its w word, in which case they choose a new one first, so they check 1013's y only when w is zero.

`data/vu_native_programs.txt` names the programs to translate by the checksum of their code, and the build translates them from the code images, recorded or rebuilt from `data/vu_programs.manifest` (both give the same C++), into its own tree; the C++ is made from the game's microcode and is never committed. At each MSCAL or MSCNT that finds VU1 idle (the world programs continue with MSCNT), `PS2_VU_NATIVE` decides: unset or `1`, the translation runs instead of the microcode; `0`, the microcode runs; `shadow`, the microcode runs and draws while the translation runs on copies of its inputs, and the GIF packets, VU1 data memory, registers and cycle count of the two are compared when the microcode reaches its E bit (`[vu1 native]` lines at the end of a `BDR_STOP_AT_VSYNC` window, mismatching inputs saved to `PS2_VU_NATIVE_DUMP`). The translations run by default since November 5, 2026, once the ten programs had been compared in four scenes.

Checks: `vu1_native_tests` runs every recorded start of a translated program (`BDR_VU_NATIVE_CAPTURES`, recorded with `PS2_VU_CAPTURE_DIR`) in shadow mode, once with the interpreter and once with the compiled routines, and with `BDR_VU_NATIVE_BENCH=N` times them; `tests/python/test_translate_vu_programs.py` checks the control flow, the branch backup, the XGKICK waits, the register jumps and their start checks, and the Q and P latencies on synthetic code.

What a translation does not reproduce, none of which happened in the starts compared below: a store into a GIF packet while PATH1 still sends it (the translation copies the packet at the XGKICK) and a program longer than its MSCAL's cycle budget (shadow mode counts such starts as not compared; when the translation runs, a warning names the program). The status flags and the R register keep their values.

### The sky dome

698 instruction pairs, 282 blocks, about 8,900 lines of C++ that MSVC compiles in 21 s. Results on November 5, 2026:

- Shadow mode, all starts identical (GIF packets, VU1 memory, registers, cycles), none left unfinished: 33,534 starts in the deterministic race of `cbench.sh` (VSyncs 60,000 to 64,000), 6,771 at the car select (`carsel14300`, cross held into the race) and 10,197 from `cam17740` (VSyncs 17,740 to 19,000). The 13 starts recorded in the race and 20 recorded at the car select are identical under the interpreter and under the compiled routines. Two deliberate errors in the C++, an addition in place of a multiplication and one cycle more in the main loop, were each found in 8 of the 26 runs.
- Time per start in `vu1_native_tests` (200 rounds of the 13 race starts): 47 to 62 µs with the compiled routines, 12 to 17 µs translated, about 3.8 times less; sending the packets costs nothing in the test.
- In the race (VSyncs 60,300 to 61,800): the GS worker took 5.43 and 5.51 s with the microcode, 5.00 and 5.04 s translated, 8% less. The wall time stayed at 7.4 to 7.5 s: at 60 frames a second the GS worker is not what this race waits for; it is at high frame rates, where it is busy all the time.

Three things the comparisons found on the way:

- The car select gave 186 mismatches in 7,293 starts: one lane of a vertex the program writes back, a denormal where the microcode stores zero. The program adds VF0's x (0) to a value loaded as an integer bit pattern, which the VU reads as zero; MSVC, knowing VF0, compiled `x + 0` as `x`, even with `/fp:strict`. The translation now hides VF0, the I immediates and MFIR results from the compiler (`ops::opaque`), and the file is compiled with exact floating point.
- The first build took 37 µs a start: the exact fallback ran for every product sum whose result was zero. With flushing to zero done by the MXCSR, a fused result is what the interpreter keeps in every case, and only results with exponent 255 go another way.
- `find` first hashed the program's code at each microcode upload, 1.6% of the GS worker; it now compares the bytes, once per upload.

### The world, vehicle, prop, spark, trail and GtIm3d programs

Nine more programs translate since November 5, 2026: the four world programs (`g3_bfc_adc`, `g3_bfc_adc_s`, `g3_gen_adc`, `g3_gen_adc_s`), the vehicles, the props (`g3_instanced`), the sparks, the trails and `GtIm3d`, 205 to 1,209 instruction pairs each, about 83,000 lines of C++ for the ten. They needed the register jumps above, `XTOP` and `XITOP`, `SQRT`, `RSQRT`, the EFU instructions with `WAITP` and `MFP`, and a start from MSCNT as well as MSCAL. Results:

- Shadow mode, every start identical, none unfinished, none left to the microcode: 3.72 million starts in the deterministic race (VSyncs 60,000 to 64,000; 1.38 million of them the vehicles'), 1.47 million at the car select (`carsel14300` to VSync 17,000) and 0.48 million from `cam17740` (VSyncs 17,800 to 19,000). From `user_quick` at 60 Hz (`hfr_race.sh`, VSyncs 57,100 to 62,600, the race with its crashes), 3.85 million starts identical; 11 of the sky dome's 35,991 ran past their MSCAL's cycle budget and were not compared.
- In the race (VSyncs 60,300 to 61,800), with the microcode and translated, twice each: the GS worker took 7.38 and 5.46 s, then 4.17 and 3.51 s; the VU1 instruction pairs run as microcode fell from 2.47 billion to 0.24 billion. The wall time stayed between 7.5 and 9 s, set by the game thread at 60 frames a second.
- At 300 Hz (`hfr_race.sh` from `user_quick`), race seconds: 171 frames a second on average (109 to 241) with the microcode, 228 (142 to 302) translated; the game thread waited for the GS worker 31% of the time, then 1.3%.

What the comparisons found on the way:

- The world programs continue with MSCNT, which did not go through the hook in `VU1Interpreter::execute`: the translations now start in `run`, from any idle start.
- `GtIm3d`'s translation, 18,000 lines in one function, came out of MSVC unoptimized (warning C4883) with a 3 MiB stack frame, and the GS worker thread overflowed its stack at the first start. The translations are compiled with `/d2OptimizeHugeFunctions` and spread over eight files: frames of 1.1 to 2.5 KiB.
- 739 of 63,545 `GtIm3d` starts ended with another vf9: its last loop iteration divides by a word read past the end of its input, with exponent 255, which the interpreter reads as FLT_MAX. The translated DIV now reads its operands as the interpreter does.
- With the first start checks, a fifth of the world program starts went to the microcode: the game had set the new-variant flag in 1013.w and left 0 in 1013.y. The checks now apply only on the paths that read 1013.y.

### Starts at 0x0020

With the ten programs translated, 0.24 billion pairs a race still ran as microcode, 8.5% of the VU1 time before translation, and nearly all of it was the same programs started at 0x0020 instead of their stub: the game runs a four-pair setup at 0x0000 with an MSCAL, which ends with an E bit at 0x0010, then continues at 0x0020 with an MSCNT. `data/vu_native_programs.txt` translates the four world programs, the props and the vehicles a second time from 0x0020 (`g3_bfc_adc_0020` and so on): their code from 0x0020 to the stub has its own checksum, and `find` already chooses among programs sharing a start address by comparing their code. In shadow mode, 0.80 million of these starts were identical in the deterministic race and from `user_quick`, and 0.25 million at the car select and from `cam17740`. In the race the pairs left to the microcode fell to 71 million and the GS worker took 3.0 s instead of 4.2 s on a quiet machine (5.3 s against 9.5 s with the microcode on a loaded one).

### VU0 programs

Translated, the VU1 programs leave the GS worker less to do than the game thread; the game thread's VU0 microprograms, which the EE starts with VCALLMS, took about 9% of its busy time in a race, plus 4% calling them. The same translator handles them: a line of `data/vu_native_programs.txt` that ends with `vu0` is a VU0 program, translated from the VU0 code images. VU0 has 4 KiB of code and 4 KiB of data, so jump targets wrap at 0x0FFF and data addresses at quadword 0xFF (the memory helpers take the size as a template argument), and the VU1-only instructions (`XGKICK`, the EFU instructions, `WAITP`, `MFP`) stop a VU0 translation. Like the VU1 ones, the translations keep the status flags as they were; the EE could read VU0's through `CFC2`, but the game reads only vi0 to vi8, FBRST and VPU-STAT that way. `VU1Interpreter::run` starts a translation at a VCALLMS that finds VU0 idle as at an MSCAL on VU1, and `find` keeps the answer for each address until the microcode changes: VU0 starts thousands of times a frame.

26 VU0 programs translate, 25 of which run: in shadow mode, all their starts were identical, none unfinished and none left to the microcode, 9.95 million in the deterministic race (VSyncs 60,000 to 64,000), 9.78 million from `user_quick` at 60 Hz (VSyncs 57,100 to 62,600), 4.59 million at the car select (`carsel14300` to VSync 17,000) and 2.64 million from `cam17740` (VSyncs 17,800 to 19,000). The 26th, `vu0_0708_5d74`, started in none of these scenes and stays microcode until it has been compared. The 415 starts of 16 of the programs recorded in the race give the same results in `vu1_native_tests`, under the interpreter and under the compiled routines. In the race's game-thread profile (VSyncs 60,300 to 64,300), the samples spent in VU0 work and its calls fell from about 800 to 426, of about 6,150 busy.

### Next

`GtIm3d` started at 0x0098 (0.6%), `G3_Gen_Line` (0.5%, its image was not among the recordings) and the small VU1 programs left can follow the same way. On the game thread, the VU0 calls' own cost then fell from 151 to 71 samples of about 3,300 busy: `VU1Interpreter::execute` starts a translation without the interpreter's set-up, and the thread runs with the MXCSR the translations need (docs/CHANGES.md). Its largest shares are now the compiled EE code (39%), the SPU2 mixing (10%), the IOP interpreter (9%), the scheduler's checkpoints (8%) and the copies of DMA chains for the GS worker (7%, about 4 MiB a frame at memory speed). These game-thread savings did not move the 300 Hz race (239 to 246 frames a second over the same VSyncs, before and after, within run-to-run noise): there the frames were lost to the game's own waits between VBlanks. Those waits came from the EE cycles of a VBlank, 60 / rate of a console frame: charged a console frame's work, the same race draws 297 frames a second at 300 and 323 at 360, and the game thread becomes the limit ([FINDINGS.md](FINDINGS.md) E023).
