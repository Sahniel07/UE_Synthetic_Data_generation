# UE Synthetic Data Generation: Weather Pipeline

Captures labelled images of real cities in different weather, inside Unreal Engine 5.5. The cities are
Google Photorealistic 3D Tiles streamed through Cesium, and the weather comes from Ultra Dynamic
Sky / Weather.

You list locations in one Python file. For each location the pipeline:

1. Draws random camera spots inside limits you set (area, height, pitch, yaw, roll, time of day).
2. Photographs every spot in every weather you chose, so image `pNN` shows the same view in each
   weather.
3. Writes a JSON record per image with values read back from the engine: pose, lat/lon, altitude,
   rain, snow, fog, visibility, wind, sun elevation and so on.
4. Checks every record before reporting success.

```
Saved/WeatherPipeline/WeatherSnapshots/<location>/
    clear_p00.png  rain_medium_p00.png  ...  weather_metadata.json
```

---

## 1. Requirements

| What | Version / note |
|---|---|
| Windows 10/11 with a GPU | Captures render on the GPU |
| Unreal Engine | **5.5**, from the Epic Games Launcher |
| Visual Studio 2022 | Workloads "Game development with C++" and "Desktop development with C++". Opening `.vsconfig` in the VS Installer selects them. |
| Cesium for Unreal | 2.27 (free, Fab). Install it to the engine. |
| Ultra Dynamic Sky | 7.4 (paid, Fab). **Not in this repo**: its license does not allow redistribution. |
| Cesium ion account | Free. **Google Photorealistic 3D Tiles** (ion asset `2275207`) must be added to *My Assets*. |

## 2. Setup (once)

1. **Download** the repo (clone, or *Code → Download ZIP* and unzip), e.g. to `C:\Projects\MyWorld`.
   Avoid OneDrive folders.
2. **Install Cesium for Unreal.** Epic Games Launcher → Fab Library → Cesium for Unreal →
   *Install to Engine* → 5.5.
3. **Install Ultra Dynamic Sky into the project's Content.** Fab Library → Ultra Dynamic Sky →
   *Add to Project* → choose this project. It must end up in `Content/UltraDynamicSky/`, which is
   where the plugin and the weather presets look for it. Do this **before** opening the project.
4. **Build.** Double-click `MyWorld.uproject` and answer **Yes** when it asks to rebuild the missing
   modules. If that fails, right-click `MyWorld.uproject` → *Generate Visual Studio project files*,
   open `MyWorld.sln` and build **Development Editor / Win64**.
