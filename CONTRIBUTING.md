# Contributing

Thanks for helping. This project aims at a faithful native version of the game, so a few rules matter more than speed. Read the [README](README.md) and [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for building and [docs/STATUS.md](docs/STATUS.md) for where things stand.

## Ground rules

- **Never fake progress.** Do not make guest code return a fabricated success, skip a function, or stub game code to get further. A blocker is fixed by emulating the missing hardware or kernel behaviour, or by binding a correctly identified SDK function.
- **SDK bindings need evidence.** Bind a game function to a runtime handler (`src/burnout_overrides.cpp`) only when its identity is established (RPC SID and function number, referenced strings or semaphore names, library version marker, matching arity) and the handler has the same ABI. Bind every IOP-facing entry point of a library together. Record the evidence in [docs/SDK_FUNCTIONS.md](docs/SDK_FUNCTIONS.md) and the expected handler in `tests/native/burnout_overrides_tests.cpp`.
- **Keep upstream changes minimal and documented.** The root folders `ps2xRecomp`, `ps2xRuntime`, `ps2xIOP`, `ps2xTest`, … are a PS2Recomp snapshot (commit in `UPSTREAM.json`). List every change you make there in [docs/CHANGES.md](docs/CHANGES.md); game-specific code belongs in `src/`, `cmake/` and `tools/`.
- **Line endings.** Upstream files use CRLF and `.gitattributes` does not normalise them: keep CRLF when you edit them. Project files (`src/`, `tools/`, `docs/`, …) use LF. Windows batch files use CRLF.
- **No game data in Git.** Never commit disc files, Ghidra output, generated C++ or builds; they live in the work directory. The one exception is `data/functions.ee.csv` (names, addresses and sizes only).
- **Claims need evidence.** When a run changes what the game does, update [docs/STATUS.md](docs/STATUS.md) and the run history in [docs/BOOT.md](docs/BOOT.md) with what was observed, and say what it does not prove.

## Workflow

1. Build the tools and tests (`python tools/project.py configure` then `build`); no game files are needed for this.
2. Run the tests: `python -m unittest discover -s tests/python` and the native programs listed in the README (`ps2x_tests` from the repository root). CI runs them with GCC, Clang and MSVC.
3. If you have the disc, check the game itself with a bounded run, e.g. `python tools/project.py run --headless --seconds 60 --status-ms 2000 --log mytest.log --tail 120`. Exit code 124 only means the time limit was reached.
4. Add a test for new non-trivial logic (runtime, IOP, recompiler or tools).

Changing a header included by the generated code (for example `ps2xRuntime/include/ps2_runtime.h`) rebuilds about 51,000 files; prefer changes in `.cpp` files.

## Commits and pull requests

- Everything in the repository is written in English: code, comments, docs and commit messages.
- Commit messages start with the area: `runtime:`, `iop:`, `recomp:`, `game:`, `tools:`, `tests:`, `docs:`, `ci:`, followed by a short summary in the imperative; the body explains why.
- Keep pull requests focused, and mention which tests you ran and, if relevant, which game run you observed.

## Reporting a problem

Open an issue with your OS, the commit (`git rev-parse --short HEAD`), the command you ran and the log printed after `Log:` (normally `C:\bdr-work\local\logs\run.log` or `~/bdr-work/local/logs/run.log`). Do not attach game files.
