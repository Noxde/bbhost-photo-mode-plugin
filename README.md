# bbphoto - Photo mode for Bloodborne (bbhost plugin)

A photo mode for Bloodborne 1.09 running on [bbhost](https://github.com/droogie/bbhost): a free camera over the
world, held still or running, with the HUD out of the picture and its settings on pages of the game's own options
dialog, driven with the controller or the keyboard and mouse.

<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/451f638b-746c-4317-a58c-028df10a221e" />

This is bbphoto rebuilt on bbhost's plugin API (version 29, from bbhost v0.2.35). Nothing in it writes the game's
code or reads an address of its own: the free camera, the pause and frame advance, the hidden HUD, the picture's look
and the panel are all calls the plugin API answers. bbhost does not bundle it: install a build from here to play
with it.

## Install a build from here

1. Build it (below), or take `photo_mode.dll` (Windows) or `photo_mode.so` (Linux) from a release here.
2. Copy it into your per-user plugins folder (`%APPDATA%\bbhost\plugins` on Windows, `~/.config/bbhost/plugins`
   on Linux).
3. Start the game. The log shows `plugin: photo_mode 1.0.0 loaded (...)`. It needs bbhost v0.2.35 or later: an older
   bbhost logs `photo mode: needs plugin API 29; this host has <n>` and leaves it off.

While photo mode is on, bbhost does not load the older bbphoto (1.0.x), so the two never answer the same buttons.
`photo_mode = false` under `[plugins]` in bbhost.toml turns photo mode off.

## Controls

Press **L3**, or **P**, to start photo mode and to end it (**Escape** ends it too). L3 is the button your bindings
give the game's L3, which the game does nothing with in play. Its **Start with** setting (on bbhost's Plugins tab,
or F10 › Plugins in game) can ask for L3 held half a second instead, so that a press does nothing; P works either
way.

| Controller | Keyboard and mouse | |
|---|---|---|
| Left stick | WASD | Move the camera the way it looks |
| Right stick | Mouse | Turn the camera |
| L2 / R2 | Left Ctrl / Space | Camera down / up |
| L1 / R1 | Right / left mouse button | Camera slower / faster |
| Cross | N | Frozen: let the world run one frame (a frame advance) |
| R3 | Tab | Show or hide the panel |

The panel opens with photo mode: the game's own options dialog, made smaller and put at the bottom right so the
picture stays in view. **L1 / R1** (Page Up / Page Down) turn its pages, the **d-pad** (the arrows) chooses and
changes a row, **Circle** (Escape) hides it and **R3** (Tab) shows it again; the mouse pointer works its rows too.
While it is up, the buttons and keys it moves by are its own: hide it to use L1, R1 and Cross for the camera. The
sticks, L2, R2, WASD, Left Ctrl, Space and N work either way.

## Modes

- **Frozen**: the world held still while the camera flies. Photo mode always starts Frozen.
- **Static**: the camera stays where it is and the hunter plays on.
- **Live**: the camera flies while the world runs. The hunter stands, since the camera has the controls, and enemies
  can still attack: the first hit holds the world again.

Photo mode runs only with nobody else in your world: it does not start while another player is in it or on the way,
or in another player's world, and it ends as soon as one is coming. It also ends at a loading screen, a cutscene or
one of the game's menus. Ending it puts the game's own view back as it was.

## Pages

- **Camera**: Mode, Field of view (-40 to +60 degrees around the game's own), Roll, HUD, Vignette
- **Picture**: Saturation, Brightness, Exposure, Gamma, Contrast
- **Depth of field**: Area's own, On or Off; Sharp from and Sharp to (sharp between them, from 0.25 m to 40 m and
  then Beyond 50 m); Blur in front, Blur behind and Fade
- **Chromatic aberration**: Area's own, On or Off, and four dispersion values
- **Fog**: Distance, Density, Contrast, Start, Height fog
- **Motion blur**: Area's own, On or Off; Time, Length, Camera blur, Samples (2 to 8)

Each page ends with a **Defaults** row, and the dialog's help line says what the row under the cursor does. The look goes over the area's own
while photo mode runs, and nowhere else. bbhost's Performance mode leaves depth of field and motion blur out; their
rows then do nothing.

## What changed from 1.0.1

- No code caves or import-slot hooks: bbhost's plugin API 29 has the calls they made, and grew them for this plugin
  (the world held and stepped, the controller and keys as a plugin's own, the HUD left out of the picture, the
  picture's look over the area's). The panel is still the game's own options dialog, as in 1.0.1, but bbhost builds
  its pages and opens it: the plugin only lists its rows.
- Start and end with a press of L3, or P. The Cross + L3 chord is gone, because Cross is the game's Interact.
- Frozen's frame advance is one frame.
- Live warns that enemies can still attack, and holds the world again at the hunter's first hit. The hunter no
  longer takes the buttons the camera uses in Live.
- bbhost's Debug camera is not needed (bbhost v0.2.35 removed that setting).

## Building

With Clang and lld (and the MinGW headers and libraries for the Windows build):

```sh
./build.sh        # build/photo_mode.dll and build/photo_mode.so
```

- `src/photo_mode.cpp`: the plugin.
- `include/bbhost_plugin.h`: bbhost's plugin API header (version 29), as bbhost v0.2.35 ships it.

## Credits and license

- **Nox-**: bbphoto, the photo mode this is built from - its modes, pages and controls.
- **Lance McDonald**: the debug camera patch for Bloodborne 1.09 that bbphoto's first free camera used.
- The **bbhost** project (droogie): the rebuild on the plugin API, and the API's additions for it.
- GPL-3.0-or-later. See `LICENSE`.
