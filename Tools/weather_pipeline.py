#!/usr/bin/env python3
r"""
weather_pipeline.py - capture labelled weather images of real cities in Unreal.

    Tools\weather.cmd --imagecount     check the settings; show the image count and time, run nothing
    Tools\weather.cmd --test           run the tests (no Unreal needed)
    Tools\weather.cmd                  capture every city below
    Tools\weather.cmd --location Sydney    one location only

With a real Python 3.8+ the same works as `python Tools\weather_pipeline.py [option]` or with the
Run button in VS Code. weather.cmd only finds one for you: on Windows `python` is often just the
Microsoft Store placeholder, so it uses the Python that ships with Unreal.

It writes the settings of every city into Config/DefaultGame.ini, saves the level at the first city,
runs ONE Unreal session that visits the cities in turn (the plugin moves the globe to each) while
printing progress and an ETA, and finally checks every metadata record against what the engine should
have produced. Images land in Saved/WeatherPipeline/WeatherSnapshots/<city>/.

The capture itself is the C++ plugin WeatherPipeline (AMyActor_weather); this file only configures and
runs it. Unreal silently ignores ini keys it does not know, so test_weather_pipeline.py checks every
key written here against the plugin's header.

The same file also runs INSIDE Unreal (with WP_INSIDE_UNREAL set) to move the georeference - see the
bottom of the file. Standard library only.
"""

from __future__ import annotations

import argparse
import datetime
import json
import math
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

# ##################################################################################################
#   BASIC SETTINGS - what to capture and how
#   "checked" ranges are enforced - the run refuses to start outside them. "typical" is guidance.
# ##################################################################################################

# Every weather condition the pipeline knows, in capture order. Use it whole, or list a subset per city.
WEATHER_ALL = ["clear", "partly_cloudy", "cloudy", "overcast",
               "rain_light", "rain_medium", "rain_heavy", "rain_fog",
               "snow_light", "snow_medium", "snow_heavy", "snow_fog",
               "fog_light", "fog_medium", "fog_heavy",
               "rain_wind_n", "rain_wind_e", "rain_wind_s", "rain_wind_w",
               "snow_wind_n", "snow_wind_e", "snow_wind_s", "snow_wind_w",
               "partly_cloudy_fog", "day_after_snow"]

WEATHER_BASE = ["clear", "rain_medium", "cloudy", "snow_medium","partly_cloudy_fog"]

# One entry per location, with all of its own settings; each gets its own image folder. All locations
# run in one Unreal session. The camera jumps to `spots` camera spots, each drawn at random inside the
# entry's limits, and captures every spot in every weather of the entry - so image N shows the same
# view in each weather. A [low, high] value is drawn per spot; low = high fixes it. Images per
# location: spots x weathers x the shot's pictures per spot. All checked.
#   name            folder name for its images: no spaces or < > : " / \ | ? *
#   lat             -90..90. South is negative.
#   lon             -180..180. West is negative.
#   weather         names from WEATHER_ALL, captured at every spot, in that order. More = more images.
#   spots           1..10000 camera spots. More = more images.
#   area_km2        0..25: the square the spots are spread over, centred on lat/lon (with AUTOMATE
#                   False: on where you press P). Bigger = more varied places, more tiles to load;
#                   0 = every spot at the centre.
#   height_m        [low, high] 5..2000 m above the ground under each picture. Higher = wider view,
#                   smaller buildings. A spot in, against or facing into a building, or over no
#                   ground, is drawn again - never raised above the limits.
#   pitch_deg       [low, high] -89..89. 0 = level with the horizon; lower = looking further down.
#   yaw_deg         [from, to] compass 0..360 the camera faces (0 north, 90 east), clockwise from
#                   `from` to `to`: [0, 360] = any direction, [90, 180] = east to south,
#                   [300, 60] = north-west round to north-east.
#   roll_deg        [low, high] -45..45. Tilts the horizon; 0 = level.
#   time_of_day     [low, high] 0000..2400 (HHMM; 1530 = 3:30 pm); sunrise 0557, sunset 1742 (SUN).
#                   Each spot gets its own time, shared by all its weathers. Outside sunrise to
#                   sunset the pictures are dark.
#   shot            what is captured at each spot: {"type": its name, then its own parameters}.
#                     {"type": "frame"}
#                         one picture.
#                     {"type": "orbit", "radius_m": 100, "pictures": 8}
#                         `pictures` pictures round a circle: the spot is the first, the circle's
#                         centre is `radius_m` straight ahead of the spot's view, and every picture
#                         faces that centre at the spot's height, pitch, roll and time.
#                         radius_m  10..2000 m. Bigger = a wider circle round a centre further off.
#                         pictures  2..360. More = more pictures round the circle, closer together.
#                   Your own shot: a class in the plugin's CaptureShots.h (how is explained there) and
#                   an entry in SHOT_TYPES below.
#   seed            a whole number: the same spots every run (another number = other spots).
#                   None = new spots every run.
#   image           the pictures:
#   width, height   64..8192 px. Bigger = more detail, larger files, slower captures.
#   fov_deg         10..170 (10..150 with fisheye). Horizontal field of view. Higher = wider view,
#                     smaller objects; lower = zoomed in.
#   fisheye         True: a fish-eye lens - straight lines bow outward, more fits in towards the
#                     edges, like a webcam or action camera; same width x height, a little slower.
#                     False: a normal lens - straight lines stay straight.
#   auto_exposure   True: exposes like a real camera - each picture metered on its own content,
#                     ignoring a sun or moon in view - so overcast looks grey, not black.
#                     False: one fixed exposure for every picture - darker weather stays darker, but
#                     dim conditions (overcast, dusk) come out very dark.
#   exposure_bias   -15..15 stops. +1 doubles the brightness, -1 halves it. With auto_exposure it is
#                     the camera's exposure compensation (0 = as metered).
#   quality         0..4 render quality: 0 Low, 1 Medium, 2 High, 3 Epic, 4 Cinematic. Higher =
#                     sharper shadows, better lighting, reflections and effects; slower captures.
#   tile_error      2..32 how coarse the 3D tiles may be. Lower = finer buildings and textures, but
#                     more downloads (Google quota) and longer loading; higher = faster, blurrier city.
#   particles       rain_drop_scale, snow_flake_scale: 0.05..20. Higher = bigger drops / flakes;
#                   1 = UDW's default.
# The game window shows the first location's image shape.
LOCATION = [
    {"name": "Sydney", "lat": -33.8688, "lon": 151.2093, "weather": WEATHER_ALL,
     "spots": 20, "area_km2": 1.0, "height_m": [40, 120], "pitch_deg": [-30, -5], "yaw_deg": [0, 360],
     "roll_deg": [0, 0], "time_of_day": [900, 1700], "shot": {"type": "frame"}, "seed": 1,
     "image": {"width": 1920, "height": 1080, "fov_deg": 75.0, "fisheye": False, "auto_exposure": True,
               "exposure_bias": 0, "quality": 3, "tile_error": 6.0},
     "particles": {"rain_drop_scale": 3.0, "snow_flake_scale": 2.0}},
    {"name": "Ingolstadt", "lat": 48.7665, "lon": 11.4258, "weather": WEATHER_ALL,
     "spots": 20, "area_km2": 1.0, "height_m": [40, 120], "pitch_deg": [-30, -5], "yaw_deg": [0, 360],
     "roll_deg": [0, 0], "time_of_day": [900, 1700], "shot": {"type": "frame"}, "seed": 1,
     "image": {"width": 1920, "height": 1080, "fov_deg": 75.0, "fisheye": False, "auto_exposure": True,
               "exposure_bias": 0, "quality": 3, "tile_error": 6.0},
     "particles": {"rain_drop_scale": 3.0, "snow_flake_scale": 2.0}},
    {"name": "Amsterdam", "lat": 52.377956, "lon": 4.897070, "weather": WEATHER_ALL,
     "spots": 20, "area_km2": 1.0, "height_m": [40, 120], "pitch_deg": [-30, -5], "yaw_deg": [0, 360],
     "roll_deg": [0, 0], "time_of_day": [900, 1700], "shot": {"type": "frame"}, "seed": 1,
     "image": {"width": 1920, "height": 1080, "fov_deg": 75.0, "fisheye": False, "auto_exposure": True,
               "exposure_bias": 0, "quality": 3, "tile_error": 6.0},
     "particles": {"rain_drop_scale": 3.0, "snow_flake_scale": 2.0}},
    {"name": "Gerolfing", "lat": 48.7686, "lon": 11.3372, "weather": WEATHER_ALL,
     "spots": 20, "area_km2": 1.0, "height_m": [40, 120], "pitch_deg": [-30, -5], "yaw_deg": [0, 360],
     "roll_deg": [0, 0], "time_of_day": [900, 1700],
     "shot": {"type": "orbit", "radius_m": 100, "pictures": 8}, "seed": 1,
     "image": {"width": 1920, "height": 1080, "fov_deg": 75.0, "fisheye": False, "auto_exposure": True,
               "exposure_bias": 0, "quality": 3, "tile_error": 6.0},
     "particles": {"rain_drop_scale": 3.0, "snow_flake_scale": 2.0}},
    ]

# True:  hands-off: the camera jumps from spot to spot on its own; keyboard and mouse are ignored.
# False: you choose where it starts. A window opens with the drone above the location; fly to a view
#        and press P. That view is the first spot, taken as it is, and the area is centred on it; the
#        rest are random jumps round it (the controls lock until it is done). Then the next location
#        loads in the same window; it closes after the last.
AUTOMATE = True


# ##################################################################################################
#   ADVANCED SETTINGS - change only with a reason
# ##################################################################################################

# Where images and weather_metadata.json go: OUTPUT_DIR/WeatherSnapshots/<city>/. Relative = inside
# the project folder; an absolute path (e.g. "D:/datasets") also works.
OUTPUT_DIR = "Saved/WeatherPipeline"

# The sun (all checked).
#   real           False: a regular day-night cycle, the same at every location - the sun rises at
#                  sunrise, is highest halfway to sunset and sets at sunset; yaw_deg and pitch_deg set
#                  its path across the sky.
#                  True: the real sun for each location's latitude and longitude on date - season and
#                  day specific. time_of_day is then the place's sun time (1200 = about its highest);
#                  sunrise, sunset, yaw_deg and pitch_deg are not used.
#   sunrise        0000..2400 (HHMM), when the sun comes up (real False). Earlier = a longer day.
#   sunset         0000..2400 (HHMM), after sunrise, when it goes down (real False). Later = a longer day.
#   date           "YYYY-MM-DD" the real sun is solved for (real True). Summer = higher sun, later dusk.
#   yaw_deg        0..360, only when real is False. Turns the sun around the sky, and the shadows.
#   pitch_deg      -90..90, only when real is False. UDS "Sun Pitch": tilts the sun's path.
#   size           0.1..20, the sun disc's size: 1 = as large as the real sun looks (0.53 deg across),
#                  2 = twice as wide. Bigger = a larger, softer-edged glow round the sun.
#   lens_flare     True/False. True: the rings and hexagons a camera lens throws off a bright sun, as in
#                  real photos. False: none - only the sun and its glow.
#   intensity_lux  0..100. UDS "Sun Light Intensity". Higher = sunlit ground brighter against the sky,
#                  harder shadows.
#   sky_light      0..10. UDS "Sky Light Intensity": the sky's ambient light - under cloud most of the
#                  ground's light. Higher = brighter shade and cloudy ground; lower = darker, more
#                  contrast. Saved into the level.
SUN = {"real": False, "sunrise": 557.0, "sunset": 1742.0, "date": "2026-09-30", "yaw_deg": 0.0,
       "pitch_deg": 30.0, "size": 1.0, "lens_flare": True, "intensity_lux": 5.0, "sky_light": 1.0}

# Cloud base, metres above the ground; typical 800..3000 (real cumulus sit about 1-1.5 km up). Checked
# to be at least 300 m above the highest picture: UDW switches rain and snow off for a camera above
# the clouds. Higher = clouds further off, small and high; lower = a heavier, closer deck.
CLOUD_BASE_ABOVE_GROUND_M = 1375.0

