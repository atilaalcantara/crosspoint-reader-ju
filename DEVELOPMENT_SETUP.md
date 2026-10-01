# CrossPoint Development Setup

## Repository Layout

- `dev` is the local integration branch.
- `upstream` points to `https://github.com/crosspoint-reader/crosspoint-reader.git`.
- `CrossInk` and `crossplay` are sibling reference checkouts; they are not firmware dependencies.

## Host Dependencies

- macOS with SDL2 installed and `sdl2-config` available.
- PlatformIO CLI (`pio`).
- Python available to PlatformIO.

## Firmware Build

```bash
pio run -e x4pro
```

## Host Simulator

The CrossPoint simulator is configured as a native PlatformIO environment. It
uses the pinned simulator revision in `platformio.sim.ini` and writes simulated
storage below `fs_` in the simulator working directory.

```bash
pio run -e simulator_x4
./.pio/build/simulator_x4/program
```

The simulator target is intended for host compilation and UI inspection. It
does not emulate ESP heap capabilities; the host-only `MemoryManager` shim
reports host memory semantics instead.

## Reference Checkouts

- `CrossInk`: `main` at `b25beb13761d5851f98e7eada09aa5e7d430df48`
- `crossplay`: `xteink` at `1c8b52650c95990d4a3244c1e5d238f2677cbd2d`

CrossPlay's simulator scripts are useful references for SDL input scripting,
screenshot capture, and filesystem seeding.
