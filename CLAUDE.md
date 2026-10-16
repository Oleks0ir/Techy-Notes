# Techy-Notes

Native pen notebook. Product spec and architecture: README.md. Original idea: VISION.md.

## Stack
C++20, Qt 6 Widgets, CMake. Toolchain on this machine: MSYS2 MinGW64 (Qt from pacman); keep code MSVC-compatible.
No web tech, no new dependencies without asking.

## Build & check (PATH must include `C:\msys64\mingw64\bin`)
- `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build`
- `./build/selftest.exe` — must pass after model changes.
- `./build/inputtest.exe -platform offscreen` — must pass after canvas changes (still verify on the tablet).

## Conventions
- Minimal code: prefer Qt built-ins over custom code; keep the small file layout from README.md.
- Mark deliberate shortcuts with a `ponytail:` comment naming the limit and upgrade path.
- World units are millimetres; pages are (col,row) cells, may be negative.
- The sheet file format is versioned (`QDataStream` + version int): bump the version on any change.
- Pen/touch behavior must be verified on real tablet hardware, not only with a mouse.
