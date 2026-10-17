# backend_lwe — scene rendering with linux-wallpaperengine

Scene (`scene.pkg`) wallpapers are rendered with
[Almamu/linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine)
(lwe) instead of the bundled `src/backend_scene`. Video wallpapers keep using
`src/backend_mpv` and web wallpapers keep using QtWebEngine: only scene
rendering comes from lwe.

## Layout

| path | contents |
| --- | --- |
| `CMakeLists.txt` | builds lwe's `linux-wallpaperengine-core` (as an `ExternalProject`) and the `lwebackend` Qt adapter library |
| `qml_helper/` | `LweSceneViewer`, a `QQuickFramebufferObject` that renders a lwe project into its own scene-sized FBO and blits it into Qt's FBO (same pattern as `backend_mpv`) |
| `patches/` | local patches applied to the submodule at build time (see below) |

The QML backend that instantiates the type lives in
`plugin/contents/ui/backend/LweScene.qml`; `plugin/contents/ui/main.qml` picks
it when `PluginInfo.hasLweScene` is true.

The directory is only built with `-DUSE_LWE_SCENE=ON` (the default). With
`-DUSE_LWE_SCENE=OFF` the plugin builds and keeps selecting the old
`backend_scene`, which is still present as a fallback.

## Submodule and pin

lwe is a git submodule at `third_party/linux-wallpaperengine`, pinned to
`ddea3b493d2f4cca78a2e9547c4e61b92a14e0fc` (`next-v2`, i.e. PR #609) so that a
build never picks up upstream churn. Only the `linux-wallpaperengine-core`
target is built, and with `-DWPBUILD_WEBHELPER=OFF`: the CEF-based web helper
is not used (web wallpapers stay on QtWebEngine) and leaving it out keeps CEF
out of the build entirely. Scene audio is disabled at configuration time.

## Local patches

The submodule is never modified in our git history. The patch files are applied
to its working tree at build time (both at configure time and from the
`ExternalProject` `PATCH_COMMAND`); the submodule therefore always reports the
patched files as locally modified, which is expected.

The series is bundled into one patch file (`lwe-patch-series.patch` in the build
directory) before `git apply` sees it, and always checked as a whole:

- the bundle applies  -> it is not applied yet, apply it;
- the bundle reverses -> it is already applied, do nothing;
- neither             -> the tree is in an unexpected state (a partially applied
  series, a patch that was added while the tree still had the previous ones, or
  hand edits). The working tree is then reset to the pinned commit and the series
  is applied from scratch, logging whatever was there.

A per-patch "already applied" check (`git apply --reverse --check` on one patch)
stops working as soon as two patches touch nearby lines: a later patch rewrites
the context the earlier one needs to be recognised, and git only chains hunks
*within* one patch file, not across files.

| patch | why |
| --- | --- |
| `0001-video-texture-real-time-playback` | lwe puts mpv into untimed mode, so a video texture advanced exactly one frame per rendered frame and playback speed followed the configured FPS instead of real time |
| `0002-scripted-shader-constants` | shader constants declared with `script` (e.g. driven by `engine.timeOfDay`) were never queued with the script engine, so their JSON `value` was used forever. Scene `2843295581` blended four videos with three time-of-day multiply constants and rendered as a flat clear-colour fill |
| `0003-initialize-configuration-flags` | `Configuration::{baseOnly,noSolidFinal,passLog}` were uninitialised, so effects were randomly skipped and final passes randomly re-routed |
| `0004-memory-footprint` | scene video textures were held twice in RAM (the header kept the copy `MemoryStreamProtocol` had already copied), mpv applied its demuxer cache (150 + 50 MiB by default) to a stream that is already in RAM, and it created an audio output for a texture that is always muted. Scene `2843295581` went from 1.31 GB to 0.77 GB RSS |
| `0005-video-playback-rate` | with `0001` the videos play in real time, so the plugin's speed setting never reached them (the engine's own clock only drives shader animations and particles). `Configuration::playbackRate` is forwarded to mpv's `speed` property, like the plugin's own video backend does |
| `0006-video-pause` | pausing the wallpaper only stopped the engine clock and the render timer, so scene videos kept decoding in the background and their position kept advancing; a resumed wallpaper showed the video jumping forward by however long it had been paused. `Configuration::playbackPaused` is forwarded to mpv's `pause` property, combined with each player's own pause flag (video wallpapers pause themselves) |

Revert them with `git -C third_party/linux-wallpaperengine checkout .`; the next
build re-applies the series.

## Frame timing diagnostic

Setting `WEKDE_LWE_TIMING=1` in the environment makes every 30th rendered frame
log where its time goes: the engine render (including the mpv video textures),
the blit into Qt's target, and the `glFinish()` that waits for the GPU.

```
WEKDE_LWE_TIMING=1 plasmashell
# wekde.lwe: frame cost: avg 16.36 ms, worst 16.93 ms, 60 render/s | engine 1.20 ms, blit 0.03 ms, glFinish 15.14 ms
```

Measured on a Quadro P620 with scene `2843295581` (2560x1440, one 2560x1440@60fps
video texture) at a 1920x1080 desktop:

| configured `Fps` | engine | blit | glFinish | avg |
| --- | --- | --- | --- | --- |
| 30 | 0.8-1.1 ms | 0.03 ms | 2.2-2.7 ms | ~3.4-3.8 ms |
| 60 | 1.2-1.3 ms | 0.03 ms | ~15.1 ms | ~16.4 ms |

The wallpaper's own work is ~1.3 ms of CPU per frame at 60 fps, independent of
the desktop resolution; at `Fps` equal to the screen refresh rate the `glFinish`
call additionally waits for the display, which is why the averaged "frame cost"
sits at exactly one frame interval. The number therefore does *not* say that
the wallpaper saturates the compositor's main thread — the `glFinish` wait is
where the render thread sits idle — but it does mean the wallpaper keeps the
GPU busy: `nvidia-smi` showed ~26 % GPU utilisation for this scene at 60 fps,
which is what a dialog opened on top of it has to share.


## Build and install

```bash
cmake --build build-lwe
sudo cmake --install build-lwe
```

`cmake --install` installs both the plugin and
`liblinux-wallpaperengine-core.so*` into `${KDE_INSTALL_LIBDIR}`; the plugin is
built with `INSTALL_RPATH=${KDE_INSTALL_FULL_LIBDIR}` so the installed binary
resolves the engine library from the system and not from the build tree.

## Known gaps

- Only video textures used by a scene object itself are played. Textures
  referenced exclusively by an effect pass are never updated by lwe (`CPass`
  does no usage counting), so scenes that blend such videos can sample an empty
  texture — this hits the daytime blend of scene `2843295581`.
- `engine.timeOfDay` is always `0`: lwe never writes `Context::daytime`.
- Scene video playback grows plasmashell's RSS by a few MB per minute
  (`2843295581`: ~4.6 MB/min at 30 fps, ~3.6 MB/min at 1 fps). Heaptrack shows
  ~90% of the leaked bytes coming from `libcuda.so.1` and `libnvidia-glcore`
  with lwe and the adapter only as intermediate stack frames, the growth scales
  with the frame rate, and the per-process VRAM stays flat: it is the NVIDIA
  driver's own CPU-side bookkeeping, not the plugin, lwe or Qt. The only knob is
  the wallpaper's `Fps` setting.