# Cloud look: Ultra Dynamic Sky's own cloud settings, the same for every condition and city (all
# checked). HOW MUCH cloud there is stays each condition's preset; these shape that cloud. They are
# saved into the level with the city, so the editor shows the same sky. The defaults are the level's.
#   size                   0.25..4   bigger, broader formations; lower = smaller, busier clouds
#   thickness              0.25..4   taller, towering clouds; lower = flat, thin layers
#   density                0.5..30   solid clouds with dark undersides; lower = soft, see-through
#   patchiness             0..1      clumps with open gaps between; 0 = evenly spread
#   patch_size             0.25..4   bigger clumps and gaps; lower = smaller, more of them
#   wisps                  True/False  thin, high wisp clouds (cirrus streaks). False = none in any weather
#   wisps_clear            0..1      how strong the wisps are in clear weather (when wisps is True)
#   wisps_cloudy           0..1      the same in cloudy weather
#   second_layer           True/False  a second cloud layer above the main one - more cloud in every
#                                      cloudy condition, without touching the presets
#   second_layer_coverage  0..2      more of the second layer; lower = less
#   second_layer_density   0.01..1   more solid second layer; lower = fainter
#   random_formation       True/False  True: new cloud shapes every run. False: the same shapes every
#                                      run, so datasets can be repeated
CLOUDS = {"size": 1.0, "thickness": 1.0, "density": 10.0, "patchiness": 0.16, "patch_size": 1.3,
          "wisps": True, "wisps_clear": 0.2, "wisps_cloudy": 0.5, "second_layer": False, "second_layer_coverage": 1.0,
          "second_layer_density": 0.05, "random_formation": True}

# How far one can see on a clear day, km: horizontally at street level, to where contrast has dropped
# to 5% (the weather-station definition). 0.2..1000 (checked); real clear days are typically 20..40 km,
# very clean air after rain 100 km and more. From 40 m up the ground horizon is only ~23 km away, so
# above ~60 km the pictures barely change unless the camera flies much higher.
# Lower = a hazier horizon; higher = a crisper one. It sets where Ultra Dynamic Sky's height fog sits,
# so the haze of every condition moves with it - fog conditions stay foggier than clear. Each run
# places the sky for it and checks the clear pictures' visibility from the plugin's log.
CLEAR_VISIBILITY_KM = 20.0

# The preset asset behind each condition and its authored values (cloud, rain, snow, fog, wind are
# 0..10, higher = more; snowcov is 0..1, the share of ground under snow). Every condition is a 1:1 copy
# of its asset. The metadata check compares each image's live values with this table, and a test
# compares the table with the assets (Saved/wp_presets.txt): change the preset in Unreal, then here.
#
# WP_* and Rain_Fog / Snow_Fog are project copies of stock presets in /Game/WeatherPipeline/Presets:
#   WP_Rain_Heavy  Rain_Thunderstorm with wind 0 - the downpour falls straight down
#   WP_Rain_Wind   Rain with wind 7; WP_Snow_Wind is WP_Snow with wind 7 (direction: WIND_FROM_DEG)
#   WP_Fog_Light   Foggy at fog 6 - light haze;  WP_Fog_Medium  Foggy at fog 8 - distinct fog
#   WP_Snow_Light  patchy snow under a broken sky;  WP_Snow  heavier, but not full cover
#   Rain_Fog / Snow_Fog  Rain / WP_Snow with the Foggy preset's fog
#   WP_Partly_Cloudy_Fog  Partly_Cloudy with medium fog (8) - fog under a broken cloud deck
#   WP_Day_After_Snow     Cloudy with WP_Snow's snow cover (0.55) on the ground and roofs, no snowfall
# Wetness and thunder/lightning are not listed: neither shows in the images.
PRESET_CHANNELS = ("cloud_coverage", "rain", "snow", "fog", "wind_intensity", "material_snow_coverage")
PRESET_TABLE = {
    #                asset                 cloud rain  snow  fog   wind  snowcov
    "clear":         ("Clear_Skies",       0.0,  0.0,  0.0,  0.0,  2.0,   0.0),
    "partly_cloudy": ("Partly_Cloudy",     3.8,  0.0,  0.0,  1.0,  2.0,   0.0),
    "cloudy":        ("Cloudy",            5.0,  0.0,  0.0,  1.0,  2.5,   0.0),
    "overcast":      ("Overcast",          7.5,  0.0,  0.0,  1.5,  3.0,   0.0),
    "rain_light":    ("Rain_Light",        6.0,  3.0,  0.0,  1.5,  2.0,   0.0),
    "rain_medium":   ("Rain",              7.5,  7.0,  0.0,  3.0,  3.0,   0.0),
    "rain_fog":      ("Rain_Fog",          7.5,  7.0,  0.0,  10.0, 3.0,   0.0),
    "rain_heavy":    ("WP_Rain_Heavy",     8.0,  10.0, 0.0,  6.5,  0.0,   0.0),
    "snow_light":    ("WP_Snow_Light",     5.0,  0.0,  3.0,  3.0,  1.0,   0.45),
    "snow_medium":   ("WP_Snow",           8.5,  0.0,  6.0,  5.0,  4.0,   0.55),
    "snow_fog":      ("Snow_Fog",          8.5,  0.0,  6.0,  10.0, 4.0,   0.55),
    "snow_heavy":    ("Snow_Blizzard",     10.0, 0.0,  10.0, 10.0, 10.0,  1.0),
    "rain_wind_n":   ("WP_Rain_Wind",      7.5,  7.0,  0.0,  3.0,  7.0,   0.0),
    "rain_wind_e":   ("WP_Rain_Wind",      7.5,  7.0,  0.0,  3.0,  7.0,   0.0),
    "rain_wind_s":   ("WP_Rain_Wind",      7.5,  7.0,  0.0,  3.0,  7.0,   0.0),
    "rain_wind_w":   ("WP_Rain_Wind",      7.5,  7.0,  0.0,  3.0,  7.0,   0.0),
    "snow_wind_n":   ("WP_Snow_Wind",      8.5,  0.0,  6.0,  5.0,  7.0,   0.55),
    "snow_wind_e":   ("WP_Snow_Wind",      8.5,  0.0,  6.0,  5.0,  7.0,   0.55),
    "snow_wind_s":   ("WP_Snow_Wind",      8.5,  0.0,  6.0,  5.0,  7.0,   0.55),
    "snow_wind_w":   ("WP_Snow_Wind",      8.5,  0.0,  6.0,  5.0,  7.0,   0.55),
    "fog_light":     ("WP_Fog_Light",      0.0,  0.0,  0.0,  6.0,  1.0,   0.0),
    "fog_medium":    ("WP_Fog_Medium",     0.0,  0.0,  0.0,  8.0,  1.0,   0.0),
    "fog_heavy":     ("Foggy",             0.0,  0.0,  0.0,  10.0, 1.0,   0.0),
    "partly_cloudy_fog": ("WP_Partly_Cloudy_Fog", 3.8, 0.0, 0.0, 8.0, 2.0, 0.0),
    "day_after_snow":    ("WP_Day_After_Snow",    5.0, 0.0, 0.0, 1.0, 2.5, 0.55),
}

# Compass direction the wind blows FROM, 0..360 (0 = north, 90 = east), for the wind conditions.
# The plugin sets the same; each image's recorded wind_from_deg must match it within
# WIND_TOLERANCE_DEG (typical 0.5..5; higher = a more lenient check).
WIND_FROM_DEG = {f"{kind}_wind_{point}": bearing for kind in ("rain", "snow")
                 for point, bearing in (("n", 0.0), ("e", 90.0), ("s", 180.0), ("w", 270.0))}
WIND_TOLERANCE_DEG = 1.0

# The fields the plugin writes to weather_metadata.json, in order - each read back from the engine,
# null when unreadable. Must match the C++ writer (a test compares them): change both together.
# altitude_m and ground_altitude_m are metres above mean sea level. visibility_km is how far one can see
# horizontally at street level (5% contrast), from the live height fog.
METADATA_FIELDS = (
    "image_file", "condition", "waypoint_index",
    "camera_location", "camera_rotation", "camera_fov_deg", "lens",
    "image_width", "image_height",
    "latitude_deg", "longitude_deg", "altitude_m",
    "ground_altitude_m", "height_above_ground_m",
    "rain", "snow", "fog", "visibility_km", "cloud_coverage", "wind_intensity", "wind_from_deg",
    "material_snow_coverage", "rain_drop_scale", "snow_flake_scale",
    "sun_elevation_deg", "time_of_day",
)

# ==================================================================================================
#   Machinery
# ==================================================================================================

# --- Project and Unreal ---------------------------------------------------------------------------

# globals().get: Unreal's script runner is not guaranteed to define __file__, and nothing inside Unreal
# needs these paths.
PROJECT_ROOT = Path(globals().get("__file__", "Tools/weather_pipeline.py")).resolve().parent.parent
OUTPUT_ROOT = (PROJECT_ROOT / str(OUTPUT_DIR)).resolve()   # an absolute OUTPUT_DIR stays as it is
UPROJECT = PROJECT_ROOT / "MyWorld.uproject"
CONFIG_INI = PROJECT_ROOT / "Config" / "DefaultGame.ini"
RUN_LOG = PROJECT_ROOT / "Saved" / "Logs" / "weather_pipeline_run.log"
SITE_RESULT = PROJECT_ROOT / "Saved" / "weather_pipeline_site.txt"
# The ground height the plugin calculated for each city (metres above the WGS84 ellipsoid). Written
# into the level's georeference, so the editor shows each city at its geo-accurate height.
GROUND_HEIGHTS = PROJECT_ROOT / "Saved" / "weather_pipeline_ground_heights.json"
GROUND_HEIGHT_CHANGE_M = 0.5               # re-save the level only when the height really changed
LEVEL = "/Game/Ingolstadt"
INI_SECTION = "/Script/WeatherPipeline.MyActor_weather"
BEGIN_MARKER = "; >>> BEGIN weather_pipeline.py generated block - do not edit by hand <<<"
END_MARKER = "; <<< END weather_pipeline.py generated block >>>"
ENGINE_DIRS = [r"C:\Program Files\Epic Games\UE_5.5", r"C:\Program Files\Epic Games\UE_5.4"]


def window_args(image: dict, max_width: int, max_height: int) -> List[str]:
    """A window with the image's aspect ratio, as large as fits the box - so the view on screen is
    framed like the picture that is saved. The pictures keep their own resolution."""
    scale = min(max_width / image["width"], max_height / image["height"])
    return ["-windowed", f"-resx={round(image['width'] * scale)}", f"-resy={round(image['height'] * scale)}"]


AUTOMATED_WINDOW_MAX = (1280, 720)         # the window only shows the run; captures have their own size
MANUAL_WINDOW_MAX = (1600, 900)            # larger, so the view you fly and frame is easy to judge


def launch_args(automate: bool, image: dict) -> List[str]:
    """Automated: no input, starts itself. Manual: a normal game window to fly in, started with P.
    The window has `image`'s shape.
    Auto-start is a command-line flag, never an ini value: DefaultGame.ini is shared with the editor,
    and an auto-start left in it made Play start flying the route on its own."""
    if automate:
        return ["-game", "-unattended", "-nopause", "-nosplash", "-WeatherAutoStart",
                *window_args(image, *AUTOMATED_WINDOW_MAX)]
    return ["-game", "-nosplash", *window_args(image, *MANUAL_WINDOW_MAX)]
POLL_SECONDS = 2.0
LEVEL_SAVE_TIMEOUT_S = 900                 # the headless level save takes about a minute
TILE_STATUS_REASONS = {
    "429": "Google Map Tiles quota used up - see the quotas in Google Cloud Console, or wait for the reset",
    "403": "the Google API key was refused - check the key and that the Map Tiles API is enabled",
    "401": "no valid Google API key",
}
CM_PER_M = 100.0
# In the georeferenced world Unreal +X is east and +Y is south, so a compass bearing is Unreal yaw + 90.
COMPASS_TO_UNREAL_YAW_DEG = -90.0

