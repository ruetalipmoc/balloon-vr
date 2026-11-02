# Balloon VR

A room-scale OpenXR hot-air balloon. You stand in the gondola — the tracked
play space is only the basket — and fly by pulling two handles that hang from
the ceiling on cables.

* **Red T-bar** — fires the burner. Hold it down and the envelope heats up,
  buoyancy rises, and you climb.
* **Green ring** — opens the crown vent. Hold it down and hot air spills out,
  so you descend.

Either hand can grab either handle (squeeze / grip, then pull down). The
handle springs back when you let go, and the burner or vent stays on only
while the pull is past a small threshold.

On the **left wrist** a flat instrument shows:

* course and horizontal speed
* vertical speed
* altitude above the ground under the basket, and height above the world datum
* wind direction and strength at -50 m, here, +50 m and +100 m

The landscape is generated from a fixed seed as you drift: rolling ground,
river valleys and lakes, and broad regions of farmland (square fields),
forest, open country and small towns. Detail steps down with distance, easing
onto the next grid before it switches. The farthest ground is a single quad
per chunk and drains to a blue haze, then into the sky.
There are no art assets — meshes, the wicker basket, the envelope and the
watch face are built in code.

If no OpenXR runtime is running, the same simulation opens in a window.

## Controls

| Input | Effect |
| --- | --- |
| Grip + pull red handle | Burner, climb |
| Grip + pull green handle | Vent, descend |
| `F` or left mouse (desktop) | Pull the red handle |
| `G` or right mouse (desktop) | Pull the green handle |
| Mouse (desktop, after a click) | Look around the basket |
| Look down, or `C` (desktop); turn the left wrist (headset) | Show the watch |
| Roll the wrist, or `V` | Wind page of the watch |
| `R` | Recenter your head over the basket |
| `Esc` | Quit |

`./balloon-vr --desktop` skips OpenXR entirely.

```bash
./balloon-vr --desktop --view 500 --lod 80,160,280,420
```

`--view` is how far the ground is drawn, in metres (default 704, max 1536). The blue haze stretches with it, so a shorter view fades sooner and a longer one stays coloured further out. `--lod` is four ascending distances where the mesh steps down (default `100,200,340,520`). Past the last one each chunk is a single quad. A step narrower than one chunk (64 m) still joins, but the change is sharper.

The stage origin is recentered onto the middle of the basket floor the first
time a head pose arrives. The basket is about 1.2 m by 1.0 m. Stay inside the
rim; the world scrolls past a stationary gondola, which is what room-scale
tracking can actually do.

## Build (Linux, X11)

The OpenXR path uses the GLX graphics binding, so the app wants an X11
session (`DISPLAY` set). Wayland-only sessions fall back to the desktop window.

```bash
sudo apt install build-essential pkg-config libraylib-dev libopenxr-dev \
    libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev libxinerama-dev \
    libxcursor-dev libxext-dev libasound2-dev
make
make test
./balloon-vr
```

If those packages are not in your distro, the bootstrap script downloads
raylib 5.5 and the OpenXR 1.1 loader and builds them under `third_party/`
(that directory is gitignored):

```bash
sudo apt install build-essential cmake curl tar \
    libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev libxinerama-dev \
    libxcursor-dev libxext-dev libxrender-dev libxfixes-dev libasound2-dev
./scripts/bootstrap-deps.sh
make
```

You need a runtime to actually see the headset image: Monado, SteamVR, or a
vendor runtime that exposes `XR_KHR_opengl_enable`. The loader alone is enough
to compile. Without a runtime the process prints a line and keeps the desktop
view.

Some runtimes reject an OpenGL 3.3 context. Rebuild raylib with
`GRAPHICS=GRAPHICS_API_OPENGL_43` (see the script) if session creation says the
context is too old.

## Layout

| File | Role |
| --- | --- |
| `src/balloon.c` | Heat, buoyancy around the neutral temperature, wind drift, landing |
| `src/handles.c` | Cable handles, grab, spring return |
| `src/wind.c` | Altitude-sheared wind field |
| `src/terrain.c` | Deterministic height and moisture |
| `src/terrain_mesh.c` | Chunk meshes, aggressive LOD, props |
| `src/scene.c` | Envelope, basket, burner, vent, particles |
| `src/watch.c` | Wrist instrument |
| `src/xr_glx.c` | GLX binding for the current raylib context |
| `src/xr_session.c` | Instance, swapchains, stereo frame, controller actions |
| `src/main.c` | Desktop loop and VR loop |

World axes: **+X east, +Y up, +Z north**. Basket space is the tracked room;
the gondola floor is y = 0 there, and the terrain is shifted by the balloon
position so the basket never leaves the play space.
