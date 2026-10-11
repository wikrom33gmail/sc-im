# Repository Guidelines

## Project Overview

**sc-im** ("Spreadsheet Calculator Improvised") is an ncurses-based, vim-like spreadsheet calculator written in C. It descends from `sc` (James Gosling & Mark Weiser; later mods by Chuck Martin). Current release: **v0.8.5**.

Capabilities: vim-style movement/editing of cell content, UNDO/REDO, up to 65,536 rows × 702 columns (expandable), CSV/TAB/XLSX import+export, ODS import, Markdown export, per-cell triggers, and Lua scripting. It runs two ways: an interactive ncurses TUI, or headless via `--nocurses` for piping command scripts.

## Architecture & Data Flow

Single-process C app. Entry point is **`src/main.c`** (`main()` at `main.c:169`). Startup order: signal handlers → locale → build config dictionary + parse argv → (unless `--nocurses`) ncurses init via `ui_start_screen()` → optional Lua init (`doLuainit()`, guarded by `#ifdef XLUA`) → `create_structures()` (marks, yanklist, dependency graph) → allocate `session` + empty workbook → `load_rc()` **after** structures exist (deliberate ordering) → load input file → drain piped stdin into the interpreter → main loop.

The data model is a hierarchy of `extern` globals:
```
struct session *session  →  roman (workbook/doc)  →  sheet  →  ent (cell)
accessed as: session->cur_doc->cur_sh   (e.g. trigger.c:93)
```

**Request flow (input → dispatch → mutation → recalc → redraw):**
1. **Input** — main loop calls `handle_input(buffer)` (`main.c:340`) for curses, or reads a stdin line and `send_to_interp()` (`main.c:345`). `input.c` parses keys/multipliers/timeouts into a command buffer.
2. **Dispatch** — `exec_single_cmd()` (`input.c:335-354`) switches on `curmode` → `do_normalmode` / `do_insertmode` / `do_commandmode` / `do_editmode` / `do_visualmode` (in `cmds/cmds_*.c`).
3. **Mutation** — handlers build an interp line (e.g. `"let A1 = 5"`) and call `send_to_interp()` (`cmds/cmds.c:1268-1284`), which sets the parser input and calls **`yyparse()`** (bison, from `gram.y`). Grammar actions invoke interp semantics (`let`, `label`, `eval` in `interp.c`) that mutate the cell and update the dependency graph.
4. **Recalc** — `EvalRange`/`EvalAll` re-evaluate affected cells through the dependency graph (`graph.c`). WRITE triggers fire on mutation; READ triggers fire on access via `ATBL`.
5. **Redraw** — TUI layer redraws from sheet data: `ui_update(TRUE)`, `ui_show_header`, `ui_print_mode`, `ui_show_celldetails` (`tui.h:74-81`).

### Key modules
|Module|Purpose|
|---|---|
|`src/main.c`|Entry point, startup orchestration, main loop, `exit_app()`, signal handlers.|
|`src/sc.h`|Core header — defines `session`, `roman`, `sheet`, `ent` (cell), `enode`, `range`; included nearly everywhere.|
|`src/gram.y` → `y.tab.c`/`gram.c`|Bison grammar generating `yyparse()`; actions call interp semantics.|
|`src/interp.c`|Expression interpreter *semantics*: `eval`, `seval`, `let`, `label`, `getent`, `lookat`, `copy`, `yankr`, `mover`.|
|`src/cmds/*.c`|Per-mode command dispatch (`do_<mode>mode`) and cell content entry.|
|`src/input.c`|Keyboard input: `handle_input`, multipliers, timeouts, `exec_single_cmd()`.|
|`src/graph.c`|Dependency graph for formula evaluation (CamelCase API).|
|`src/undo.c`|Undo/redo linked list (`#ifdef UNDO`).|
|`src/xmalloc.c`|Memory wrappers: `scxmalloc/scxfree/scxrealloc`, `fatal()`.|
|`src/vmtbl.c`|Cell-table accessors & growth; **`ATBL()`** is the cell accessor that fires READ triggers.|
|`src/trigger.c` + `trigger.lua`|Per-cell READ/WRITE triggers (Lua/C/shell).|
|`src/tui.c`|ncurses TUI layer (`ui_*` prefix); also hosts yacc's `yyerror`.|

## Key Directories