# --- Camera spots -----------------------------------------------------------------------------------

SPOT_KEYS = ("spots", "area_km2", "height_m", "pitch_deg", "yaw_deg", "roll_deg", "time_of_day", "shot", "seed")
CAPTURE_RANGES = {"height_m": (5.0, 2000.0), "pitch_deg": (-89.0, 89.0), "yaw_deg": (0.0, 360.0),
                  "roll_deg": (-45.0, 45.0), "time_of_day": (0.0, 2400.0)}   # each [low, high] setting
SPOT_COUNT_RANGE = (1, 10000)
AREA_RANGE_KM2 = (0.0, 25.0)
M_PER_DEG_LAT = 110574.0                   # metres per degree, for the area check from lat/lon
M_PER_DEG_LON_AT_EQUATOR = 111320.0


@dataclass(frozen=True)
class ShotParameter:
    """One number a shot takes: its bounds, and whether it must be whole."""
    low: float
    high: float
    whole: bool = False

    def holds(self, value: object) -> bool:
        bounds = (self.low, self.high)
        return is_whole(value, bounds) if self.whole else in_range(value, bounds)

    def __str__(self) -> str:
        return f"{'a whole number ' if self.whole else ''}within ({self.low}, {self.high})"


@dataclass(frozen=True)
class ShotType:
    """A shot as the settings see it. The shot itself - the poses it photographs - is the plugin's class
    U<Name>CaptureShot in CaptureShots.h; this says what the settings may give it and what to expect."""
    parameters: Dict[str, ShotParameter]       # every one required in the location's "shot" table
    pictures: Callable[[dict], int]            # pictures per spot, from the "shot" table
    reach_m: Callable[[dict], float]           # how far from its spot a picture may stand, m


# Every shot a LOCATION entry may name. Adding your own: its class in CaptureShots.h, then an entry here.
SHOT_TYPES = {
    "frame": ShotType(parameters={}, pictures=lambda shot: 1, reach_m=lambda shot: 0.0),
    "orbit": ShotType(parameters={"radius_m": ShotParameter(10.0, 2000.0),
                                  "pictures": ShotParameter(2, 360, whole=True)},
                      pictures=lambda shot: int(shot["pictures"]),
                      reach_m=lambda shot: 2.0 * shot["radius_m"]),      # the far side of the circle
}

# --- Setting limits ---------------------------------------------------------------------------------

LOCATION_KEYS = ("name", "lat", "lon", "weather", "image", "particles")
DEG_PER_SUN_HOUR = 15.0                    # the sun's time runs one hour per 15 degrees of longitude
SUN_KEYS = ("real", "sunrise", "sunset", "date", "yaw_deg", "pitch_deg", "size", "lens_flare", "intensity_lux",
            "sky_light")
IMAGE_KEYS = ("width", "height", "fov_deg", "fisheye", "auto_exposure", "exposure_bias", "quality", "tile_error")
FISHEYE_FOV_RANGE_DEG = (10.0, 150.0)      # beyond ~155 deg a 16:9 frame's corners pass 90 deg off-axis
PARTICLE_KEYS = ("rain_drop_scale", "snow_flake_scale")
PARTICLE_SCALE_RANGE = (0.05, 20.0)
TIME_OF_DAY_RANGE = (0.0, 2400.0)
LAST_MINUTE = 59                           # times are HHMM; the plugin rejects minutes above 59
SUN_EDGE_MIN = 20                          # the sun-up check leaves this long either side of sunrise/sunset
IMAGE_SIZE_RANGE_PX = (64, 8192)
FOV_RANGE_DEG = (10.0, 170.0)
EXPOSURE_RANGE = (-15.0, 15.0)
QUALITY_LEVELS = (0, 1, 2, 3, 4)           # Unreal scalability: Low, Medium, High, Epic, Cinematic
TILE_ERROR_RANGE = (2.0, 32.0)             # below ~2 Cesium loads far more tiles for no visible gain
# Ultra Dynamic Sky's "Sun Scale" that draws the sun as large as the real one looks: its disc grows
# 40.7 px per unit at 1920 px / 75 deg, and the real sun (0.53 deg) is 11.6 px there (measured 2026-10-01).
SUN_SCALE_FOR_REAL_SIZE = 0.285
SUN_RANGES = {"yaw_deg": (0.0, 360.0), "pitch_deg": (-90.0, 90.0), "size": (0.1, 20.0), "intensity_lux": (0.0, 100.0),
              "sky_light": (0.0, 10.0)}
# Each CLOUDS key: the Ultra Dynamic Sky property it sets, and its range (None = True/False).
CLOUD_PROPERTIES = {
    "size":                  ("Volumetric Clouds Scale", (0.25, 4.0)),
    "thickness":             ("Layer Height Scale", (0.25, 4.0)),
    "density":               ("Extinction Scale", (0.5, 30.0)),
    "patchiness":            ("Macro Variation", (0.0, 1.0)),
    "patch_size":            ("Macro Scale", (0.25, 4.0)),
    "wisps_clear":           ("Cloud Wisps Opacity (Clear)", (0.0, 1.0)),
    "wisps_cloudy":          ("Cloud Wisps Opacity (Cloudy)", (0.0, 1.0)),
    "second_layer":          ("Two Layers", None),
    "second_layer_coverage": ("Layer 2 Coverage Scale", (0.0, 2.0)),
    "second_layer_density":  ("Layer 2 Extinction Scale", (0.01, 1.0)),
    "random_formation":      ("Randomize Cloud Formation on Run", None),
}
# Each CLOUDS on/off switch that has no sky property of its own: the settings it zeroes when False.
CLOUD_SWITCHES = {"wisps": ("wisps_clear", "wisps_cloudy")}
CLOUD_MARGIN_M = 300.0                     # cloud base must clear the highest waypoint by this much
VISIBILITY_RANGE_KM = (0.2, 1000.0)
# The haze in the pictures looks about 10x as thick as Unreal's height-fog formula alone gives (judged
# against real views, 2026-10-01): a formula 2 km looked like 200 m. Visibility is the formula's
# distance times this - for the sky placement, the run check and the metadata alike.
VISIBILITY_CALIBRATION = 0.1
# Ultra Dynamic Sky's height fog in clear weather as the plugin logs it - density at the fog plane and
# height falloff, engine units. The same in every city and at every time of day measured.
CLEAR_FOG_DENSITY = 0.0046
CLEAR_FOG_FALLOFF = 0.065
VISIBILITY_CONTRAST_BITS = math.log2(20.0) # 5% contrast left, in Unreal's base-2 fog transmittance
FORBIDDEN_NAME_CHARS = '<>:"/\\|?* '       # location names become folder names

# The kind of value each setting must be - checked first, so nothing reads a setting of the wrong kind.
SETTING_KINDS = {
    "WEATHER_ALL": (lambda v: isinstance(v, list), "a list of names"),
    "LOCATION": (lambda v: isinstance(v, list), "a list of {...} entries"),
    "AUTOMATE": (lambda v: isinstance(v, bool), "True or False"),
    "OUTPUT_DIR": (lambda v: isinstance(v, str) and bool(v.strip()), "a folder path in quotes"),
    "SUN": (lambda v: isinstance(v, dict), "a {...} table"),
    "CLOUD_BASE_ABOVE_GROUND_M": (lambda v: is_number(v), "a number"),
    "CLOUDS": (lambda v: isinstance(v, dict), "a {...} table"),
    "CLEAR_VISIBILITY_KM": (lambda v: is_number(v), "a number"),
    "WIND_FROM_DEG": (lambda v: isinstance(v, dict) and all(is_number(x) for x in v.values()),
                      "a {...} table of compass bearings"),
    "WIND_TOLERANCE_DEG": (lambda v: is_number(v), "a number"),
}

# --- Presets, as looked up by condition name ------------------------------------------------------

PRESETS: Dict[str, dict] = {name: {"asset": row[0], **dict(zip(PRESET_CHANNELS, row[1:]))}
                            for name, row in PRESET_TABLE.items()}

# --- Check tolerances ---------------------------------------------------------------------------------

# "The recorded value is what was asked for". Each is about what rendering can legitimately wobble by,
# or the precision the plugin prints with - not a place to hide errors.
CHANNEL_TOLERANCE = 0.25                   # live weather vs preset, 0-10 channels
POSE_TOLERANCE = 0.01                      # cm / degrees between conditions at one waypoint
FOV_TOLERANCE_DEG = 0.01
HEIGHT_SUM_TOLERANCE_M = 0.05              # altitude - ground = clearance, printed to 2 decimals
CLEARANCE_TOLERANCE_M = 1.0
TIME_OF_DAY_TOLERANCE = 1.0
PARTICLE_TOLERANCE = 0.001
ANGLE_TOLERANCE_DEG = 0.05                 # a spot's view against its limits; rotations print to 3 decimals
AREA_TOLERANCE_M = 5.0                     # the area check converts lat/lon to metres approximately
GROUND_RANGE_M = (-450.0, 8850.0)          # Dead Sea shore to Everest: any real ground

# The [Scene] log check.
SKY_TOLERANCE_M = 10.0
VISIBILITY_TOLERANCE = 0.1                 # share of CLEAR_VISIBILITY_KM; the density is logged to 4 decimals
CLOUD_TOLERANCE_M = 150.0                  # the cloud mode's single ground trace can land on a roof
DRONE_TOLERANCE_M = 5.0
SNOW_COVER_TOLERANCE = 0.05
MAX_PROBLEMS_SHOWN = 40

# --- Run-time estimate (--imagecount) ------------------------------------------------------------

RUN_START_S = 45.0          # saving the level at the first city and starting Unreal, once per run
CITY_SETUP_S = 10.0         # ground height, spawn and first tiles, per city
CONDITION_START_S = 8.0     # the plugin's weather transition (3 s) and settle (5 s), per condition
TELEPORT_CAPTURE_S = 2.2    # the plugin's waypoint settle (2 s) and the shutter - measured, tiles in memory
COLD_TILES_S = 2.0          # extra per picture in the first condition, while each spot's tiles first load
                            # (measured 4.2 s against 2.2 s; later conditions find them in tile memory)
SHADING_SWITCH_S = 14.0     # snow conditions use smooth shading; switching it reloads the tiles in view


class ConfigError(ValueError):
    """A setting is invalid. The message names it."""


def is_whole(value: object, bounds: Sequence[int]) -> bool:
    """A whole number within bounds."""
    return in_range(value, bounds) and value == int(value)


def is_number(value: object) -> bool:
    """A finite int or float - not True/False, not text, not NaN or infinity."""
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def in_range(value: float, bounds: Sequence[float]) -> bool:
    return is_number(value) and bounds[0] <= value <= bounds[1]


def near(a: Optional[float], b: Optional[float], tolerance: float) -> bool:
    """Both known and within tolerance. A missing value is never "near" anything."""
    return a is not None and b is not None and abs(float(a) - float(b)) <= tolerance


# --------------------------------------------------------------------------------------------------
# A city's run
# --------------------------------------------------------------------------------------------------


