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

**M2: recognition** of lasso-selected ink → the dock's text. Engine to be chosen (open source:
e.g. Seshat or an ONNX model with OSS weights); points already carry pressure and timestamps.

**Later:** parameters with sliders, implicit `f(x,y)=0`, parametric/polar.
