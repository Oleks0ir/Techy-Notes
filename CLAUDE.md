# Techy-Draw

Native pen notebook. Product spec and architecture: README.md. Original idea: VISION.md.

## Stack
C++20, Qt 6 Widgets, CMake, MSVC. No web tech, no new dependencies without asking.

## Build & check
- `cmake -B build -DCMAKE_PREFIX_PATH="$env:QT_DIR"` (QT_DIR from `.env`)
- `cmake --build build --config Release`
- `./build/Release/selftest.exe` — must pass after model changes.

## Conventions
- Minimal code: prefer Qt built-ins over custom code; keep the small file layout from README.md.
- Mark deliberate shortcuts with a `ponytail:` comment naming the limit and upgrade path.
- World units are millimetres; pages are (col,row) cells, may be negative.
- The sheet file format is versioned (`QDataStream` + version int): bump the version on any change.
- Pen/touch behavior must be verified on real tablet hardware, not only with a mouse.