- **`src/`** — all C source, the bison grammar (`gram.y`), and the only build entry point (`src/Makefile`). Subdirs: `cmds/`, `utils/`.
- **`tests/`** — bash test harness (assert.sh framework) + `.sc`/`.sh` test pairs.
- **`examples/<mode>/`** — runnable examples per feature: `lua/`, `xlsx/`, `nocurses/`, `markdown/`, `plot/`, `tex/`, `Module/`, `external/`, `gui_example/`, `ui_example/`, `sc/`. Each pairs a `.sc`/`.lua` script with a short readme.
- **`themes/*.sc`** — color themes loaded at runtime via the `theme <name>` command (`REDEFINE_COLOR` / `color "type=… fg=… bg=…"` directives).
- **`doc/grammar_yacc_tools/`** — dev-only tool (wraps y2l + pandoc) that extracts sc-im's command/setitem/term grammar from `src/gram.y`. Not part of the build.
- **Root docs**: `Readme.md`, `CHANGES`, `WIKI` (→ GitHub wiki), `HELP` ("It's all in :help" — help is in-app), `BUGS`, `KNOWN_ISSUES`, `USER_REQUESTS`.

## Development Commands

There is **no top-level Makefile and no CMake** — `src/Makefile` is the only build entry point. Run everything from the repo root:

```bash
make -C src                 # build → produces src/sc-im  (default target `all`)
make -C src clean           # removes objects, generated headers, gram.c, binary — AND deletes ../docs/ (destructive)
make -C src install         # installs to /usr/local (honors DESTDIR); also bin/scopen + help/man/themes
make -C src docs            # runs doxygen Doxyfile
bash tests/run_all_tests.sh # run the full test suite (binary must already be built; valgrind on PATH)
```

- Override compiler/tools: `CC`, `YACC`, `SED` are overridable Make vars (`src/Makefile:23-25`). e.g. `make -C src CC=clang`.
- Generated files (gitignored): `y.tab.c/h`, `gram.c`, `experres.h`, `statres.h` — produced by bison + sed scripts (`eres.sed`, `sres.sed`).

### Feature flags (pkg-config detection at build time, `src/Makefile:101-150`)
Detection only runs when `pkg-config` is present. A feature is compiled in **only if its library resolves** — otherwise it's silently absent from the binary.

|Define|pkg-config dep|Effect|
|---|---|---|
|`-DXLS`|`libxls`|XLS file reading|
|`-DODS -DXLSX`|`libzip libxml-2.0`|ODS + XLSX import|
|`-DXLSX_EXPORT`|`xlsxwriter`|XLSX export|
|`-DXLUA`|highest-versioned `lua-5.x`, else `luajit`|Lua scripting (links `-Wl,--export-dynamic`)|
|`-DGNUPLOT`|`which gnuplot` probe|Plot support|

Override the Lua pkg name explicitly: `make -C src LUA_PKGNAME=lua-5.4`. Other toggles (`-DUNDO`, `-DMOUSE`, `-DAUTOBACKUP`, `-DUSECOLORS`) are compile-time `#define`s you comment out in the Makefile to disable.

## Code Conventions & Common Patterns

