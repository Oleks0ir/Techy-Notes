# Techy-Notes

A fast, native pen notebook for Windows (Android/iOS later), in the spirit of OneNote — with a
long-term goal of turning handwritten math into formatted equations and plots (Desmos / Matlab /
Derive inspired). See [VISION.md](VISION.md) for the original idea.

## Download
Windows 10/11 x64: get `Techy-Notes-<version>-win64.zip` from [Releases](https://github.com/Oleks0ir/Techy-Notes/releases),
unzip anywhere, run `techy-notes.exe`. Notebooks live in `Documents\Techy-Notes`.

License: GPL-3.0-or-later ([LICENSE](LICENSE)), Copyright (C) 2026 Oleks. The Windows zip bundles Qt 6 (LGPL-3.0) and other libraries from
[MSYS2](https://packages.msys2.org) as separate, replaceable DLLs; their license texts are in `licenses/`, and their
sources are available from [Qt](https://download.qt.io/official_releases/qt/) and the MSYS2 package pages.

Releasing: push a tag `v*` (`git tag v0.1.0 && git push origin v0.1.0`); GitHub Actions builds, tests and attaches the zip.

## Stack
- **C++20 + Qt 6 Widgets + CMake** (MinGW via MSYS2 or MSVC on Windows).
- No web technologies. Native rendering via `QPainter`, pen input via Windows Ink (`QTabletEvent`).

## Build
Toolchain: MSYS2 MinGW64 (`pacman -S mingw-w64-x86_64-{qt6-base,cmake,ninja,gcc}`).
```powershell
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"   # compiler, CMake, Qt DLLs
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/selftest.exe     # model self-check (asserts)
./build/inputtest.exe -platform offscreen   # simulated mouse/pen/touch on the canvas
./build/techy-notes.exe
```
MSVC + official Qt kit also works: `cmake -B build -DCMAKE_PREFIX_PATH="$env:QT_DIR"` (`QT_DIR` noted in `.env`),
`cmake --build build --config Release`, binaries in `build/Release/`.

## Concepts
| Term | Meaning |
|---|---|
| **Notebook** | A folder. |
| **Chapter** | A subfolder of a notebook. Custom order stored in `order.txt` per folder. |
| **Sheet** | One `.tsheet` file = one seamless canvas. |
| **Page** | A cell (col,row) of the sheet's page grid, A4 portrait by default. Page lines are visual guides only — you can write across them. |
| **Active page** | A page that currently contains ink. Only active pages are exported. |
| **Focused page** | The page under the center of the screen. |
| **Ghost page** | A dashed page shown right of the focused page; becomes real when written on. |

## V1 features

### Tools (top toolbar, always visible)
- **Freehand** — width follows pen pressure; color and width presets.
- **Straight line** — Shift (or toolbar toggle on touch) snaps to 15°.
- **Select** — lasso and rectangle; move, delete, copy/paste (also across sheets), recolor
  (click a color preset while a selection exists).
- **Eraser** — removes whole strokes. The pen's eraser end / barrel button switches to it
  temporarily (button mapping is a setting).
- **Undo/redo** — up to 100 steps.

### Input
- Pen draws, finger pans and pinch-zooms, touch is ignored while the pen is near the screen (palm rejection).
- Mouse: draws; wheel scrolls, Shift+wheel scrolls horizontally, Ctrl+wheel zooms, middle-drag pans.

### Pages
- Pages form a free layout (L-shapes and separate islands allowed; coordinates may be negative).
- **Down:** ink in the bottom third of a page creates an empty page below it.
- **Right:** a ghost page always sits right of the focused page.
- **Anywhere:** ink outside any page creates the pages it touches.
- **"+" buttons** on free page edges appear when the pen/mouse hovers near them (to add empty space ahead of time).
- **Delete:** only empty pages, and only if the layout stays connected.
- **Page setup** (A4 / A5 / Letter / custom, orientation) — only while the sheet is empty.

### Export
- A page that gets ink is appended to the end of the export queue; a page that becomes empty leaves it.
- At export the user reorders the queue (drag-and-drop); the order is saved in the sheet.
- Output: vector PDF, one page per queued page; strokes crossing page lines are clipped per page.

### Notebook UI
- Side panel tree (notebooks → chapters → sheets) with drag reorder, inline rename,
  new sheet / new chapter via context menu.
- Full-screen mode hides the side panel; the toolbar stays.
- Autosave shortly after each change; atomic writes so a crash never corrupts a sheet.

## Architecture
```cpp
struct Pt     { float x, y, p; qint64 t; };                       // mm, pressure, timestamp
struct Stroke { QVector<Pt> pts; QColor color; float width; };    // a line is a 2-point stroke
struct Sheet  { QList<Stroke> strokes; QSet<QPoint> pages; QSizeF page{210,297}; QList<QPoint> exportQueue; };
```
- World units are millimetres. A stroke's pages = pages containing its points (lines are resampled
  first), so diagonal strokes never activate pages they don't cross.
- Rendering: committed ink is cached in one HiDPI-aware pixmap; the live stroke is painted straight
  into it (low latency). During pan/zoom the cached pixmap is transformed and re-rendered when the
  gesture ends.
- Persistence: `QDataStream` with a format version + `QSaveFile`.
- Undo: implicitly-shared snapshots of the stroke list.

| File | Contents |
|---|---|
| `src/main.cpp` | Main window: toolbar, tree dock, full screen, page setup + export dialogs |
| `src/Sheet.h/.cpp` | Model, page layout/growth, activation + export queue, load/save, PDF export |
| `src/Canvas.h/.cpp` | Canvas widget: view transform, pen/touch/mouse input, tools, painting |
| `src/Math.h/.cpp` | Math objects: expression parser, typesetting, `y = f(x)` plots |
| `src/Eval.cpp` | The only ExprTk user (curve evaluation), alone because it takes minutes to compile |
| `src/Ink.h/.cpp` | Handwriting → dock text: stroke grouping, $P symbol matching, layout from geometry; template files |
| `data/symbols.tpl` | Seed symbol templates from HWRT (ODbL, see `data/LICENSE-HWRT.txt`), built in as `:/symbols.tpl` |
| `tools/hwrt2tpl.cpp` | One-off converter HWRT CSV → `data/symbols.tpl` (`cmake --build build --target hwrt2tpl`) |
| `third_party/exprtk.hpp` | [ExprTk](https://github.com/ArashPartow/exprtk) (MIT), vendored |
| `src/selftest.cpp` | Assert-based checks of the model |
| `src/inputtest.cpp` | Canvas checks with simulated mouse, pen and touch input |

## Milestones
1. Window, toolbar, tree, canvas pan/zoom, page lines — tested on a real pen tablet right away.
2. Freehand with pressure, straight line, eraser, undo.
3. Select (lasso/rect, move, delete, copy/paste, recolor).
4. Save/load/autosave, notebook tree, new sheet/chapter.
5. Page growth, activation, export queue, PDF, page setup, page delete, full screen.
6. Pictures (paste, drop, resize, crop, trim), highlighter, snip, pen ring (radial menu on the side button).

## To do
- **PDF import.** Needs Qt PDF: the official Qt installer (Qt 6.10 + "Qt PDF"), since MSYS2 has no
  Qt PDF package. Sheet > Import PDF, or drop a PDF on the notebook tree → the Export page picker to choose
  pages → a new sheet named after the PDF, pages stacked in one column, page size from the PDF's first page.
  Each page is rendered at ~200 dpi into the image store as a locked background (`Stroke::locked`): not
  selectable, not erasable, no picture menu; Clear/Delete page still work. Later: import into the current sheet.

## Deliberately postponed
- Tile cache / GPU rendering — when a 10k-stroke sheet stutters.
- Spatial index — when hit-testing shows up in a profile.
- Stroke smoothing — if pen input looks jaggy.
- Rotating selections; unreferenced images are never removed from `.images/`.
- Pen ring on Android/iOS: the hover side-button state comes from a Windows API.

## Math (in progress, branch `dev`)
Goal: lasso-select a handwritten equation → a **Math dock** shows it formatted and plotted → insert it
onto the canvas. Everything stays GPL-3-compatible open source.

**Milestone M1: typed input** (handwriting recognition comes later and only has to fill the text box)
- **Math dock** (right side; ∑ toolbar button and pen-ring entry): one expression per line, Desmos-style
  (`y = x^2 - 2x`, `sin(x)/x`, `2x`). Below it: the formatted equations and a live plot, pan/zoom with
  pen, finger, mouse.
- Plot: several `y = f(x)` curves, one color each, axes + grid + tick numbers.
- **Insert** places a **math object** at the screen center (80×60 mm), already selected so it can be moved/resized
  right away. Two kinds: plot, equation (typeset).
- On the canvas the object is non-interactive, drawn as vector from its stored text (crisp in PDF).
  Right-click → **Edit** loads it into the dock; **Update** writes it back (one undo step).
- Model: like pictures, a `Stroke` whose `pts[0..1]` is the rectangle, plus `QString math` (expressions +
  view range). Select/move/resize/undo/copy/PDF reuse the picture paths. Sheet format v3.
- Parsing: own recursive-descent parser → tree used for typesetting (fractions, powers, roots,
  functions, laid out with `QPainter`). Evaluation for plotting: ExprTk (MIT, vendored header
  `third_party/exprtk.hpp`).

**M2: recognition** of lasso-selected ink → the dock's text. Lasso → selection menu / pen ring →
**Recognize math** → dock opens with the text; Insert asks **Replace ink** or **Keep ink**.
Points already carry pressure and timestamps.

Engine survey (2026-10, licenses of code *and* weights/training data; the app must stay GPL-3):
| Option | Code | Weights / data | Verdict |
|---|---|---|---|
| Seshat (falvaro/seshat) | GPL-3 ✓, C++, Boost headers + Xerces-C (InkML) | models trained on CROHME: "research purposes only" | code usable, shipped models not |
| CoMER, BTTR, PosFormer (PyTorch → ONNX) | CoMER: no license file (= all rights reserved) | CROHME / CC BY-NC-SA | ✗ |
| Models trained on MathWriting (Google) | — | data CC BY-NC-SA 4.0 (non-commercial) | ✗ (NC conflicts with GPL-3) |
| HASYv2 (symbol images) | — | ODbL, 369 symbols, 32 px images (no strokes) | ✓ for training a symbol classifier, with attribution |
| **HWRT** (Martin Thoma, zenodo 50022; ~90% Detexify data) | — | **ODbL 1.0, online strokes with timestamps**, alphanumerics, Greek, arrows, math symbols (∫ Σ √ …) | ✓ symbol templates / training; ship the subset we use as an ODbL data file + attribution |
| Own engine: stroke grouping + symbol classifier + geometric layout parser | ours, GPL-3 | templates written by the user (+ optionally HASYv2) | ✓ |

All ready-made math recognizers are trained on non-commercial data, so their weights are out. Single-symbol
stroke data with a free license exists (HWRT); whole-expression data does not, so layout (powers, fractions, roots,
∫ limits) is parsed from geometry rather than learned. Templates the user corrects in the dock are added
(personal handwriting improves over time).

Decided: **own engine, HWRT seed templates.** Steps:
1. **Notation (typed first):** Greek names (`alpha` → α, `theta`, …), `int(a, b, f)` / ∫ with limits, `sum`,
   `lim`, `d/dx`, in the parser and typesetting. Plots only use what ExprTk can evaluate.
2. **Templates:** a one-off converter (`tools/`) turns the HWRT CSVs into `data/symbols.tpl` (~100 symbols ×
   ~20 samples, normalized strokes), embedded as a Qt resource; `licenses/HWRT.txt` (ODbL notice + attribution).
3. **`src/Ink.h/.cpp`:** strokes → symbols (grouping by overlap and time) → $P point-cloud matching, top
   candidates per symbol → layout from geometry (baseline, super/subscript, fraction bar, radical, ∫ limits) →
   dock text, plus each line's stroke indices (kept for Review). Selftest feeds it ink built from templates.
4. **UI:** selection menu / pen ring → **Recognize math** → dock: text box (freely editable) + a strip of the
   recognized symbols; tapping one lists the alternatives, a pick fixes the text and becomes a personal template
   (`<notebook root>/.templates.tpl`, used before the seed ones). Insert asks **Replace ink** / **Keep ink**.
   Several symbols can be corrected at once (one character each, or one symbol to merge them); double-tap rewrites
   one, Delete drops stray ones. Recognized text goes in as new lines under the text box's other functions, and
   corrections rewrite only those lines. A dot above a symbol joins it (i, j) whenever it was written. Kept ink stays
   linked to its object: right-click on it → **Edit math**.
5. **Settings > Calibrate handwriting:** 12 prompts (3 each: basics, letters + Greek, functions, calculus), written
   in a row. After each stroke the page is guessed with what was learned so far; each symbol is framed red
   (wrong or < 50%), yellow (50–90%) or green (≥ 90%), with its guess and certainty under it (certainty: how
   clearly the expected symbol beats its closest rival). **Train** learns all symbols of the page and asks for it
   again; only when all are green does it become **Next**. Skip / Stop testing always work. **Train a symbol**
   (there or in Settings): one symbol alone, its ink kept whole, repeated until green.

**Later:** parameters with sliders, implicit `f(x,y)=0`, parametric/polar.

**Future, optional: Review.** A checker for written math: it follows the steps of a derivation (lines in a
lasso selection, top to bottom) and highlights the first one that does not follow from the one before, plus
evaluate/solve (`2^10 =`, derivatives, solve for x). Not planned yet; the modules are kept ready for it:
- recognition returns text **per line with each line's ink stroke indices**, so a checker can mark the ink
  of a wrong step (highlight over those strokes) without knowing about recognition;
- the parser's `MathNode` tree is the one shared representation: plotting, typesetting and a later checker all
  read it (a numeric check needs only ExprTk: compare both sides at sample points; a symbolic one a GPL-compatible
  CAS, e.g. SymEngine).
