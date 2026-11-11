# bbphoto - Photo mode for Bloodborne (bbhost plugin)

A photo mode for Bloodborne 1.09 running on [bbhost](https://github.com/droogie/bbhost): a free camera
with an in-game panel in the game's own options menu, driven with the controller or the keyboard and mouse.

<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/451f638b-746c-4317-a58c-028df10a221e" />

## Install

1. Download the latest release and copy `bbphoto.dll` (Windows) or `bbphoto.so` (Linux) into bbhost's `plugins` folder (next to the
   bbhost executable, or `<data>/plugins`).
2. Leave bbhost's **Debug camera** off: the plugin writes its own free camera.
3. Start the game. The log should show
   `plugin: bbphoto: ready (free camera on, panel on, picture on, vignette on, L1/R1 on, keyboard on)`.
   The keyboard and mouse need bbhost with plugin API 13 or later.

No game files need to be replaced: the plugin builds its panel from the options menu bbhost generates.

## Controls

### Controller

| | |
|---|---|
| Hold **Action** (Cross) + press **L3** | Free camera on / off. Entering it opens the panel. |
| **L2 / R2** | Camera down / up |
| **L1 / R1** | Camera speed (panel hidden) |
| **Cross** (panel hidden) | Advance the frozen game two frames (after letting go of it once) |
| **L1 / R1** (panel open) | Previous / next page |
| **Circle** | Hide the panel (the camera keeps moving) |
| **R3** | Show the panel again |

### Keyboard and mouse

| | |
|---|---|
| **P** | Free camera on / off |
| **WASD** / **Mouse** | Move / turn the camera |
| **Space / Left Ctrl** | Camera up / down |
| **Left / right click** | Camera faster / slower (R1 / L1) |
| **N** | Advance the frozen game two frames |
| **Tab** | Hide / show the panel |
| **Q / E** (or Page Up / Page Down) | Previous / next page |

In the free camera these keys and the mouse are the camera's only. With the panel
open bbhost gives the mouse to the pointer, so the mouse turns the camera with the panel hidden. The key guide at the
bottom follows what you use: the keyboard's keys or the controller's buttons.

The free camera can't be entered with a game menu open.

## Pages

- **Camera**: Mode, FOV, Roll, HUD, Vignette. Mode (always Frozen when the photo mode starts; a choice applies once confirmed with Cross): **Frozen** (the game paused), **Static** (the camera
  stays where it is and the hunter can move), **Live** (the camera flies while the game runs)
- **Picture**: Saturation, Vibrance, Exposure, Gamma, Contrast (in % of the area's own)
- **Depth of Field**: On/Off, Focus Depth, Front Blur, Back Blur
- **Chromatic Aberration**: On/Off and four dispersion values
- **Fog**: Distance, Intensity, Contrast (%), Distance Offset, Height Offset (100 = none)
- **Motion Blur**: On/Off, Blur Time, Max Blur Length, Blur Scale, Samples

Rows you change are kept the next time you enter the free camera (also in other areas); rows you
didn't touch follow each area. **Defaults** resets the page. Everything goes back to the game's own
values when the free camera ends.

## Known limitations

- In Live the hunter still takes the buttons the camera uses (R2 attacks while the camera goes up).
- Depth of field needs Performance mode off

## Building

With Clang (and the MinGW headers for the Windows build):

```sh
./build.sh        # build/bbphoto.dll and build/bbphoto.so
```

- `src/bbphoto.c`: the plugin (it includes `movie.c` and `cave_bytes.h`).
- `src/movie.c`: the photo panel's sections, added to the options movie bbhost generates.
- `src/cave_bytes.h`: the free camera patch with the plugin's stubs, generated from it and `src/asm/*.s`.
- `include/bbhost_plugin.h`: bbhost's plugin API header (version 14), as bbhost ships it.

## Credits and license

- Free camera: **Lance McDonald**'s debug camera patch for Bloodborne 1.09.
- Built on **bbhost** (droogie) and its plugin API.
- GPL-3.0-or-later. See `LICENSE`.