@dataclass
class City:
    """One location's run: where, which weathers, and its camera spots."""
    name: str
    lat: float
    lon: float
    weather: List[str]
    capture: dict                                         # the entry's SPOT_KEYS settings
    image: dict
    particles: dict
    automate: bool = field(default_factory=lambda: AUTOMATE)
    cloud_above_ground_m: float = field(default_factory=lambda: CLOUD_BASE_ABOVE_GROUND_M)

    # --- the camera spots (mirrors BuildShot in the plugin) ---------------------------------------

    def shot_type(self) -> ShotType:
        return SHOT_TYPES[self.capture["shot"]["type"]]

    def pictures_per_spot(self) -> int:
        return self.shot_type().pictures(self.capture["shot"])

    def waypoint_count(self) -> int:
        return int(self.capture["spots"]) * self.pictures_per_spot()

    def spot_of(self, waypoint: int) -> Tuple[int, int]:
        """(spot, picture within its shot) of capture point `waypoint`."""
        return divmod(waypoint, self.pictures_per_spot())

    def start_time(self) -> float:
        """The middle of the time_of_day range, HHMM: the sky shown before the first spot, and with
        AUTOMATE False the time of the view you choose."""
        middle = round(sum(hhmm_minutes(time) for time in self.capture["time_of_day"]) / 2)
        return float(middle // 60 * 100 + middle % 60)

    def expected_images(self) -> int:
        return len(self.weather) * self.waypoint_count()

    def estimated_seconds(self) -> float:
        """Rough capture time for this location, from the plugin's own waits."""
        per_condition = CONDITION_START_S + TELEPORT_CAPTURE_S * self.waypoint_count()
        first_load = COLD_TILES_S * self.waypoint_count()
        return (CITY_SETUP_S + len(self.weather) * per_condition + first_load
                + SHADING_SWITCH_S * self.shading_switches())

    def shading_switches(self) -> int:
        """How often the run switches between snow (smooth shading) and other weather - each reloads
        the tiles in view. Listing the snow conditions together keeps this low."""
        smooth = [needs_smooth_shading(name) for name in self.weather]
        return sum(1 for before, after in zip([False] + smooth, smooth) if before != after)

    def snapshot_dir(self) -> Path:
        return OUTPUT_ROOT / "WeatherSnapshots" / self.name     # mirrors the plugin's ResolveOutputDirs

    # --- checks -----------------------------------------------------------------------------------

    def validate(self) -> List[str]:
        """Every problem with this location's settings; empty means it can run."""
        problems = self._place_problems() + self._weather_problems() + self._look_problems()
        capture_problems = self._capture_problems()
        # The cloud check reads the height limits, so only once those are sound.
        return problems + (capture_problems or self._cloud_problems())

    def _capture_problems(self) -> List[str]:
        """The camera spots: counts whole and in range, every [low, high] inside its bounds."""
        capture, problems = self.capture, []
        if not is_whole(capture["spots"], SPOT_COUNT_RANGE):
            problems.append(f"spots {capture['spots']} must be a whole number within {SPOT_COUNT_RANGE}")
        if not in_range(capture["area_km2"], AREA_RANGE_KM2):
            problems.append(f"area_km2 {capture['area_km2']} must be within {AREA_RANGE_KM2}")
        for key, bounds in CAPTURE_RANGES.items():
            low, high = capture[key]
            if not (in_range(low, bounds) and in_range(high, bounds)):
                problems.append(f"{key} {list(capture[key])} must be within {bounds}")
            elif key != "yaw_deg" and low > high:         # a yaw range may run on past north
                problems.append(f"{key} {list(capture[key])}: the low end is above the high end")
            elif key == "time_of_day" and any(time % 100 > LAST_MINUTE for time in (low, high)):
                problems.append(f"time_of_day {list(capture[key])} is HHMM: the last two digits are "
                                f"minutes, 00..59 (1330 = 13:30)")
        shot = capture["shot"]
        problems += [f"shot {shot['type']} {name} {shot[name]} must be {limits}"
                     for name, limits in self.shot_type().parameters.items() if not limits.holds(shot[name])]
        return problems

    def _place_problems(self) -> List[str]:
        problems = []
        if not isinstance(self.name, str) or not self.name or any(char in self.name for char in FORBIDDEN_NAME_CHARS):
            problems.append(f"name '{self.name}' must be a plain folder name")
        if not (in_range(self.lat, (-90, 90)) and in_range(self.lon, (-180, 180))):
            problems.append(f"lat/lon {self.lat}, {self.lon} out of range (west and south are negative)")
        return problems

    def _weather_problems(self) -> List[str]:
        problems = []
        if not self.weather:
            problems.append("weather is empty - nothing to capture")
        problems += [f"weather '{name}' is not a preset; use one of {', '.join(PRESETS)}"
                     for name in self.weather if name not in PRESETS]
        if len(set(self.weather)) != len(self.weather):
            problems.append("weather lists a condition twice")
        problems += [f"particles {size} {value} must be within {PARTICLE_SCALE_RANGE}"
                     for size, value in self.particles.items() if not in_range(value, PARTICLE_SCALE_RANGE)]
        return problems

    def _look_problems(self) -> List[str]:
        """Image and sun."""
        image = self.image
        problems = [f"image {side} {image[side]} must be a whole number within {IMAGE_SIZE_RANGE_PX}"
                    for side in ("width", "height") if not is_whole(image[side], IMAGE_SIZE_RANGE_PX)]
        fov_range = FISHEYE_FOV_RANGE_DEG if image["fisheye"] is True else FOV_RANGE_DEG
        if not in_range(image["fov_deg"], fov_range):
            problems.append(f"image fov_deg {image['fov_deg']} must be within {fov_range}"
                            + (" with fisheye" if image["fisheye"] is True else ""))
        problems += [f"image {key} must be True or False" for key in ("fisheye", "auto_exposure")
                     if not isinstance(image[key], bool)]
        if not in_range(image["exposure_bias"], EXPOSURE_RANGE):
            problems.append(f"image exposure_bias {image['exposure_bias']} must be within {EXPOSURE_RANGE}")
        if image["quality"] not in QUALITY_LEVELS or isinstance(image["quality"], bool):
            problems.append(f"image quality {image['quality']} must be one of {QUALITY_LEVELS}")
        if not in_range(image["tile_error"], TILE_ERROR_RANGE):
            problems.append(f"image tile_error {image['tile_error']} must be within {TILE_ERROR_RANGE}")
        problems += [f"SUN {key} {SUN[key]} must be within {bounds}"
                     for key, bounds in SUN_RANGES.items() if not in_range(SUN[key], bounds)]
        problems += [f"SUN {key} must be True or False" for key in ("real", "lens_flare")
                     if not isinstance(SUN[key], bool)]
        for key in ("sunrise", "sunset"):
            if not in_range(SUN[key], TIME_OF_DAY_RANGE) or SUN[key] % 100 > LAST_MINUTE:
                problems.append(f"SUN {key} {SUN[key]} must be a time HHMM within {TIME_OF_DAY_RANGE}")
        if is_number(SUN["sunrise"]) and is_number(SUN["sunset"]) and SUN["sunrise"] >= SUN["sunset"]:
            problems.append(f"SUN sunrise {SUN['sunrise']:g} must be before sunset {SUN['sunset']:g}")
        if sun_date() is None:
            problems.append(f"SUN date {SUN['date']!r} must be a real date written YYYY-MM-DD")
        if not in_range(CLEAR_VISIBILITY_KM, VISIBILITY_RANGE_KM):
            problems.append(f"CLEAR_VISIBILITY_KM {CLEAR_VISIBILITY_KM} must be within {VISIBILITY_RANGE_KM}")
        return problems + cloud_problems()

    def _cloud_problems(self) -> List[str]:
        highest = self.capture["height_m"][1]
        if self.cloud_above_ground_m >= highest + CLOUD_MARGIN_M:
            return []
        return [f"cloud base {self.cloud_above_ground_m:.0f} m above ground must be at least "
                f"{CLOUD_MARGIN_M:.0f} m above the highest spot ({highest:.0f} m above ground): "
                f"rain and snow switch off above the clouds"]

    # --- ini --------------------------------------------------------------------------------------

    def to_ini(self, sites: Sequence["City"] = ()) -> Dict[str, object]:
        """Every key must be a Config UPROPERTY of AMyActor_weather (tested). With sites, one session
        visits each of them in turn (this location is the first); each gets its own folder, spots and
        conditions from its CaptureSites entry."""
        values = {**self._scene_ini(), **self.site_ini()}
        if sites:
            values["CaptureSites"] = [site_struct(site) for site in sites]
        return values

    def _scene_ini(self) -> Dict[str, object]:
        return {
            "bAutoStartPipeline": False,             # automated runs pass -WeatherAutoStart instead
            "bManualFrame": not self.automate,       # the user flies to the first spot and presses P
            "bUseFlightPath": True,                  # the camera spots
            "bStartWithLitmus": False,               # P goes straight to the run; Play previews
            "bSetGeoreferenceOrigin": False,          # the level is moved and saved beforehand
            "OutputRootDir": OUTPUT_ROOT.as_posix(),
            "OutputSubfolder": self.name,
            "CaptureConditionFilter": list(self.weather),
            "bApplySunSettings": not SUN["real"],     # the regular day-night cycle
            "bUseGeoreferencedSun": SUN["real"],      # the real sun for the location and date
            "SunYear": sun_date().year,
            "SunMonth": sun_date().month,
            "SunDay": sun_date().day,
            "SunNorthYawDeg": unreal_yaw(0.0),        # which way north is in the georeferenced world
            "SunYawDeg": SUN["yaw_deg"],
            "SunPitchDeg": SUN["pitch_deg"],
            "SunLightIntensityLux": SUN["intensity_lux"],
            "SunriseTime": SUN["sunrise"],
            "SunsetTime": SUN["sunset"],
            "SunScale": SUN["size"] * SUN_SCALE_FOR_REAL_SIZE,
            "bSunLensFlare": SUN["lens_flare"],
            "bCalibrateOriginToGround": True,         # measures the ground, puts Z = 0 on it
            "bSpawnAtHeightAboveGround": True,       # the drone waits above the buildings
            "bLitmusOnPlay": False,                  # weather changes only when the capture starts
            "bFollowTerrain": True,                  # each spot's height is above the ground under it
            "bCalculateOriginHeight": True,           # ground height from the tileset -> Z = 0
            "CloudAltitudeMode": "AboveTerrainAtStart",
            "CloudHeightAboveGroundM": self.cloud_above_ground_m,
            "VisibilityCalibration": VISIBILITY_CALIBRATION,   # the metadata's visibility_km uses it too
        }

    def site_ini(self) -> Dict[str, object]:
        """What differs between the locations of one session: the camera spots and their limits, the
        pictures and particles, and the zone the real sun's time is in - the sun's own time at the
        location's longitude."""
        capture = self.capture
        yaw_from, yaw_to = capture["yaw_deg"]
        return {
            "SpotCount": int(capture["spots"]),
            "SpotAreaKm2": capture["area_km2"],
            "SpotSeed": -1 if capture["seed"] is None else int(capture["seed"]),
            "SpotHeightMinM": capture["height_m"][0],
            "SpotHeightMaxM": capture["height_m"][1],
            "SpotPitchMinDeg": capture["pitch_deg"][0],
            "SpotPitchMaxDeg": capture["pitch_deg"][1],
            "SpotYawStartDeg": unreal_yaw(yaw_from),
            "SpotYawSpanDeg": yaw_span_deg(yaw_from, yaw_to),
            "SpotRollMinDeg": capture["roll_deg"][0],
            "SpotRollMaxDeg": capture["roll_deg"][1],
            "SpotTimeMin": capture["time_of_day"][0],
            "SpotTimeMax": capture["time_of_day"][1],
            "CaptureShot": capture["shot"]["type"],
            "ShotParameters": shot_parameters_text(capture["shot"]),
            "bFlyBetweenWaypoints": False,           # random jumps: straight to each spot
            "TimeOfDay": self.start_time(),
            "SpawnHeightAboveGroundM": sum(capture["height_m"]) / 2,   # where the drone waits
            "TimeZoneHours": self.lon / DEG_PER_SUN_HOUR,
            "CaptureResolutionX": int(self.image["width"]),
            "CaptureResolutionY": int(self.image["height"]),
            "CaptureFOV": self.image["fov_deg"],
            "bFisheyeLens": self.image["fisheye"],
            "bCaptureAutoExposure": self.image["auto_exposure"],
            "CaptureExposureBias": self.image["exposure_bias"],
            "RenderQuality": self.image["quality"],
            "CesiumMaximumScreenSpaceError": float(self.image["tile_error"]),
            "RainDropScale": self.particles["rain_drop_scale"],
            "SnowFlakeScale": self.particles["snow_flake_scale"],
        }

    def ini_block(self, sites: Sequence["City"] = ()) -> str:
        names = ", ".join(site.name for site in sites) or self.name
        lines = [BEGIN_MARKER, f"; {names}: random camera spots", f"[{INI_SECTION}]"]
        for key, value in self.to_ini(sites).items():
            if isinstance(value, list):
                lines += [f"+{key}={format_ini_value(item)}" for item in value]   # +Key appends
            else:
                lines.append(f"{key}={format_ini_value(value)}")
        return "\n".join(lines + [END_MARKER]) + "\n"

    def write_ini(self, path: Path = CONFIG_INI, sites: Sequence["City"] = ()) -> None:
        """Replaces the generated block in DefaultGame.ini; everything else in the file is kept."""
        problems = [f"{city.name}: {problem}" for city in (sites or [self]) for problem in city.validate()]
        if problems:
            raise ConfigError("; ".join(problems))
        existing = path.read_text(encoding="utf-8") if path.exists() else ""
        if BEGIN_MARKER in existing and END_MARKER in existing:
            head = existing.split(BEGIN_MARKER)[0]
            tail = existing.split(END_MARKER, 1)[1].lstrip("\n")
            text = head + self.ini_block(sites) + tail
        else:
            separator = "" if existing.endswith("\n") or not existing else "\n"
            text = existing + separator + "\n" + self.ini_block(sites)
        path.write_text(text, encoding="utf-8")


def site_struct(city: City) -> str:
    """One CaptureSites entry: where, when and what weather - plus the ground height when an earlier
    run calculated it, so the globe lands with Z = 0 near the ground and the tiles load in place."""
    fields = [f'Name="{city.name}"', f"Latitude={format_ini_value(float(city.lat))}",
              f"Longitude={format_ini_value(float(city.lon))}"]
    height = known_ground_height(city)
    if height is not None:
        fields.append(f"HeightM={format_ini_value(float(height))}")
    fields.append(f"TimeOfDay={format_ini_value(city.start_time())}")
    fields.append("Conditions=(" + ",".join(f'"{name}"' for name in city.weather) + ")")
    fields.append("Settings=(" + ",".join(f'"{key}={format_ini_value(value)}"'
                                          for key, value in city.site_ini().items()) + ")")
    return "(" + ",".join(fields) + ")"


def shot_parameters_text(shot: dict) -> str:
    """A shot's parameters as the plugin's TMap<FName, float> reads them: ((radius_m,100),(pictures,8))."""
    return "(" + ",".join(f"({name},{format_ini_value(float(value))})"
                          for name, value in shot.items() if name != "type") + ")"


def needs_smooth_shading(condition: str) -> bool:
    """Snow lying on the ground and roofs: the plugin turns Cesium's smooth shading on for it (its table
    marks exactly these conditions - tested)."""
    preset = PRESETS.get(condition)
    return bool(preset) and preset["material_snow_coverage"] > 0.0


def sun_date() -> Optional[datetime.date]:
    """SUN date as a date, or None when it is not a real YYYY-MM-DD date."""
    try:
        return datetime.date.fromisoformat(str(SUN["date"]))
    except ValueError:
        return None


def visibility_km(density: float, falloff: float, plane_m: float) -> float:
    """Street-level visibility under an Unreal height fog, from its density at the fog plane, its
    falloff and the plane's height above the ground. Unreal (SceneCore.cpp, HeightFogCommon.ush):
    transmittance 2^-(density/1000 per cm of path), the density halving every 10/falloff m upward -
    times VISIBILITY_CALIBRATION, to what the pictures look like."""
    ground_density_per_m = density / 10.0 * 2.0 ** (falloff * plane_m / 10.0)
    return VISIBILITY_CALIBRATION * VISIBILITY_CONTRAST_BITS / ground_density_per_m / 1000.0


def sky_anchor_above_ground_m() -> float:
    """Where the sky actor - the height fog plane - goes for CLEAR_VISIBILITY_KM: visibility_km solved
    for the plane, with the clear weather's fog."""
    formula_km = CLEAR_VISIBILITY_KM / VISIBILITY_CALIBRATION
    wanted_density_per_m = VISIBILITY_CONTRAST_BITS / (formula_km * 1000.0)
    return 10.0 / CLEAR_FOG_FALLOFF * math.log2(wanted_density_per_m * 10.0 / CLEAR_FOG_DENSITY)


def cloud_problems() -> List[str]:
    """Every CLOUDS key known and in range; the True/False ones really True or False."""
    known = {**CLOUD_SWITCHES, **CLOUD_PROPERTIES}
    problems = [f"CLOUDS has no setting '{key}'; use {', '.join(known)}" for key in CLOUDS if key not in known]
    problems += [f"CLOUDS {key} must be True or False" for key in CLOUD_SWITCHES
                 if not isinstance(CLOUDS.get(key), bool)]
    for key, (_, bounds) in CLOUD_PROPERTIES.items():
        value = CLOUDS.get(key)
        if bounds is None and not isinstance(value, bool):
            problems.append(f"CLOUDS {key} must be True or False")
        elif bounds is not None and (isinstance(value, bool) or not isinstance(value, (int, float))
                                     or not in_range(value, bounds)):
            problems.append(f"CLOUDS {key} {value} must be within {bounds}")
    return problems


def yaw_span_deg(yaw_from: float, yaw_to: float) -> float:
    """Degrees clockwise from `yaw_from` round to `yaw_to`: [0, 360] is all the way round, [300, 60] 120."""
    span = (yaw_to - yaw_from) % 360.0
    return 360.0 if span == 0.0 and yaw_to != yaw_from else span


def compass_in_range(compass: float, yaw_range: Sequence[float]) -> bool:
    """Whether a compass bearing lies in a yaw_deg [from, to] range (within ANGLE_TOLERANCE_DEG)."""
    past_start = (compass - yaw_range[0]) % 360.0
    return (past_start <= yaw_span_deg(*yaw_range) + ANGLE_TOLERANCE_DEG
            or 360.0 - past_start <= ANGLE_TOLERANCE_DEG)


def unreal_yaw(compass_deg: float) -> float:
    """Compass bearing (0 = north, 90 = east) to the Unreal yaw of the georeferenced world."""
    return (compass_deg + COMPASS_TO_UNREAL_YAW_DEG) % 360.0


def format_ini_value(value: object) -> str:
    """Unreal wants True/False and no scientific notation."""
    if isinstance(value, bool):
        return "True" if value else "False"
    if isinstance(value, float):
        return f"{value:.6f}".rstrip("0").rstrip(".") or "0"
    return str(value)


def build_cities() -> List[City]:
    """The LOCATION entries as City objects.
    Raises ConfigError naming every setting that is missing a part, before anything else reads it."""
    problems = setting_shape_problems()
    if problems:
        raise ConfigError("\n  - " + "\n  - ".join(problems))
    return [_build_city(entry) for entry in LOCATION]


def setting_shape_problems() -> List[str]:
    """Every setting has the shape the pipeline reads, so a wrong kind of value, a deleted or misspelled
    key, or text where a number belongs is named here rather than crashing later. In stages, each only
    once the one before holds."""
    settings = globals()
    problems = [f"{name} must be {kind}" for name, (check, kind) in SETTING_KINDS.items()
                if not check(settings[name])]
    if problems:
        return problems
    if not LOCATION:
        return ["LOCATION is empty - list at least one location"]
    problems = [f"LOCATION entry {entry!r} must be a {{...}} table" for entry in LOCATION if not isinstance(entry, dict)]
    return problems or _entry_problems()


def _entry_problems() -> List[str]:
    """The keys of every table, and the kind of every value the run computes with before validating."""
    problems = []
    for entry in LOCATION:
        where = f"LOCATION entry {entry.get('name', entry)}"
        problems += _key_problems(where, entry, LOCATION_KEYS + SPOT_KEYS, LOCATION_KEYS + SPOT_KEYS)
        problems += _capture_kind_problems(where, entry)
        problems += _sub_table_problems(where, entry)
        if "name" in entry and not isinstance(entry["name"], str):
            problems.append(f"{where} needs a name in quotes")
        weather = entry.get("weather", [])
        if not isinstance(weather, list) or not all(isinstance(name, str) for name in weather):
            problems.append(f"{where} weather must be a list of names from WEATHER_ALL")
    names = [str(entry.get("name", "")).lower() for entry in LOCATION]
    if len(set(names)) != len(names):
        problems.append("two LOCATION entries share a name; names become output folders")
    problems += _key_problems("SUN", SUN, SUN_KEYS, SUN_KEYS)
    return sorted(set(problems), key=problems.index)      # each once, in order


def _sub_table_problems(where: str, entry: dict) -> List[str]:
    """An entry's "image" and "particles": {...} tables with their keys, and numbers where numbers go."""
    problems = []
    for name, keys in (("image", IMAGE_KEYS), ("particles", PARTICLE_KEYS)):
        if name not in entry:
            continue                                      # named as a missing key already
        if not isinstance(entry[name], dict):
            problems.append(f"{where} {name} must be a {{...}} table")
            continue
        problems += _key_problems(f"{where} {name}", entry[name], keys, keys)
    return problems


def _key_problems(setting: str, table: dict, known: Sequence[str], required: Sequence[str]) -> List[str]:
    """Required keys that are missing, and keys the pipeline does not know."""
    missing = [key for key in required if key not in table]
    unknown = [str(key) for key in table if key not in known]
    return ([f"{setting} is missing {', '.join(missing)}"] if missing else []) + \
           ([f"{setting} has unknown key(s) {', '.join(unknown)}; use {', '.join(known)}"] if unknown else [])


def _kind_problems(setting: str, table: dict, texts: Sequence[str] = (), numbers: Sequence[str] = ()) -> List[str]:
    """Values present in the table that are not the kind they must be."""
    kinds = [(texts, lambda v: isinstance(v, str), "text in quotes"), (numbers, is_number, "a number")]
    return [f"{setting} {key} {table[key]!r} must be {kind}"
            for keys, check, kind in kinds for key in keys if key in table and not check(table[key])]


def _capture_kind_problems(setting: str, table: dict) -> List[str]:
    """Camera-spot values of the wrong kind: [low, high] not two numbers, a count that is text..."""
    problems = _kind_problems(setting, table, numbers=("spots", "area_km2"))
    if "shot" in table:
        problems += _shot_shape_problems(setting, table["shot"])
    problems += [f"{setting} {key} {table[key]!r} must be [low, high]: two numbers" for key in CAPTURE_RANGES
                 if key in table and not (isinstance(table[key], (list, tuple)) and len(table[key]) == 2
                                          and all(is_number(value) for value in table[key]))]
    seed = table.get("seed")
    if "seed" in table and seed is not None and not (is_number(seed) and seed == int(seed) and seed >= 0):
        problems.append(f"{setting} seed {seed!r} must be a whole number of 0 or more, or None")
    return problems


def _shot_shape_problems(setting: str, shot: object) -> List[str]:
    """A {"type": ...} table naming a shot in SHOT_TYPES, with exactly that shot's parameters, all numbers."""
    where = f"{setting} shot"
    if not isinstance(shot, dict) or not isinstance(shot.get("type"), str):
        return [f'{where} {shot!r} must be a {{"type": ...}} table, e.g. {{"type": "frame"}}']
    if shot["type"] not in SHOT_TYPES:
        return [f"{where} type {shot['type']!r} must be one of {', '.join(SHOT_TYPES)} "
                f"(your own: a class in CaptureShots.h and an entry in SHOT_TYPES)"]
    keys = ("type",) + tuple(SHOT_TYPES[shot["type"]].parameters)
    return _key_problems(where, shot, keys, keys) + _kind_problems(where, shot, numbers=keys[1:])


def _build_city(entry: dict) -> City:
    return City(name=entry["name"], lat=entry["lat"], lon=entry["lon"], weather=list(entry["weather"]),
                image=dict(entry["image"]), particles=dict(entry["particles"]),
                capture={key: _own_copy(entry[key]) for key in SPOT_KEYS})


def _own_copy(value: object) -> object:
    """A list or table copied, so a City never shares one with LOCATION."""
    if isinstance(value, (list, tuple)):
        return list(value)
    return dict(value) if isinstance(value, dict) else value


# --------------------------------------------------------------------------------------------------
# Running Unreal
# --------------------------------------------------------------------------------------------------


def find_editor_cmd() -> Optional[Path]:
    roots = ([os.environ["UE_ENGINE_DIR"]] if os.environ.get("UE_ENGINE_DIR") else []) + ENGINE_DIRS
    for root in roots:
        exe = Path(root) / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"
        if exe.exists():
            return exe
    return None


def editor_is_open() -> bool:
    """True if the Unreal editor (not a headless -Cmd run) is running. It holds the level file open, so
    the level cannot be saved at the city's coordinates and nothing can be captured."""
    return _is_running("UnrealEditor.exe")


def session_is_running() -> bool:
    """True if a headless Unreal (an earlier capture, still going or stuck) is running. Two sessions
    would share the ini, the level and the run log, and each would break the other's checks."""
    return _is_running("UnrealEditor-Cmd.exe")


def _is_running(image_name: str) -> bool:
    if sys.platform != "win32":
        return False
    result = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image_name}", "/NH"],
                            capture_output=True, text=True)
    return image_name.lower() in result.stdout.lower()