5. **Log in to Cesium with your own key** in the editor that opens (level `Ingolstadt`):
   1. On [ion.cesium.com](https://ion.cesium.com): *Asset Depot* → **Google Photorealistic 3D Tiles**
      → *Add to my assets*.
   2. In the editor's *Cesium* panel: *Connect to Cesium ion* and sign in, then *Token* → *Select or
      create a new project default token* (your *Default* token works).
   3. The city appears in the viewport.
6. **Ultra Dynamic Sky and Weather in the level.** The level already contains one
   `Ultra_Dynamic_Sky` and one `Ultra_Dynamic_Weather`. If they are missing, drag both in from
   `Content/UltraDynamicSky/Blueprints/`. Extra copies do no harm: before each capture the pipeline
   keeps one of each, removes the rest (it says so), and applies the sky look it is tuned for.
7. **File → Save All, then close the editor.** *Save All* matters: it stores your Cesium token in
   the project, where the capture reads it. A token chosen without saving stays in the editor only.
   Captures start their own Unreal and refuse to start while the editor is open.
8. **Check the install:**
   ```bat
   .\Tools\weather.cmd --test
   ```
   All tests should pass. This needs no Unreal window.

Run every `.\Tools\weather.cmd` command **from the project folder** (the one with
`MyWorld.uproject`), in PowerShell or Command Prompt. Keep the leading `.\`: PowerShell does not run
files from the current folder without it. Already inside `Tools`? Use `.\weather.cmd` instead.

`weather.cmd` finds a usable Python on its own; it uses Unreal's bundled Python first. To use
another Python, `set UE_PYTHON=C:\path\python.exe`.

**Cesium token without the editor:** paste the token as one line into a file `cesium_ion_token.txt`
next to `MyWorld.uproject`, or set the environment variable `CESIUM_ION_TOKEN`. Either one takes
precedence over the token saved in the editor. The file is git-ignored; never commit a token.

Every capture checks the token with Cesium ion before Unreal starts, and names the problem if there
is one.

## 3. Capture a dataset

All settings live at the top of **`Tools/weather_pipeline.py`**. Each setting is documented there
with its range.

1. Edit the `LOCATION` list. One entry per location; see the example and reference below.
2. Preview the job without running anything:
   ```bat
   .\Tools\weather.cmd --imagecount
   ```
   This validates every setting (wrong values are named with the reason) and prints the image
   count and an ETA.
3. Run it:
   ```powershell
   .\Tools\weather.cmd                       # every location
   .\Tools\weather.cmd --location Sydney     # only that one
   ```
   It prints progress and an ETA, then the result of the checks for each location. A run is
   successful only when it ends with `Checks: all passed.`

### A location entry

```python
LOCATION = [
    {"name": "Amsterdam", "lat": 52.377956, "lon": 4.897070, "weather": WEATHER_ALL,
     "spots": 20, "area_km2": 1.0, "height_m": [40, 120], "pitch_deg": [-30, -5], "yaw_deg": [0, 360],
     "roll_deg": [0, 0], "time_of_day": [900, 1700], "shot": {"type": "frame"}, "seed": 1,
     "image": {"width": 1920, "height": 1080, "fov_deg": 75.0, "fisheye": False, "auto_exposure": True,
               "exposure_bias": 0, "quality": 3, "tile_error": 12.0},
     "particles": {"rain_drop_scale": 3.0, "snow_flake_scale": 2.0}},
]
```

| Key | Meaning |
|---|---|
| `name` | Output folder name. |
| `lat`, `lon` | Centre of the area. South and west are negative. |
| `weather` | Conditions to capture: `WEATHER_ALL`, `WEATHER_BASE` or your own list of names (below). |
| `spots` | Number of random camera spots (1..10000). |
| `area_km2` | Square the spots are spread over, centred on lat/lon (0..25). |
| `height_m` | `[low, high]` metres above the ground under the camera (5..2000). |
| `pitch_deg` | `[low, high]`, -89..89. 0 is level; negative looks down. |
| `yaw_deg` | `[from, to]` compass heading, clockwise. `[0, 360]` means any direction; `[300, 60]` means NW→NE. |
| `roll_deg` | `[low, high]`, -45..45. |
| `time_of_day` | `[low, high]` in HHMM, e.g. `[900, 1700]`. Sunrise 0557, sunset 1742 (see `SUN`). |
| `shot` | What is taken at each spot. `{"type": "frame"}` takes one picture. `{"type": "orbit", "radius_m": 100, "pictures": 8}` takes 8 pictures on a circle around a point 100 m ahead, all facing it. |
| `seed` | Whole number: the same spots every run. `None`: new spots every run. |
| `image` | Resolution, field of view, fisheye, exposure, render quality 0..4, and `tile_error` (lower means sharper tiles but more downloads). |
| `particles` | Rain-drop and snowflake size (1 = default). |

Images per location = `spots × len(weather) × pictures per spot`.

A spot inside, against or facing into a building is drawn again automatically.

**Weather conditions** (`WEATHER_ALL`):
- Sky: `clear`, `partly_cloudy`, `cloudy`, `overcast`.
- Rain: `rain_light`, `rain_medium`, `rain_heavy`, `rain_fog`.
- Snow: `snow_light`, `snow_medium`, `snow_heavy`, `snow_fog`.
- Fog: `fog_light`, `fog_medium`, `fog_heavy`.
- Wind (the wind blows *from* that compass point): `rain_wind_n|e|s|w`, `snow_wind_n|e|s|w`.
- Other: `partly_cloudy_fog`, `day_after_snow` (snow cover, no snowfall).

### Manual start (choose the first view yourself)

Set `AUTOMATE = False` and run `.\Tools\weather.cmd`.

1. A game window opens above the location.
2. Fly to the view you want (WASD to move, Q/E for height, the mouse to look), then press **P**.
3. That view becomes spot 1 and the area is centred on it; the remaining spots are random jumps
   around it.
4. The next location then loads in the same window.

### Other settings (in `Tools/weather_pipeline.py`)

| Setting | What it controls |
|---|---|
| `OUTPUT_DIR` | Where images go (relative to the project, or an absolute path). |
| `SUN` | Day-night cycle (sunrise/sunset), or `"real": True` for the real sun on a `date`. Also sun size and lens flare. |
| `CLOUDS` | Cloud shape: size, thickness, density, wisps, a second layer, random or repeatable formation. |
| `CLOUD_BASE_ABOVE_GROUND_M` | Height of the cloud base. |
| `CLEAR_VISIBILITY_KM` | How far you can see on a clear day. The haze of every condition scales with it. |

## 4. Output

`weather_metadata.json` holds one record per image, matched by `image_file`.

| Field group | Fields |
|---|---|
| Identity | `condition`, `waypoint_index` |
| Camera | `camera_location`, `camera_rotation`, `camera_fov_deg`, `lens`, `image_width`, `image_height` |
| Position | `latitude_deg`, `longitude_deg`, `altitude_m`, `ground_altitude_m`, `height_above_ground_m` (altitudes in metres above mean sea level) |
| Weather (live values) | `rain`, `snow`, `fog`, `visibility_km`, `cloud_coverage`, `wind_intensity`, `wind_from_deg`, `material_snow_coverage`, `rain_drop_scale`, `snow_flake_scale` |
| Time | `sun_elevation_deg`, `time_of_day` |

Every value is read back from the engine. A value that cannot be read is `null` and is never
estimated.

After each run the pipeline checks every record:
- each image exists and has the right size;
- the pose is inside your limits and the position inside the area;
- the weather matches its preset;
- the sun is up or down to match the time;
- every spot was captured in every weather.

## 5. Add your own shot (e.g. a spiral)

1. In `Plugins/WeatherPipeline/Source/WeatherPipeline/Public/CaptureShots.h`, add a class
   `USpiralCaptureShot : public UCaptureShot`.
   - Override `BuildPoses(Spot, Parameters)` to return the camera poses for one spot, with the
     spot's own pose first. The header explains the rest.
   - The class name sets the name used in the settings: `USpiralCaptureShot` is `"spiral"`.
2. In `Tools/weather_pipeline.py`, add an entry to `SHOT_TYPES` with its parameters and limits, the
   pictures per spot and how far a picture may stand from its spot.
3. Rebuild. Then use `"shot": {"type": "spiral", ...}` in a location.

The tests catch a shot that exists on only one side, and check that every shot class starts at its
spot.

## 6. Tests

| Tests | Command |
|---|---|
| Python: settings, ini, validation, metadata checks | `.\Tools\weather.cmd --test` |
| C++: plugin logic, headless | `"C:\Program Files\Epic Games\UE_5.5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "%CD%\MyWorld.uproject" -ExecCmds="Automation RunTests WeatherPipeline; Quit" -unattended -nullrhi -log` |

## 7. Troubleshooting

| Message / symptom | Fix |
|---|---|
| "The Unreal editor is open. Close it first" | Close it; a run needs the level file. |
| `Not capturing: No Cesium ion access token` | The editor's token was never saved. Redo Setup steps 5 and 7 (**File → Save All**), or use `cesium_ion_token.txt`. |
| `Not capturing: Cesium ion rejected the access token` (or HTTP 401 while loading tiles) | The token is wrong, truncated or deleted. Choose it again in the Cesium panel and **Save All**, or copy it from ion.cesium.com → *Access Tokens* into `cesium_ion_token.txt`. |
| `... cannot read Cesium ion asset 2275207` (or HTTP 404) | Add *Google Photorealistic 3D Tiles* to *My Assets*, and let the token read it (Setup step 5). |
| Editor shows a "Cesium ion Token Troubleshooting" window | Choose *Select or create a new project default token*, then **File → Save All**. |
| `note: removed the extra Ultra_Dynamic_Sky ...` | Expected when UDS/UDW were dragged into a level that already had them; the pipeline keeps one of each. |
| `Not capturing: the level has no Ultra_Dynamic_Sky` (or Weather) | Drag it in from `Content/UltraDynamicSky/Blueprints/`, **Save All**, close the editor. |
| HTTP 429 | Tile quota used up: wait, or raise `tile_error`. |
| `The module 'Tools' could not be loaded` (PowerShell) | Run from the project folder with the leading `.\`: `.\Tools\weather.cmd --imagecount`. Inside `Tools`: `.\weather.cmd --imagecount`. |
| `No usable Python found` | Install UE 5.5 in the default path, or `set UE_PYTHON=...`. |
| Build fails: "LiveCoding ... denied" | An editor, possibly hidden, is still running. End `UnrealEditor.exe` in Task Manager. |
| Missing Ultra Dynamic Sky classes | UDS is not at `Content/UltraDynamicSky/`. Redo Setup step 3. |

## Repository layout

```
MyWorld.uproject
Config/                       project settings; DefaultGame.ini gets a block the pipeline writes
Content/                      level Ingolstadt, weather presets (WeatherPipeline/Presets), snow material, EGM96 geoid grid
Plugins/WeatherPipeline/      the C++ capture plugin (AMyActor_weather, CaptureShots, tests)
Tools/weather_pipeline.py     all settings + the runner and checks
Tools/test_weather_pipeline.py
Tools/weather.cmd             runs the pipeline with a working Python
```