- **Naming**: snake_case throughout (`send_to_interp`, `lookat`, `create_undo_action`). Prefix conventions: TUI = `ui_*`; memory wrappers = `scx*` (`scxmalloc/scxfree`); mode dispatchers = `do_<mode>mode`. The graph module is the exception — CamelCase (`GraphCreate`, `EvalRange`). Types are `struct X` (no typedefs except graph's `vertexT/edgeT/graphADT`).
- **Error handling**: macros in `macros.h:96-98` — `sc_error(x,…)`, `sc_debug(x,…)`, `sc_info(x,…)` all route to `ui_sc_msg(msg, TYPE_MSG, …)` (message bar / stderr). Fatal path is `fatal()` (`xmalloc.c:128`) → calls `sc_error` and sets `shall_quit=2`.
- **Quitting**: global `int shall_quit` (`main.c:129`) — 0 normal, 1 `:q`, -1 error/ABRT, 2 `:q!`/SIGTERM. Main loop polls it; cleanup via `exit_app(status)` (`main.c:484`).
- **Memory**: prefer `scxmalloc/scxfree/scxrealloc` for must-not-fail allocations — they call `fatal()` on NULL (`xmalloc.c:67,98,110`). (Some legacy code still uses raw `malloc/calloc`, e.g. session alloc at `main.c:262`.)
- **Cell access**: always via the `ATBL(sh, sh->tbl, row, col)` accessor (`vmtbl.c:294`) — it fires READ triggers before returning; or `lookat()` (`cmds/cmds.c:1296`) to get-or-create a cell. Don't index `sh->tbl` directly in new code.
- **Cell mutation → recalc**: after changing a cell, call `EvalRange(sh,r,c,rf,cf)` (or `EvalAll`) when autocalc is on — see `enter_cell_content` (`cmds/cmds.c:1254-1257`).
- **Includes & guards**: nearly every `.c` starts with `#include "sc.h"` then `"macros.h"`, then feature headers. Subdir files use relative paths (e.g. `cmds/cmds.c`: `"../utils/string.h"`, `"../y.tab.h"`). Feature guards throughout: `#ifdef XLUA`, `#ifdef UNDO`, `#ifdef USECOLORS`, `#ifdef MOUSE`. Doxygen-style `\brief/\param/\return` comments on most functions.

## Important Files

- **Entry point**: `src/main.c` (`main()` at line 169).
- **Core data structures**: `src/sc.h`.
- **Parser/semantics**: `src/gram.y`, `src/interp.c`.
- **Build config**: `src/Makefile` (the only build file; feature-flag logic at lines 101-150).
- **Config/runtime state**: `src/conf.c/h`, `src/utils/dictionary.*`; user rc lives in `$HOME/.config/sc-im/scimrc`.
- **CI**: `.github/workflows/ci.yml` — ubuntu-latest + gcc only; installs valgrind+bison, runs `make -C src` then the test suite. No lint/coverage step.
- **Docs pointers**: `WIKI`, `BUGS`, `USER_REQUESTS` (all point to GitHub); in-app help via `:help`.

## Runtime / Tooling Preferences

- **Language/toolchain**: C, built with gcc or clang (`CC` overridable). Requires **bison** (`YACC`) and **pkg-config**. Links `-lm -pthread`, ncursesw (use `-lncursesw`, not `-lncurses`, for wide chars — see `KNOWN_ISSUES`).
- **Required runtime dep**: ncurses. Optional features (Lua, XLSX/ODS import/export) are enabled only when their libraries resolve via pkg-config at build time.
- **Two run modes**: interactive TUI (default), and headless scripting:
  ```bash
  cat examples/nocurses/export_to_mkd.sc | sc-im --nocurses examples/sc/subtotals.sc
  echo "let A0=1" | ./src/sc-im --nocurses --quit_afterload
  ```
- **Scripting**: Lua via the in-app `lua` command / trigger files; `.sc` command scripts for batch ops (e.g. `EXECUTE "e! mkd …"`).
- **Docs generation** (`doc/grammar_yacc_tools`): needs **pandoc + AWK**, reads `src/gram.y`; dev-only, not part of the build.

## Testing & QA

Bash harness on the standard `lehmannro/assert.sh 1.1` framework (`tests/assert.sh`). Key helpers: `assert <command> <expected-output>` and `assert_iffound_notcond <file> <pattern> [notcond]`.

Each test is a pair in `tests/`:
- **`test<name>.sc`** — *optional* initial spreadsheet state (cell values/formulas), passed as the document argument. Not required when the test only pipes commands.
- **`test<name>.sh`** — harness: sources `assert.sh`, defines a newline-separated command string, pipes it into the binary via stdin with `echo -e`, asserts on stdout.

Canonical invocation (`tests/test-getent-range.sh:8-14,51`):
```bash
NAME=test-getent-range
SCIM=../src/sc-im
VALGRIND_CMD='valgrind --tool=memcheck --track-origins=yes --leak-check=full ...'
. assert.sh
CMD='LET B0 = @sum(A0:@getent(D0,0))\nGETNUM B0'
EXP="1234567890"
assert "echo -e '${CMD}' | $VALGRIND_CMD ${SCIM} ${NAME}.sc --nocurses --nodebug --quit_afterload 2>&1" "$EXP"
```

- sc-im flags used by tests: `--nocurses` (headless), `--nodebug`, `--quit_afterload` (process stdin then exit). stdout is filtered with `grep -v '^$\|Interp\|Change'`.
- **Every test runs under valgrind memcheck**, then asserts on the log: `assert_iffound_notcond ${NAME}_vallog "definitely lost.*bytes" "0 bytes"` (a leak line, if present, must say 0 bytes). `test-replace.sh` additionally checks for no invalid free/delete and zero indirectly-lost bytes.
- **Run all**: build first (`make -C src`), then `bash tests/run_all_tests.sh`. The runner does not build — the binary must exist at `src/sc-im`, and valgrind must be on PATH.

**Adding a test:** create `tests/test-<name>.sh` (name MUST match the `test*.sh` glob in `run_all_tests.sh:10`) plus an optional `tests/test-<name>.sc`. Follow the harness pattern above; end with the valgrind leak checks. To disable a failing test, rename it out of the glob (e.g. `test7.sh-temp-disable`).

**Coverage checklist**: see `tests/tests_to_add` — covers multi-sheet formula dependencies, UNDO/REDO, YANKAREA/PASTEYANKED reference preservation, column/row deletion with undo, and string functions (`@replace`, `@getent`, circular-reference detection).
