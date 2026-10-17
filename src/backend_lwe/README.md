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
to its working tree at build time with `git apply` (idempotent: an
already-applied patch is detected with `git apply --reverse --check`), both at
configure time and from the `ExternalProject` `PATCH_COMMAND`. The submodule
therefore always reports the patched files as locally modified — that is
expected.

| patch | why |
| --- | --- |
| `0001-video-texture-real-time-playback` | lwe puts mpv into untimed mode, so a video texture advanced exactly one frame per rendered frame and playback speed followed the configured FPS instead of real time |
| `0002-scripted-shader-constants` | shader constants declared with `script` (e.g. driven by `engine.timeOfDay`) were never queued with the script engine, so their JSON `value` was used forever. Scene `2843295581` blended four videos with three time-of-day multiply constants and rendered as a flat clear-colour fill |
| `0003-initialize-configuration-flags` | `Configuration::{baseOnly,noSolidFinal,passLog}` were uninitialised, so effects were randomly skipped and final passes randomly re-routed |
| `0004-memory-footprint` | scene video textures were held twice in RAM (the header kept the copy `MemoryStreamProtocol` had already copied), mpv applied its demuxer cache (150 + 50 MiB by default) to a stream that is already in RAM, and it created an audio output for a texture that is always muted. Scene `2843295581` went from 1.31 GB to 0.77 GB RSS |

Revert them with `git -C third_party/linux-wallpaperengine checkout .`.

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
- Scene video playback has a slow, bounded CPU-side growth (a few MB per minute)
  that is not fully attributed yet; it is unrelated to the Qt adapter layer
  (a scene without video textures is completely flat).