def set_level_location(city: City, editor: Path, origin_height_m: Optional[float] = None) -> bool:
    """
    Writes the city into the level's CesiumGeoreference and saves it, by running this same file
    inside a headless editor - latitude, longitude and, when known, the calculated ground height.
    Returns False if the level was not saved - the caller must then not capture, or it photographs
    whatever city the level was last saved at.
    """
    if SITE_RESULT.exists():
        SITE_RESULT.unlink()
    env = dict(os.environ, WP_INSIDE_UNREAL="1", WP_LAT=str(city.lat), WP_LON=str(city.lon),
               WP_LEVEL=LEVEL, WP_RESULT=str(SITE_RESULT), WP_SKY=json.dumps(sky_level_settings()))
    if origin_height_m is not None:
        env["WP_HEIGHT"] = str(origin_height_m)
    print(f"Moving the level to {city.name} ({city.lat}, {city.lon}) ...")
    try:
        subprocess.run([str(editor), str(UPROJECT), "-run=pythonscript",
                        f"-script={Path(__file__).resolve().as_posix()}",
                        "-unattended", "-nopause", "-nosplash", "-NullRHI"],
                       capture_output=True, text=True, env=env, timeout=LEVEL_SAVE_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        print(f"  The level save did not finish within {LEVEL_SAVE_TIMEOUT_S // 60} minutes.", file=sys.stderr)
        return False
    result = SITE_RESULT.read_text(encoding="utf-8") if SITE_RESULT.exists() else ""
    saved, offsets = parse_site_result(result)
    if "NOSKY" in result.split():
        print("  warning: no Ultra Dynamic Sky actor in the level - CLOUDS was not applied.", file=sys.stderr)
    for name, z_cm in offsets:
        print(f"  warning: tileset '{name}' is saved at Z={z_cm:.0f} cm. It should be at 0: the plugin "
              f"moves it back at Play, but the editor view will not match the capture.", file=sys.stderr)
    print("  Level saved." if saved else "  The level was NOT saved.", file=sys.stdout if saved else sys.stderr)
    return saved


def sky_level_settings() -> dict:
    """What the level save writes onto Ultra Dynamic Sky, as this process has the settings - handed to the
    headless editor, which would otherwise re-read this file from disk and miss changes made in code."""
    off = {key for switch, keys in CLOUD_SWITCHES.items() if not CLOUDS[switch] for key in keys}
    clouds = {name: (0.0 if key in off else CLOUDS[key]) for key, (name, _) in CLOUD_PROPERTIES.items()}
    return {"clouds": clouds, "sky_light": float(SUN["sky_light"]), "lens_flare": SUN["lens_flare"],
            "anchor_m": sky_anchor_above_ground_m()}


def parse_site_result(text: str):
    """First line 'OK ...', then 'TILESET <name> <z>' per tileset. -> (ok, [(name, z) off origin])."""
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    offsets = []
    for line in lines[1:]:
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "TILESET":
            try:
                z_cm = float(parts[-1])
            except ValueError:
                continue
            if abs(z_cm) >= 1.0:
                offsets.append((" ".join(parts[1:-1]), z_cm))
    return bool(lines) and lines[0].startswith("OK"), offsets


def run_cities(cities: Sequence[City], editor: Path) -> int:
    """Capture every city in ONE Unreal session and check each. 0 = every image written and every
    check passed. The level is saved at the first city; the plugin moves the globe to each next one."""
    first = cities[0]
    first.write_ini(sites=cities)
    if not set_level_location(first, editor, known_ground_height(first)):
        print(f"Not capturing: the level is not at {first.name}'s coordinates.", file=sys.stderr)
        return 1
    session_started = time.time()
    if not capture_session(cities, editor):
        return 1
    return check_session(cities, editor, session_started)


def capture_session(cities: Sequence[City], editor: Path) -> bool:
    """Runs the one Unreal session over every city. False if it was interrupted."""
    for city in cities:
        city.snapshot_dir().mkdir(parents=True, exist_ok=True)
    expected = sum(city.expected_images() for city in cities)
    print(f"Capturing {expected} images in one session: " + ", ".join(city.name for city in cities))
    automate = cities[0].automate
    if not automate:
        print("  A game window is opening. At each location, fly to the first view you want and press P -\n"
              "  the controls lock, that view is the first spot and the camera jumps to the rest round it;\n"
              "  then the next location loads in the same window. The window closes after the last one.")
    started = time.time()
    if not capture_with_progress(editor, [city.snapshot_dir() for city in cities], expected, launch_args(automate, cities[0].image)):
        return False
    print(f"\n  Finished in {format_duration(time.time() - started)}")
    return True


def check_session(cities: Sequence[City], editor: Path, started: float) -> int:
    """Checks each city against its own part of the run log. 1 if any city has a problem. Only what
    this session wrote counts: a city it never reached still holds its previous run's files."""
    log_text = read_run_log()
    failed = 0
    for index, city in enumerate(cities):
        segment = site_log(log_text, index)
        out = city.snapshot_dir()
        written = images_written_since(out, started)
        print(f"\n--- {city.name}: {written}/{city.expected_images()} images ---")
        metadata = out / "weather_metadata.json"
        if metadata.exists() and metadata.stat().st_mtime < started:
            problems = [f"{city.name} was not captured in this run - its folder holds an earlier run's files"]
        else:
            problems = check_run(city, out, written, segment)
        report_problems(problems)
        # The level stays at the first city, so only its height is written into the level.
        save_calculated_ground_height(city, editor, segment, move_level=index == 0)
        failed += bool(problems)
    return 1 if failed else 0


def images_written_since(folder: Path, started: float) -> int:
    """PNGs in folder written at or after `started` (a time.time() value). A file can vanish between
    listing and reading - the plugin empties a city's folder as it starts there - and is not counted."""
    count = 0
    for image in folder.glob("*.png"):
        try:
            count += image.stat().st_mtime >= started
        except FileNotFoundError:
            continue
    return count


def read_run_log() -> str:
    """The last Unreal run's log, or "" when there is none."""
    return RUN_LOG.read_text(encoding="utf-8", errors="replace") if RUN_LOG.exists() else ""


def site_log(log_text: str, index: int) -> str:
    """The part of the run log for site `index`: from the plugin's "[Site n/N] <name> at" line to the
    next site's. The whole log when it has no site lines."""
    starts: Dict[int, int] = {}
    for match in re.finditer(r"\[Site (\d+)/\d+\] \S+ at -?\d", log_text):
        starts.setdefault(int(match.group(1)) - 1, match.start())   # first mention of each site
    if index not in starts:
        return log_text if not starts else ""
    later = [position for site, position in starts.items() if site > index]
    return log_text[starts[index]:min(later) if later else len(log_text)]


def parse_calculated_ground_height(log_text: str) -> Optional[float]:
    """The ground height the plugin calculated from the tileset (m above the ellipsoid), or None."""
    found = re.findall(r"Ground height calculated from the tileset: ([-\d.]+) m above the ellipsoid", log_text)
    return float(found[-1]) if found else None


def known_ground_height(city: City) -> Optional[float]:
    """The height calculated on an earlier run of this city at these coordinates, if any."""
    try:
        entry = json.loads(GROUND_HEIGHTS.read_text(encoding="utf-8")).get(city.name, {})
    except (OSError, ValueError):
        return None
    same_place = entry.get("lat") == city.lat and entry.get("lon") == city.lon
    return entry.get("ellipsoid_height_m") if same_place else None


def save_calculated_ground_height(city: City, editor: Path, log_text: Optional[str] = None,
                                  move_level: bool = True) -> None:
    """Remembers this run's calculated ground height for the city and, if it is new and move_level,
    writes it into the level. log_text defaults to the whole run log."""
    if log_text is None:
        log_text = read_run_log()
    height = parse_calculated_ground_height(log_text)
    if height is None:
        print("  note: the ground height was not calculated this run (collision probe used instead).")
        return
    previous = known_ground_height(city)
    if previous is not None and abs(previous - height) < GROUND_HEIGHT_CHANGE_M:
        return
    try:
        heights = json.loads(GROUND_HEIGHTS.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        heights = {}
    heights[city.name] = {"lat": city.lat, "lon": city.lon, "ellipsoid_height_m": height}
    GROUND_HEIGHTS.write_text(json.dumps(heights, indent=2), encoding="utf-8")
    if move_level:
        print(f"  Saving the calculated ground height ({height:.2f} m above the ellipsoid) into the level ...")
        set_level_location(city, editor, height)


def capture_with_progress(editor: Path, dirs: Sequence[Path], expected: int, launch_args: Sequence[str]) -> bool:
    """Runs the capture and prints a live count with an ETA. False if interrupted, or stopped because
    the city's tiles cannot load (the session would otherwise wait for them forever)."""
    try:
        RUN_LOG.unlink()          # the checks and the watcher must only ever read this session's log
    except FileNotFoundError:
        pass
    process = subprocess.Popen([str(editor), str(UPROJECT), LEVEL, *launch_args, f"-abslog={RUN_LOG}"])
    started = time.time()
    first_image_at: Optional[float] = None
    last_count = -1
    log_watch = LogWatch(RUN_LOG)
    try:
        while process.poll() is None:
            time.sleep(POLL_SECONDS)
            failure = log_watch.tileset_failure()
            if failure:
                process.terminate()
                print(f"\nStopped: {failure}", file=sys.stderr)
                return False
            # Only this run's images: a later city's folder still holds its previous run's until the
            # plugin reaches that city and empties it.
            written = sum(images_written_since(folder, started) for folder in dirs)
            now = time.time()
            if written and first_image_at is None:
                first_image_at = now
            if written != last_count:
                last_count = written
                eta = estimate_remaining_seconds(written, expected, first_image_at, now)
                print(f"\r  {written}/{expected} images  elapsed {format_duration(now - started)}  "
                      f"ETA {format_duration(eta) if eta is not None else '--:--'}   ", end="", flush=True)
    except KeyboardInterrupt:
        process.terminate()
        print("\nInterrupted.")
        return False
    return True


class LogWatch:
    """Reads what Unreal appends to its log between polls, to spot a failure while the run is live."""

    CARRY = 2048          # bytes kept from the last read, so a message split across reads is still seen

    def __init__(self, path: Path):
        self.path, self.position, self.tail = path, 0, ""

    def new_text(self) -> str:
        try:
            if self.path.stat().st_size < self.position:
                self.position, self.tail = 0, ""          # the log was started afresh
            with open(self.path, "rb") as handle:
                handle.seek(self.position)
                chunk = handle.read()
        except OSError:
            return ""
        self.position += len(chunk)
        text = self.tail + chunk.decode("utf-8", errors="replace")
        self.tail = text[-self.CARRY:]
        return text

    def tileset_failure(self) -> Optional[str]:
        """A plain explanation when Cesium says the tileset cannot load, else None. The log line holds
        the tile URL with the API key in it, so only the status code is reported."""
        match = re.search(r"Errors when loading tileset:\s*-?[^\n]*?status code (\d+)", self.new_text())
        if not match:
            return None
        code = match.group(1)
        reason = TILE_STATUS_REASONS.get(code, "the tile server refused the request")
        return f"the city's 3D tiles could not load (HTTP {code}: {reason})."


def check_run(city: City, out: Path, written: int, log_text: str) -> List[str]:
    """Image count, metadata records, sky placement and flight - every check a finished run must
    pass. log_text is this city's part of the run log."""
    problems = []
    if written < city.expected_images():
        problems.append(f"expected {city.expected_images()} images, got {written} (see {RUN_LOG.name})")
    problems += validate_metadata_file(out / "weather_metadata.json", city)
    return problems + check_scene_log(log_text, city) + check_terrain_log(log_text, city)


def check_terrain_log(log_text: str, city: City) -> List[str]:
    """Every capture point's ground measured (and the point placed in open air) at THIS city - one
    "[Terrain] Waypoint at (x, y): ground Z=" line each. Fewer means points were reused from the
    previous city, at its heights. The plugin's "still not in open air" warning is not a count."""
    measured = len(re.findall(r"\[Terrain\] Waypoint at \([^)]*\): ground Z=", log_text))
    if measured >= city.waypoint_count():
        return []
    return [f"only {measured} of {city.waypoint_count()} capture points were measured at {city.name} - "
            f"the rest kept another place's heights"]


def report_problems(problems: List[str]) -> None:
    for problem in problems[:MAX_PROBLEMS_SHOWN]:
        print(f"  - {problem}", file=sys.stderr)
    if len(problems) > MAX_PROBLEMS_SHOWN:
        print(f"  ... and {len(problems) - MAX_PROBLEMS_SHOWN} more", file=sys.stderr)
    print("  Checks: all passed." if not problems else f"  Checks: {len(problems)} problem(s).")


def estimate_remaining_seconds(written: int, expected: int, first_image_at: Optional[float],
                               now: float) -> Optional[float]:
    """Rate measured from the first image, so editor start-up does not inflate it."""
    if first_image_at is None or written < 2 or expected <= 0:
        return None
    return (now - first_image_at) / (written - 1) * max(expected - written, 0)


def format_duration(seconds: float) -> str:
    seconds = max(0, int(seconds))
    hours, rest = divmod(seconds, 3600)
    minutes, secs = divmod(rest, 60)
    if hours:
        return f"{hours}h {minutes:02d}m"
    return f"{minutes}m {secs:02d}s" if minutes else f"{secs}s"


# --------------------------------------------------------------------------------------------------
# Sky placement check, from the plugin's [Scene] log lines
# --------------------------------------------------------------------------------------------------

_SCENE_LINE = re.compile(
    r"\[Scene\] (?P<when>'[^']*' first capture), heights above the ground: drone (?P<drone>[-+\d.]+) m, "
    r"clearance (?P<clearance>[-+\d.]+) m.*?"
    r"height fog plane (?P<fog>[-+\d.]+) m, density (?P<fog_density>[\d.]+), falloff (?P<fog_falloff>[\d.]+)"
    r".*?atmosphere surface (?P<surface>[-+\d.]+) m.*?"
    r"cloud layer bottom (?P<cloud_km>[-\d.]+) km, height (?P<cloud_height_km>[-\d.]+) km"
    r"(?:[^\n]*?\| sun (?P<sun_deg>[-+\d.]+) deg(?:, from compass [\d.]+ deg at time (?P<sun_time>[\d.]+))?)?")


def parse_scene_lines(log_text: str) -> List[dict]:
    """
    The plugin's "[Scene] '<condition>' first capture" lines: every height-dependent layer in metres
    above the reference ground (measured at spawn), plus the drone's clearance over the ground right
    below it. -> [{"when", "drone", "clearance", "fog", "fog_density", "fog_falloff", "surface",
    "cloud_km", "cloud_height_km"}]
    """
    return [{key: (value if key == "when" or value is None else float(value))
             for key, value in match.groupdict().items()}
            for match in _SCENE_LINE.finditer(log_text)]


def check_scene_log(log_text: str, city: City) -> List[str]:
    """
    Checks, per condition, that fog, atmosphere and clouds sit where the settings put them relative
    to the ground. These decide the look and are not in the metadata, so they are checked from the
    plugin's own [Scene] log line - the same numbers in every city if the anchoring works.
    """
    lines = parse_scene_lines(log_text)
    if not lines:
        return ["no [Scene] lines in the run log - sky and cloud placement could not be checked"]
    problems = []
    anchor_m = sky_anchor_above_ground_m()
    for line in lines:
        tag = line["when"]
        for layer, label in (("fog", "height fog plane"), ("surface", "atmosphere surface")):
            if not near(line[layer], anchor_m, SKY_TOLERANCE_M):
                problems.append(f"{tag}: {label} {line[layer]:+.0f} m from the ground, "
                                f"placed at {anchor_m:+.0f} m for CLEAR_VISIBILITY_KM")
        seen_km = visibility_km(line["fog_density"], line["fog_falloff"], line["fog"])
        if tag.startswith("'clear'") and not near(seen_km, CLEAR_VISIBILITY_KM,
                                                  VISIBILITY_TOLERANCE * CLEAR_VISIBILITY_KM):
            problems.append(f"{tag}: visibility {seen_km:.1f} km, set to {CLEAR_VISIBILITY_KM:g} km")
        cloud_base_m = line["cloud_km"] * 1000.0 + line["surface"]
        if not near(cloud_base_m, city.cloud_above_ground_m, CLOUD_TOLERANCE_M):
            problems.append(f"{tag}: cloud base {cloud_base_m:.0f} m above the ground, "
                            f"set to {city.cloud_above_ground_m:.0f} m")
        if not SUN["real"] and line["sun_deg"] is not None and line["sun_time"] is not None:
            problems += _sun_problems(line["sun_deg"], line["sun_time"], tag)
        if city.automate and line["clearance"] < city.capture["height_m"][0] - DRONE_TOLERANCE_M:
            problems.append(f"{tag}: drone {line['clearance']:.0f} m above the ground at the first spot, "
                            f"below height_m {city.capture['height_m']}")
    return problems


def hhmm_minutes(time_hhmm: float) -> int:
    """1530 -> 930 minutes after midnight."""
    return int(time_hhmm) // 100 * 60 + int(time_hhmm) % 100


def _sun_problems(sun_deg: float, time_of_day: float, tag: str) -> List[str]:
    """The day-night cycle: the sun up between sunrise and sunset, down outside - away from the edges."""
    now, rise, down = (hhmm_minutes(t) for t in (time_of_day, SUN["sunrise"], SUN["sunset"]))
    if rise + SUN_EDGE_MIN < now < down - SUN_EDGE_MIN and sun_deg <= 0.0:
        return [f"{tag}: the sun is {sun_deg:+.1f} deg at {time_of_day:.0f}, but it should be up "
                f"(sunrise {SUN['sunrise']:.0f}, sunset {SUN['sunset']:.0f})"]
    if (now < rise - SUN_EDGE_MIN or now > down + SUN_EDGE_MIN) and sun_deg > 0.0:
        return [f"{tag}: the sun is {sun_deg:+.1f} deg at {time_of_day:.0f}, but it should be down "
                f"(sunrise {SUN['sunrise']:.0f}, sunset {SUN['sunset']:.0f})"]
    return []


# --------------------------------------------------------------------------------------------------
# Metadata check: is every recorded value what the engine should have produced?
# --------------------------------------------------------------------------------------------------


def png_size(path: Path):
    """(width, height) from the PNG header, or None."""
    try:
        with open(path, "rb") as handle:
            header = handle.read(24)
    except OSError:
        return None
    if len(header) < 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        return None
    return int.from_bytes(header[16:20], "big"), int.from_bytes(header[20:24], "big")


def validate_metadata(records: Sequence[dict], city: City, image_dir: Optional[Path] = None) -> List[str]:
    """Every way the records disagree with the run that produced them. Empty = all true."""
    problems: List[str] = []
    seen: set = set()
    first_pose: Dict[int, list] = {}
    centre = _area_centre(records, city)
    for index, record in enumerate(records):
        tag = record.get("image_file", f"record {index}")
        problems += _schema_problems(record, tag)
        if any(key not in record for key in METADATA_FIELDS):
            continue                                  # nothing further can be read reliably
        waypoint = record["waypoint_index"]
        if not (isinstance(waypoint, int) and 0 <= waypoint < city.waypoint_count()):
            problems.append(f"{tag}: waypoint_index {waypoint} outside 0..{city.waypoint_count() - 1}")
            continue                                  # every later check is per waypoint
        problems += _identity_problems(record, tag, city, seen)
        problems += _camera_problems(record, tag, city, first_pose)
        problems += _image_problems(record, tag, city, image_dir)
        problems += _position_problems(record, tag, city, centre)
        problems += _weather_problems(record, tag, city)
    return problems + _completeness_problems(city, seen)


def _area_centre(records: Sequence[dict], city: City) -> Optional[Tuple[float, float]]:
    """(lat, lon) the spots' square is centred on: the location, or with AUTOMATE False the user's own
    first view."""
    if city.automate:
        return city.lat, city.lon
    first = next((r for r in records if r.get("waypoint_index") == 0 and r.get("latitude_deg") is not None
                  and r.get("longitude_deg") is not None), None)
    return (first["latitude_deg"], first["longitude_deg"]) if first else None


def users_view(record: dict, city: City) -> bool:
    """With AUTOMATE False the first spot is the user's own view, taken as it is - not drawn."""
    return not city.automate and city.spot_of(record["waypoint_index"])[0] == 0


def _schema_problems(record: dict, tag: str) -> List[str]:
    problems = []
    missing = [key for key in METADATA_FIELDS if key not in record]
    extra = [key for key in record if key not in METADATA_FIELDS]
    nulls = [key for key in METADATA_FIELDS if key in record and record[key] is None]
    if missing:
        problems.append(f"{tag}: missing fields {missing}")
    if extra:
        problems.append(f"{tag}: undeclared fields {extra}")
    if nulls:
        problems.append(f"{tag}: not measured (null): {nulls}")
    return problems


def _identity_problems(record: dict, tag: str, city: City, seen: set) -> List[str]:
    problems = []
    condition, waypoint = record["condition"], record["waypoint_index"]
    if condition not in city.weather:
        problems.append(f"{tag}: condition '{condition}' was not requested")
    if (condition, waypoint) in seen:
        problems.append(f"{tag}: ({condition}, waypoint {waypoint}) recorded twice")
    seen.add((condition, waypoint))
    expected_name = f"{condition}_p{waypoint:02d}.png"
    if record["image_file"] != expected_name:
        problems.append(f"{tag}: image_file should be {expected_name}")
    return problems


def _camera_problems(record: dict, tag: str, city: City, first_pose: Dict[int, list]) -> List[str]:
    """The configured lens, the spot's view inside its limits, and one pose per waypoint across every
    condition."""
    problems = []
    if not near(record["camera_fov_deg"], city.image["fov_deg"], FOV_TOLERANCE_DEG):
        problems.append(f"{tag}: camera_fov_deg {record['camera_fov_deg']}, asked for {city.image['fov_deg']}")
    lens = "fisheye" if city.image["fisheye"] else "rectilinear"
    if record["lens"] != lens:
        problems.append(f"{tag}: lens {record['lens']!r}, asked for {lens!r}")
    if not users_view(record, city):
        pitch, yaw, roll = record["camera_rotation"]
        for key, value in (("pitch_deg", pitch), ("roll_deg", roll)):
            low, high = city.capture[key]
            if not low - ANGLE_TOLERANCE_DEG <= value <= high + ANGLE_TOLERANCE_DEG:
                problems.append(f"{tag}: camera {key[:-4]} {value} is outside {key} [{low}, {high}]")
        compass = (yaw - COMPASS_TO_UNREAL_YAW_DEG) % 360.0
        if city.spot_of(record["waypoint_index"])[1] == 0 and not compass_in_range(compass, city.capture["yaw_deg"]):
            problems.append(f"{tag}: the camera faces compass {compass:.1f}, outside yaw_deg {city.capture['yaw_deg']}")
    pose = list(record["camera_location"]) + list(record["camera_rotation"])
    reference = first_pose.setdefault(record["waypoint_index"], pose)
    if any(not near(a, b, POSE_TOLERANCE) for a, b in zip(reference, pose)):
        problems.append(f"{tag}: camera pose differs from another condition at waypoint "
                        f"{record['waypoint_index']} - not a matched pair")
    return problems


def _image_problems(record: dict, tag: str, city: City, image_dir: Optional[Path]) -> List[str]:
    """The record describes the file, and the file is the configured size."""
    problems = []
    recorded = (record["image_width"], record["image_height"])
    if recorded != (city.image["width"], city.image["height"]):
        problems.append(f"{tag}: image is {recorded[0]}x{recorded[1]}, "
                        f"asked for {city.image['width']}x{city.image['height']}")
    if image_dir is not None:
        size = png_size(image_dir / record["image_file"])
        if size is None:
            problems.append(f"{tag}: image file missing or not a PNG")
        elif size != recorded:
            problems.append(f"{tag}: PNG is {size[0]}x{size[1]}, record says {recorded[0]}x{recorded[1]}")
    return problems


def _position_problems(record: dict, tag: str, city: City, centre: Optional[Tuple[float, float]]) -> List[str]:
    """Inside the spots' square, at a height inside the limits over the measured ground."""
    problems = []
    lat, lon = record["latitude_deg"], record["longitude_deg"]
    if centre and lat is not None and lon is not None:
        east_m = (lon - centre[1]) * M_PER_DEG_LON_AT_EQUATOR * math.cos(math.radians(centre[0]))
        north_m = (lat - centre[0]) * M_PER_DEG_LAT
        shot_m = city.shot_type().reach_m(city.capture["shot"])
        reach_m = math.sqrt(city.capture["area_km2"]) * 500.0 + shot_m + AREA_TOLERANCE_M
        if abs(east_m) > reach_m or abs(north_m) > reach_m:
            problems.append(f"{tag}: {east_m:.0f} m east, {north_m:.0f} m north of the centre - outside "
                            f"the {city.capture['area_km2']} km2 area")
    altitude, ground, clearance = (record["altitude_m"], record["ground_altitude_m"],
                                   record["height_above_ground_m"])
    if None not in (altitude, ground, clearance) and not near(altitude - ground, clearance, HEIGHT_SUM_TOLERANCE_M):
        problems.append(f"{tag}: altitude_m - ground_altitude_m = {altitude - ground:.2f} "
                        f"but height_above_ground_m = {clearance}")
    if ground is not None and not in_range(ground, GROUND_RANGE_M):
        problems.append(f"{tag}: ground_altitude_m {ground:.1f} is not ground anywhere on Earth "
                        f"- offset terrain or a bad probe")
    low, high = city.capture["height_m"]
    if clearance is not None and not users_view(record, city) and \
            not low - CLEARANCE_TOLERANCE_M <= clearance <= high + CLEARANCE_TOLERANCE_M:
        problems.append(f"{tag}: {clearance:.1f} m above the ground, outside height_m [{low}, {high}]")
    return problems


def _weather_problems(record: dict, tag: str, city: City) -> List[str]:
    """The settled state of the preset channel by channel, the run-wide particle size, and the spot's
    time inside its limits with the sun up or down to match."""
    problems = []
    preset = PRESETS.get(record["condition"])
    for channel in PRESET_CHANNELS if preset else ():
        tolerance = SNOW_COVER_TOLERANCE if channel == "material_snow_coverage" else CHANNEL_TOLERANCE
        if record[channel] is not None and not near(record[channel], preset[channel], tolerance):
            problems.append(f"{tag}: {channel} = {record[channel]} but preset {preset['asset']} "
                            f"settles at {preset[channel]}")
    wind_from = WIND_FROM_DEG.get(record["condition"])
    recorded = record["wind_from_deg"]
    if wind_from is not None and recorded is not None and not near(
            (recorded - wind_from + 180.0) % 360.0 - 180.0, 0.0, WIND_TOLERANCE_DEG):
        problems.append(f"{tag}: wind from {recorded} deg, the condition is wind from {wind_from:.0f} deg")
    for size, value in city.particles.items():
        if record[size] is not None and not near(record[size], value, PARTICLE_TOLERANCE):
            problems.append(f"{tag}: {size} {record[size]}, set to {value}")
    time = record["time_of_day"]
    if time is not None:
        low, high = (hhmm_minutes(limit) for limit in city.capture["time_of_day"])
        if not low - TIME_OF_DAY_TOLERANCE <= hhmm_minutes(round(time)) <= high + TIME_OF_DAY_TOLERANCE:
            problems.append(f"{tag}: time_of_day {time} is outside {city.capture['time_of_day']}")
        if not SUN["real"] and record["sun_elevation_deg"] is not None:
            problems += _sun_problems(record["sun_elevation_deg"], time, tag)
    return problems + _visibility_problems(record, tag)


def _visibility_problems(record: dict, tag: str) -> List[str]:
    """Clear weather sees CLEAR_VISIBILITY_KM; no weather sees further than clear."""
    seen, allowed = record["visibility_km"], VISIBILITY_TOLERANCE * CLEAR_VISIBILITY_KM
    if seen is None:
        return []
    if record["condition"] == "clear" and not near(seen, CLEAR_VISIBILITY_KM, allowed):
        return [f"{tag}: visibility {seen} km, set to {CLEAR_VISIBILITY_KM:g} km on a clear day"]
    if seen > CLEAR_VISIBILITY_KM + allowed:
        return [f"{tag}: visibility {seen} km is further than a clear day ({CLEAR_VISIBILITY_KM:g} km)"]
    return []


def _completeness_problems(city: City, seen: set) -> List[str]:
    problems = []
    for condition in city.weather:
        gaps = [waypoint for waypoint in range(city.waypoint_count()) if (condition, waypoint) not in seen]
        if gaps:
            more = " ..." if len(gaps) > 8 else ""
            problems.append(f"{condition}: no record for waypoint(s) {gaps[:8]}{more}")
    return problems


def validate_metadata_file(path: Path, city: City) -> List[str]:
    if not path.exists():
        return [f"{path} does not exist - no metadata was written"]
    try:
        records = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as error:
        return [f"{path.name} is not valid JSON: {error}"]
    return validate_metadata(records, city, image_dir=path.parent)


# --------------------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------------------


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parse_args(argv)
    if args.test:
        return _run_tests()
    cities = _select_cities(args)
    if cities is None:
        return 1
    _print_plan(cities)
    if args.imagecount:
        return 0
    return _capture(cities)


def _parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Capture weather images of the locations in LOCATION.")
    parser.add_argument("--location", "--city", dest="city", help="only the LOCATION entry with this name")
    parser.add_argument("--imagecount", action="store_true",
                        help="check the settings and show how many images a run makes and how long it takes")
    parser.add_argument("--test", action="store_true", help="run the tests (no Unreal needed)")
    return parser.parse_args(argv)


def _run_tests() -> int:
    """test_weather_pipeline.py, next to this file."""
    import unittest
    tests = unittest.defaultTestLoader.discover(str(Path(__file__).resolve().parent), pattern="test_weather_pipeline.py")
    return 0 if unittest.TextTestRunner(verbosity=2).run(tests).wasSuccessful() else 1


def _select_cities(args: argparse.Namespace) -> Optional[List[City]]:
    """The cities to run, or None (with the reason printed) if the settings are not usable."""
    try:
        cities = build_cities()
    except ConfigError as error:
        print(f"Invalid settings: {error}", file=sys.stderr)
        return None
    if args.city:
        cities = [city for city in cities if city.name.lower() == args.city.lower()]
        if not cities:
            names = ", ".join(entry["name"] for entry in LOCATION)
            print(f"No location named '{args.city}'. Locations: {names}", file=sys.stderr)
            return None
    problems = [f"{city.name}: {problem}" for city in cities for problem in city.validate()]
    if problems:
        print("Invalid settings:\n  - " + "\n  - ".join(problems), file=sys.stderr)
        return None
    return cities


def _print_plan(cities: List[City]) -> None:
    """Total images and estimated run time - before anything is run."""
    total_seconds = RUN_START_S + sum(city.estimated_seconds() for city in cities)
    print(f"Images: {sum(city.expected_images() for city in cities)}")
    print(f"ETA:    {format_duration(total_seconds)}")


def _capture(cities: List[City]) -> int:
    editor = find_editor_cmd()
    if editor is None:
        print("UnrealEditor-Cmd.exe not found; set UE_ENGINE_DIR to the engine folder.", file=sys.stderr)
        return 1
    if editor_is_open():
        print("The Unreal editor is open. Close it first: it keeps the level file locked, so the level "
              "cannot be moved to each city and nothing would be captured.", file=sys.stderr)
        return 1
    if session_is_running():
        print("Another headless Unreal (UnrealEditor-Cmd.exe) is running - an earlier capture, maybe stuck. "
              "Let it finish or end it in Task Manager first: two sessions would overwrite each other's "
              "settings and log.", file=sys.stderr)
        return 1
    started = time.time()
    failed = run_cities(cities, editor)
    print(f"\nDone in {format_duration(time.time() - started)}." + (" Some checks failed." if failed else ""))
    return failed


# --------------------------------------------------------------------------------------------------
# Inside Unreal: move the level to the city and save it
# --------------------------------------------------------------------------------------------------


def _set_level_location_inside_unreal() -> None:
    """
    Runs in the headless editor (pythonscript commandlet), started by set_level_location.

    Sets the CesiumGeoreference origin to the city - latitude, longitude and, when WP_HEIGHT is given,
    the calculated ground height - and the CLOUDS look on Ultra Dynamic Sky, and saves the level, so
    the editor and the capture show the same place, height and sky. Nothing else in the level changes.
    """
    import unreal  # only exists inside the editor

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level.load_level(os.environ["WP_LEVEL"]):
        return

    georeference = next((actor for actor in actors.get_all_level_actors()
                         if actor.get_class().get_name() == "CesiumGeoreference"), None)
    if georeference is None:
        return
    georeference.set_editor_property("origin_latitude", float(os.environ["WP_LAT"]))
    georeference.set_editor_property("origin_longitude", float(os.environ["WP_LON"]))
    if os.environ.get("WP_HEIGHT"):
        georeference.set_editor_property("origin_height", float(os.environ["WP_HEIGHT"]))
    sky = next((actor for actor in actors.get_all_level_actors()
                if actor.get_class().get_name().startswith("Ultra_Dynamic_Sky")), None)
    settings = json.loads(os.environ["WP_SKY"])
    for name, value in settings["clouds"].items() if sky else ():
        sky.set_editor_property(name, value if isinstance(value, bool) else float(value))
    if sky:
        sky.set_editor_property("Sky Light Intensity", settings["sky_light"])
        sky.set_editor_property("Enable Sun Lens Flare", settings["lens_flare"])
        # The fog plane and atmosphere surface sit at the sky actor; the plugin puts the ground at Z = 0.
        where = sky.get_actor_location()
        sky.set_actor_location(unreal.Vector(where.x, where.y, settings["anchor_m"] * CM_PER_M), False, False)
    if not level.save_current_level():
        return
    # A World Partition level keeps each actor in its own file, which saving the map does not write -
    # the sky's settings and position were lost that way. Save the changed actors' files explicitly.
    changed = [actor.get_outermost() for actor in (georeference, sky) if actor]
    if not unreal.EditorLoadingAndSavingUtils.save_packages(changed, False):
        return

    report = ["OK %s %s" % (os.environ["WP_LAT"], os.environ["WP_LON"])]
    report += [] if sky else ["NOSKY"]
    report += ["TILESET %s %.1f" % (actor.get_actor_label(), actor.get_actor_location().z)
               for actor in actors.get_all_level_actors() if actor.get_class().get_name() == "Cesium3DTileset"]
    Path(os.environ["WP_RESULT"]).write_text("\n".join(report), encoding="utf-8")


if os.environ.get("WP_INSIDE_UNREAL"):
    _set_level_location_inside_unreal()
elif __name__ == "__main__":
    raise SystemExit(main())
