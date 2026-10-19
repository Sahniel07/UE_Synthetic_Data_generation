#!/usr/bin/env python3
"""
Tests for weather_pipeline.py. No Unreal needed.

    "C:\\Program Files\\Epic Games\\UE_5.5\\Engine\\Binaries\\ThirdParty\\Python3\\Win64\\python.exe" -m unittest -v test_weather_pipeline

Each group guards a way this pipeline has produced wrong data without failing:

  1. The config reaches the plugin: Unreal silently ignores ini keys it does not know, so every key
     written must be a Config UPROPERTY in MyActor_weather.h, and every setting must be used.
  2. The metadata schema is only measurements: the field list matches the C++ writer, and every
     value there goes through NumOrNull (null when unreadable, never a fallback).
  3. The metadata values are true: validate_metadata catches each failure seen in real runs, and
     TestLastRunMetadata runs it on the latest output on disk.
"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import math
import os
import re
import struct
import subprocess
import tempfile
import time
import unittest
import zlib
from pathlib import Path

import weather_pipeline as wp
from weather_pipeline import (
    LOCATION, METADATA_FIELDS, PRESETS, City, ConfigError, build_cities,
    estimate_remaining_seconds, format_ini_value, parse_site_result, png_size,
    validate_metadata, validate_metadata_file,
)

TOOLS = Path(__file__).resolve().parent
SOURCE = TOOLS.parent / "Plugins" / "WeatherPipeline" / "Source" / "WeatherPipeline"
HEADER = SOURCE / "Public" / "MyActor_weather.h"
ACTOR_CPP = SOURCE / "Private" / "MyActor_weather.cpp"
PRESET_DUMP = TOOLS.parent / "Saved" / "wp_presets.txt"

RETIRED_FIELDS = ("vertical_datum", "thunder_lightning", "material_wetness", "animate_time_of_day", "moving_fog_intensity", "weather_type", "step_index", "label",
                  "rain_drops_scale", "snow_flakes_scale", "snow_depth", "puddle_coverage",
                  "cloud_density", "cloud_direction_deg", "fog_density_scale")


@contextlib.contextmanager
def quiet():
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        yield


def config_properties(header: str) -> set:
    """Names of every UPROPERTY whose specifiers include Config."""
    names, lines, i = set(), header.splitlines(), 0
    while i < len(lines):
        line = lines[i]
        if "UPROPERTY(" not in line or line.lstrip().startswith("//"):
            i += 1
            continue
        spec = line[line.index("UPROPERTY(") + 10:]
        depth = 1 + spec.count("(") - spec.count(")")
        while depth > 0 and i + 1 < len(lines):
            i += 1
            spec += lines[i]
            depth += lines[i].count("(") - lines[i].count(")")
        i += 1
        while i < len(lines) and (not lines[i].strip() or lines[i].strip().startswith("//")):
            i += 1
        if i < len(lines) and re.search(r"\bConfig\b", spec):
            match = re.search(r"(\w+)\s*(=[^;]*)?;", lines[i])
            if match:
                names.add(match.group(1))
    return names


FRAME = {"type": "frame"}


def orbit(**parameters) -> dict:
    """An orbit shot, with any of its parameters changed."""
    return {"type": "orbit", "radius_m": 100.0, "pictures": 8, **parameters}


def spot_city(**capture) -> City:
    """The first location, automated, with its spot settings changed by `capture`."""
    city = copy.deepcopy(build_cities()[0])
    city.capture.update(capture)
    city.automate = True        # these tests are about the automated run, whatever AUTOMATE says
    return city


def every_shot():
    """One "shot" table per SHOT_TYPES entry, each parameter at its low end - a new shot included."""
    return [{"type": name, **{key: limits.low for key, limits in shot.parameters.items()}}
            for name, shot in wp.SHOT_TYPES.items()]


def both_shots():
    """One city per shot, so every branch of site_ini is covered."""
    return [spot_city(shot=shot) for shot in every_shot()]


# ==================================================================================================
# 1. The config reaches the plugin
# ==================================================================================================


class TestConfigReachesThePlugin(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.declared = config_properties(HEADER.read_text(encoding="utf-8"))

    def test_header_parser_works(self):
        for name in ("SpawnHeightAboveGroundM", "CaptureConditionFilter", "bFollowTerrain", "SunPitchDeg"):
            self.assertIn(name, self.declared)

    def test_every_key_written_is_declared(self):
        # Unreal ignores unknown keys silently - how WEATHER_TUNING spent weeks doing nothing.
        for city in list(build_cities()) + both_shots():
            for key in city.to_ini():
                self.assertIn(key, self.declared, f"{key} is not a Config UPROPERTY")

    def test_every_setting_is_used(self):
        source = Path(wp.__file__).read_text(encoding="utf-8")
        settings_part = source.split("#   Machinery", 1)[0]
        settings = re.findall(r"^([A-Z][A-Z0-9_]+)\s*=", settings_part, re.M)
        self.assertIn("LOCATION", settings)
        for name in settings:
            value = getattr(wp, name)
            if isinstance(value, list) and value and all(isinstance(item, str) and item in PRESETS
                                                         for item in value):
                continue      # a weather list the user keeps ready for LOCATION, used or not
            # Used by the machinery, or by another setting.
            uses = len(re.findall(rf"\b{name}\b", source)) - 1
            self.assertGreater(uses, 0, f"setting {name} is never used")

    def test_capture_table_matches_the_presets(self):
        # The C++ filter matches labels exactly: a label unknown to the table captures nothing.
        source = ACTOR_CPP.read_text(encoding="utf-8")
        table = dict((label, asset) for asset, label in re.findall(
            r'^\s*(?:Add|AddProjectPreset)\(TEXT\("(\w+)"\),\s*TEXT\("(\w+)"\)\)', source, re.M))
        self.assertEqual(table, {name: p["asset"] for name, p in PRESETS.items()})

    def test_tool_scripts_compile(self):
        # The Unreal-side code only runs inside the editor, so a syntax error there shows up only as a
        # failed run - once as a whole city photographed at the wrong place.
        for path in TOOLS.glob("*.py"):
            compile(path.read_text(encoding="utf-8-sig"), str(path), "exec")


class TestCleanCode(unittest.TestCase):
    """Standard-library stand-ins for a linter (no pip install): the rules the code is kept to."""

    MAX_FUNCTION_LINES = 60      # one job per function; longer ones get split

    @classmethod
    def setUpClass(cls):
        import ast
        cls.source = Path(wp.__file__).read_text(encoding="utf-8")
        cls.tree = ast.parse(cls.source)
        cls.ast = ast

    def test_no_unused_imports(self):
        imported = set()
        for node in self.ast.walk(self.tree):
            if isinstance(node, self.ast.Import):
                imported |= {alias.asname or alias.name.split(".")[0] for alias in node.names}
            elif isinstance(node, self.ast.ImportFrom) and node.module != "__future__":
                imported |= {alias.asname or alias.name for alias in node.names}
        used = {node.id for node in self.ast.walk(self.tree) if isinstance(node, self.ast.Name)}
        used |= {node.value.id for node in self.ast.walk(self.tree)
                 if isinstance(node, self.ast.Attribute) and isinstance(node.value, self.ast.Name)}
        self.assertEqual(imported - used, set())

    def test_functions_stay_short(self):
        for node in self.ast.walk(self.tree):
            if isinstance(node, (self.ast.FunctionDef, self.ast.AsyncFunctionDef)):
                length = node.end_lineno - node.lineno + 1
                self.assertLessEqual(length, self.MAX_FUNCTION_LINES, f"{node.name} is {length} lines")



class TestIni(unittest.TestCase):
    def test_value_formatting(self):
        self.assertEqual(format_ini_value(True), "True")
        self.assertEqual(format_ini_value(250.0), "250")
        self.assertEqual(format_ini_value(48.766512), "48.766512")
        self.assertNotIn("e", format_ini_value(1e-7))

    def test_spot_limits_reach_the_plugin(self):
        city = spot_city(spots=7, area_km2=2.5, height_m=[30.0, 90.0], pitch_deg=[-40.0, -10.0],
                         yaw_deg=[300.0, 60.0], roll_deg=[-5.0, 5.0], time_of_day=[930.0, 1645.0],
                         shot=orbit(radius_m=150.0, pictures=6), seed=None)
        values = city.site_ini()
        self.assertEqual((values["SpotCount"], values["SpotAreaKm2"], values["SpotSeed"]), (7, 2.5, -1))
        self.assertEqual((values["SpotHeightMinM"], values["SpotHeightMaxM"]), (30.0, 90.0))
        self.assertEqual((values["SpotPitchMinDeg"], values["SpotPitchMaxDeg"]), (-40.0, -10.0))
        self.assertEqual((values["SpotRollMinDeg"], values["SpotRollMaxDeg"]), (-5.0, 5.0))
        self.assertEqual((values["SpotTimeMin"], values["SpotTimeMax"]), (930.0, 1645.0))
        self.assertEqual((values["CaptureShot"], values["ShotParameters"]), ("orbit", "((radius_m,150),(pictures,6))"))
        self.assertEqual((values["SpotYawStartDeg"], values["SpotYawSpanDeg"]), (wp.unreal_yaw(300.0), 120.0))
        self.assertEqual(values["TimeOfDay"], 1308.0)              # the middle: (570 + 1005) / 2 = 787.5 -> 788 min
        self.assertEqual(values["SpawnHeightAboveGroundM"], 60.0)  # the drone waits mid-range
        self.assertFalse(values["bFlyBetweenWaypoints"])           # random jumps go straight there
        self.assertEqual(spot_city(seed=4).site_ini()["SpotSeed"], 4)

    def test_headings_are_compass_bearings(self):
        # Georeferenced world: +X east, +Y south. North must come out as yaw 270, east as 0.
        self.assertEqual(wp.unreal_yaw(0.0), 270.0)
        self.assertEqual(wp.unreal_yaw(90.0), 0.0)
        # A yaw range runs clockwise from its first bearing, across north if need be.
        for yaw_range, span in (([0.0, 360.0], 360.0), ([90.0, 180.0], 90.0), ([300.0, 60.0], 120.0),
                                ([45.0, 45.0], 0.0)):
            self.assertEqual(wp.yaw_span_deg(*yaw_range), span, yaw_range)
        self.assertTrue(wp.compass_in_range(10.0, [300.0, 60.0]))
        self.assertTrue(wp.compass_in_range(300.0, [300.0, 60.0]))
        self.assertFalse(wp.compass_in_range(180.0, [300.0, 60.0]))

    def test_nothing_about_the_place_has_to_be_typed_in(self):
        # Ground height is measured and calibrated by the plugin; the cloud base is relative to it.
        for entry in LOCATION:
            self.assertFalse({"ground_height_m", "cloud_base_m"} & set(entry), entry["name"])
        values = build_cities()[0].to_ini()
        self.assertTrue(values["bCalibrateOriginToGround"])
        self.assertEqual(values["CloudAltitudeMode"], "AboveTerrainAtStart")

    def test_the_editor_never_auto_starts(self):
        # The ini is shared with the editor: an auto-start there made Play fly the route unasked.
        # Headless runs get it from the command line instead.
        for city in build_cities():
            self.assertFalse(city.to_ini()["bAutoStartPipeline"])
        self.assertIn("-WeatherAutoStart", wp.launch_args(True, build_cities()[0].image))

    def test_sun_is_the_configured_sun(self):
        original = dict(wp.SUN)
        try:
            for real in (True, False):
                wp.SUN["real"] = real
                city = build_cities()[0]
                values = city.to_ini()
                # Exactly one of the two sun modes, never both.
                self.assertEqual((values["bUseGeoreferencedSun"], values["bApplySunSettings"]), (real, not real))
                self.assertEqual((values["SunYawDeg"], values["SunPitchDeg"], values["SunLightIntensityLux"]),
                                 (wp.SUN["yaw_deg"], wp.SUN["pitch_deg"], wp.SUN["intensity_lux"]))
            date = wp.sun_date()
            self.assertEqual((values["SunYear"], values["SunMonth"], values["SunDay"]), (date.year, date.month, date.day))
            self.assertEqual(values["SunNorthYawDeg"], wp.unreal_yaw(0.0))      # north in the georeferenced world
            self.assertEqual(city.site_ini()["TimeZoneHours"], city.lon / 15.0)  # the city's own sun time
        finally:
            wp.SUN.update(original)

    def test_bad_sun_settings_are_named(self):
        original = dict(wp.SUN)
        try:
            for key, value in (("date", "30.09.2026"), ("date", "2026-02-30"), ("real", "yes")):
                wp.SUN.update(original)
                wp.SUN[key] = value
                self.assertTrue(any(f"SUN {key}" in p for p in build_cities()[0].validate()), (key, value))
        finally:
            wp.SUN.update(original)

    def test_write_keeps_the_rest_of_the_file_and_replaces_the_block(self):
        with tempfile.TemporaryDirectory() as tmp:
            ini = Path(tmp) / "DefaultGame.ini"
            ini.write_text("[Other]\nKey=Value\n", encoding="utf-8")
            sydney = build_cities()[0]
            ingolstadt = copy.deepcopy(sydney)
            ingolstadt.name = "Ingolstadt"
            sydney.write_ini(ini)
            ingolstadt.write_ini(ini)
            text = ini.read_text(encoding="utf-8")
            self.assertIn("Key=Value", text)
            self.assertEqual(text.count(wp.BEGIN_MARKER), 1)
            self.assertIn("OutputSubfolder=Ingolstadt", text)

    def test_all_cities_go_to_one_session(self):
        # One CaptureSites entry per city, carrying its own place, clock and conditions in order.
        sydney = build_cities()[0]
        amsterdam = copy.deepcopy(sydney)
        amsterdam.name, amsterdam.lat, amsterdam.lon = "Amsterdam", 52.377956, 4.89707
        amsterdam.capture["time_of_day"], amsterdam.weather = [1700.0, 1800.0], ["clear", "partly_cloudy_fog"]
        block = sydney.ini_block(sites=[sydney, amsterdam])
        sites = [line for line in block.splitlines() if line.startswith("+CaptureSites=")]
        self.assertEqual(len(sites), 2)
        self.assertIn('Name="Amsterdam",Latitude=52.377956,Longitude=4.89707', sites[1])
        self.assertIn('TimeOfDay=1730,Conditions=("clear","partly_cloudy_fog"),Settings=(', sites[1])
        with tempfile.TemporaryDirectory() as tmp:
            amsterdam.capture["time_of_day"] = [2500.0, 2500.0]    # one bad location stops the whole session
            with self.assertRaises(ConfigError):
                sydney.write_ini(Path(tmp) / "DefaultGame.ini", sites=[sydney, amsterdam])

    def test_each_city_is_checked_against_its_own_part_of_the_log(self):
        log = ("startup\n[Site 1/2] Sydney at -33.868800, 151.209300, time 1200\nA\n"
               "[Site 1/2] Sydney at -33.868800, 151.209300, time 1200\nB\n"
               "[Site 2/2] Fly to the view you want and press P\n"
               "[Site 2/2] Amsterdam at 52.377956, 4.897070, time 1730\nC\n")
        self.assertEqual(wp.site_log(log, 0).count("\n"), 5)       # both of its lines, up to site 2
        self.assertTrue(wp.site_log(log, 1).endswith("C\n"))
        self.assertNotIn("Sydney", wp.site_log(log, 1))
        self.assertEqual(wp.site_log(log, 2), "")
        self.assertEqual(wp.site_log("no sites", 0), "no sites")   # older logs: the whole log

    def test_every_capture_point_is_measured_at_its_own_city(self):
        # A second city once reused the first city's capture points - and their 250 m clearance.
        frame = spot_city(spots=1, shot=FRAME)
        line = "[Terrain] Waypoint at (-200, 0): ground Z=81cm, flying 50.0 m above it (50.0 m asked).\n"
        self.assertEqual(wp.check_terrain_log(line, frame), [])
        self.assertTrue(wp.check_terrain_log("", frame))
        frame.automate = False          # manual: the first spot is the user's view, measured the same way
        self.assertTrue(wp.check_terrain_log("", frame))
        self.assertEqual(wp.check_terrain_log(line, frame), [])

    def test_the_window_has_the_images_aspect_ratio(self):
        for width, height, expected in ((1920, 1080, ("1600", "900")), (1024, 1024, ("900", "900")),
                                        (1080, 1920, ("506", "900"))):
            args = wp.window_args({"width": width, "height": height}, 1600, 900)
            self.assertEqual((args[1].split("=")[1], args[2].split("=")[1]), expected, (width, height))

    def test_invalid_city_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            ini = Path(tmp) / "DefaultGame.ini"
            city = build_cities()[0]
            city.weather = ["sunny"]
            with self.assertRaises(ConfigError):
                city.write_ini(ini)
            self.assertFalse(ini.exists())


class TestSettings(unittest.TestCase):
    def test_shipped_settings_are_valid(self):
        for city in build_cities():
            self.assertEqual(city.validate(), [], city.name)

    def test_run_size(self):
        # One frame through every condition: 15 categories, 8 wind directions, fog + clouds, day after snow.
        frame = spot_city(spots=1, shot=FRAME)
        frame.weather = list(wp.WEATHER_ALL)
        self.assertEqual(frame.expected_images(), 25)

    def test_output_dir_decides_where_images_go(self):
        # The plugin writes to OutputRootDir/WeatherSnapshots/<city>; the checks read the same place.
        city = build_cities()[0]
        self.assertEqual(city.to_ini()["OutputRootDir"], wp.OUTPUT_ROOT.as_posix())
        self.assertEqual(city.snapshot_dir(), wp.OUTPUT_ROOT / "WeatherSnapshots" / city.name)
        self.assertTrue(wp.OUTPUT_ROOT.is_absolute())

    def test_locations_are_places_and_capture_their_spots(self):
        # Each entry holds all of its own settings: where, what weather, and its spots.
        for entry, city in zip(LOCATION, build_cities()):
            self.assertEqual(set(wp.LOCATION_KEYS + wp.SPOT_KEYS), set(entry), entry["name"])
            self.assertEqual(city.capture, {key: entry[key] for key in wp.SPOT_KEYS}, entry["name"])
            self.assertIsNot(city.capture["shot"], entry["shot"])       # its own copy
            self.assertEqual(city.to_ini()["TimeOfDay"], city.start_time())


class TestModularSettings(unittest.TestCase):
    """Locations and their spots can be added, removed, changed and reordered independently."""

    def setUp(self):
        self.saved = copy.deepcopy(wp.LOCATION)
        wp.LOCATION[:] = [dict(copy.deepcopy(LOCATION[0]), name=name) for name in ("A", "B", "C")]
        self.spots = wp.LOCATION[0]["spots"]

    def tearDown(self):
        wp.LOCATION[:] = self.saved

    def assert_named(self, fragment):
        with self.assertRaises(ConfigError) as caught:
            build_cities()
        self.assertIn(fragment, str(caught.exception))

    def test_an_entry_changes_its_own_spots_only(self):
        wp.LOCATION[0]["spots"] = 3
        wp.LOCATION[1].update(shot=orbit(radius_m=80.0, pictures=5), time_of_day=[1600.0, 1700.0])
        a, b, c = build_cities()
        self.assertEqual((a.capture["spots"], b.capture["spots"], c.capture["spots"]), (3, self.spots, self.spots))
        self.assertEqual([city.capture["shot"]["type"] for city in (a, b, c)], ["frame", "orbit", "frame"])
        self.assertEqual(b.capture["time_of_day"], [1600.0, 1700.0])
        # Each location's spots go into its own CaptureSites entry, and every key there is declared.
        sites = [line for line in a.ini_block(sites=[a, b, c]).splitlines() if line.startswith("+CaptureSites=")]
        self.assertIn('"SpotCount=3"', sites[0])
        self.assertIn('"CaptureShot=orbit"', sites[1])
        self.assertIn('"ShotParameters=((radius_m,80),(pictures,5))"', sites[1])
        self.assertIn('"ShotParameters=()"', sites[0])
        self.assertIn("TimeOfDay=1630", sites[1])
        declared = config_properties(HEADER.read_text(encoding="utf-8"))
        for city in (a, b, c):
            self.assertEqual(set(city.site_ini()) - declared, set(), city.name)
        wp.LOCATION[:] = [wp.LOCATION[2], wp.LOCATION[0]]       # reordered: each keeps its own
        self.assertEqual([city.capture["spots"] for city in build_cities()], [self.spots, 3])

    def test_unknown_and_missing_keys_are_named(self):
        wp.LOCATION[1]["zoom"] = 2
        self.assert_named("LOCATION entry B has unknown key(s) zoom")
        del wp.LOCATION[1]["zoom"], wp.LOCATION[1]["seed"]
        self.assert_named("LOCATION entry B is missing seed")

    def test_a_shot_takes_exactly_its_own_parameters(self):
        self.assertEqual(build_cities()[0].site_ini()["ShotParameters"], "()")          # a frame takes none
        wp.LOCATION[0]["shot"] = {"type": "orbit"}
        self.assert_named("LOCATION entry A shot is missing radius_m, pictures")
        wp.LOCATION[0]["shot"] = {"type": "frame", "radius_m": 150.0}
        self.assert_named("LOCATION entry A shot has unknown key(s) radius_m")
        wp.LOCATION[0]["shot"] = orbit(radius_m="150")
        self.assert_named("LOCATION entry A shot radius_m '150' must be a number")
        wp.LOCATION[0]["shot"] = orbit(radius_m=150.0, pictures=6)
        values = build_cities()[0].site_ini()
        self.assertEqual((values["CaptureShot"], values["ShotParameters"]), ("orbit", "((radius_m,150),(pictures,6))"))

    def test_a_shot_is_named_by_its_type(self):
        for shot, fragment in (("frame", "C shot 'frame' must be a {\"type\": ...} table"),
                               ({"radius_m": 1}, "C shot {'radius_m': 1} must be a"),
                               ({"type": "spiral"}, "type 'spiral' must be one of frame, orbit")):
            wp.LOCATION[2]["shot"] = shot
            self.assert_named(fragment)

    def test_every_shot_type_has_a_plugin_class(self):
        # A SHOT_TYPES entry is used through its class U<Name>CaptureShot (UCaptureShot::Find).
        source = "\n".join(path.read_text(encoding="utf-8") for path in SOURCE.rglob("*.h"))
        classes = {name.lower() for name in re.findall(r"class \w+_API U(\w+)CaptureShot\s*:\s*public U\w*CaptureShot", source)}
        self.assertEqual(set(wp.SHOT_TYPES), classes)

    def test_each_location_has_its_own_image_and_particles(self):
        wp.LOCATION[1]["image"].update(width=1280, height=720, fov_deg=90.0, fisheye=True)
        wp.LOCATION[2]["particles"]["rain_drop_scale"] = 5.0
        a, b, c = build_cities()
        self.assertEqual((a.site_ini()["CaptureResolutionX"], b.site_ini()["CaptureResolutionX"]),
                         (wp.LOCATION[0]["image"]["width"], 1280))
        self.assertEqual((b.site_ini()["CaptureFOV"], b.site_ini()["bFisheyeLens"]), (90.0, True))
        self.assertEqual(c.site_ini()["RainDropScale"], 5.0)
        self.assertEqual(a.site_ini()["RainDropScale"], wp.LOCATION[0]["particles"]["rain_drop_scale"])

    def test_a_missing_image_setting_is_named(self):
        del wp.LOCATION[0]["image"]["quality"]
        self.assert_named("LOCATION entry A image is missing quality")
        wp.LOCATION[0]["image"] = "1920x1080"
        self.assert_named("LOCATION entry A image must be a {...} table")
        del wp.LOCATION[0]["image"], wp.LOCATION[1]["particles"]
        self.assert_named("LOCATION entry A is missing image")

    def test_no_locations_is_named(self):
        wp.LOCATION.clear()
        self.assert_named("LOCATION is empty")

    def test_a_location_missing_a_key_is_named(self):
        del wp.LOCATION[1]["lon"]
        self.assert_named("LOCATION entry B is missing lon")

    def test_ranges_must_be_two_numbers(self):
        for value in (-15.0, [-15.0], ["-15", 0.0], [-15.0, 0.0, 5.0]):
            wp.LOCATION[0]["pitch_deg"] = value
            self.assert_named("pitch_deg")

    def test_imagecount_works_for_any_selection(self):
        wp.LOCATION[1]["spots"] = 2
        for selection in (["A"], ["B"], ["A", "B", "C"]):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                args = ["--imagecount"] + (["--location", selection[0]] if len(selection) == 1 else [])
                self.assertEqual(wp.main(args), 0, selection)
            wanted = sum(c.expected_images() for c in build_cities() if c.name in selection)
            self.assertIn(f"Images: {wanted}\n", out.getvalue())

    def test_time_of_day_minutes(self):
        city = spot_city(time_of_day=[1200.0, 1275.0])
        self.assertTrue(any("HHMM" in p for p in city.validate()))
        city.capture["time_of_day"] = [1200.0, 1259.0]
        self.assertFalse(any("time_of_day" in p for p in city.validate()))

    def test_spot_counts_and_start_time(self):
        circling = spot_city(spots=5, shot=orbit(pictures=6))
        self.assertEqual(circling.waypoint_count(), 30)
        self.assertEqual((circling.spot_of(0), circling.spot_of(13)), ((0, 0), (2, 1)))
        self.assertEqual(spot_city(spots=5, shot=FRAME).waypoint_count(), 5)
        self.assertEqual(spot_city(time_of_day=[900.0, 1700.0]).start_time(), 1300.0)
        self.assertEqual(spot_city(time_of_day=[1000.0, 1050.0]).start_time(), 1025.0)

    def test_the_estimate_uses_the_plugins_own_waits(self):
        # The per-condition and per-picture waits are the plugin's defaults; the estimate must follow them.
        header = HEADER.read_text(encoding="utf-8")

        def default(name):
            return float(re.search(rf"float {name} = ([\d.]+)f;", header).group(1))
        self.assertEqual(wp.CONDITION_START_S,
                         default("ConditionTransitionSeconds") + default("ConditionSettleSeconds"))
        self.assertGreaterEqual(wp.TELEPORT_CAPTURE_S, default("WaypointSettleSeconds"))

    def test_smooth_shading_conditions_match_the_plugin(self):
        source = ACTOR_CPP.read_text(encoding="utf-8")
        marked = set(re.findall(r'TEXT\("(\w+)"\)\)[^;\n]*bRequiresSmoothNormals\s*=\s*true', source))
        self.assertEqual(marked, {name for name in PRESETS if wp.needs_smooth_shading(name)})
        self.assertTrue(wp.needs_smooth_shading("day_after_snow"))     # snow cover, though no snowfall
        city = spot_city(spots=1)
        city.weather = ["clear", "snow_light", "snow_heavy", "fog_light", "snow_wind_n"]
        self.assertEqual(city.shading_switches(), 3)             # on, off, on

    def test_imagecount_counts_and_estimates_without_running(self):
        city = spot_city(spots=3, shot=orbit(pictures=4))
        city.weather = ["clear", "cloudy"]
        per_condition = wp.CONDITION_START_S + 12 * wp.TELEPORT_CAPTURE_S      # 3 spots x 4 pictures
        first_load = wp.COLD_TILES_S * 12                                       # each spot's tiles once
        self.assertAlmostEqual(city.estimated_seconds(), wp.CITY_SETUP_S + 2 * per_condition + first_load)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(wp.main(["--imagecount"]), 0)
        self.assertIn(f"Images: {sum(c.expected_images() for c in build_cities())}\nETA:", out.getvalue())

    def test_bad_settings_are_named(self):
        city = spot_city()
        for field_name, value, fragment in (("weather", ["sunny"], "not a preset"),
                                            ("lon", 250.0, "lat/lon"),
                                            ("cloud_above_ground_m", 300.0, "above the clouds")):
            broken = copy.deepcopy(city)
            setattr(broken, field_name, value)
            self.assertTrue(any(fragment in p for p in broken.validate()), field_name)
        for key, value in (("spots", 0), ("spots", 2.5), ("spots", 10001), ("area_km2", 30.0),
                           ("height_m", [2.0, 50.0]), ("height_m", [90.0, 40.0]), ("pitch_deg", [-95.0, 0.0]),
                           ("yaw_deg", [0.0, 400.0]), ("roll_deg", [-50.0, 0.0]), ("time_of_day", [2500.0, 2500.0])):
            broken = spot_city(**{key: value})
            self.assertTrue(any(key in p for p in broken.validate()), (key, value))
        for key, value in (("radius_m", 5.0), ("radius_m", 2001.0), ("pictures", 1), ("pictures", 361)):
            broken = spot_city(shot=orbit(**{key: value}))
            self.assertIn(f"shot orbit {key} {value} must be", "".join(broken.validate()), (key, value))

    def test_a_yaw_range_may_cross_north(self):
        self.assertEqual(spot_city(yaw_deg=[300.0, 60.0]).validate(), [])

    def test_seed(self):
        for good in (None, 0, 12345):
            wp.LOCATION[1]["seed"] = good
            self.assertEqual(len(build_cities()), 3, good)
        for bad in (-1, 1.5, "1", True):
            wp.LOCATION[1]["seed"] = bad
            self.assert_named("seed")

    def test_image_and_sun_ranges_are_enforced(self):
        for key, value in (("width", 20), ("fov_deg", 180.0), ("exposure_bias", 20), ("fisheye", "yes")):
            city = build_cities()[0]
            city.image[key] = value
            self.assertTrue(any(f"image {key}" in p for p in city.validate()), key)
        city = build_cities()[0]
        city.particles["snow_flake_scale"] = 0.0
        self.assertTrue(any("particles snow_flake_scale" in p for p in city.validate()))
        for key, value in (("pitch_deg", 95.0), ("intensity_lux", -1.0)):
            original = wp.SUN[key]
            wp.SUN[key] = value
            try:
                self.assertTrue(any(key in p for p in build_cities()[0].validate()), key)
            finally:
                wp.SUN[key] = original

    def test_cloud_look_is_checked(self):
        # Every setting maps to a sky property; out of range, unknown or not-a-bool values are named.
        self.assertEqual(set(wp.CLOUDS), set(wp.CLOUD_PROPERTIES) | set(wp.CLOUD_SWITCHES))
        self.assertEqual(wp.cloud_problems(), [])
        original = dict(wp.CLOUDS)
        try:
            for key, value in (("size", 9.0), ("patchiness", -0.1), ("second_layer", 1), ("density", True),
                               ("wisps", "off")):
                wp.CLOUDS.update(original)
                wp.CLOUDS[key] = value
                self.assertTrue(any(key in p for p in wp.cloud_problems()), key)
            wp.CLOUDS.update(original)
            wp.CLOUDS["cloud_shape"] = 1.0
            self.assertTrue(any("cloud_shape" in p for p in build_cities()[0].validate()))
        finally:
            wp.CLOUDS.clear()
            wp.CLOUDS.update(original)

    def test_duplicate_location_names_rejected(self):
        wp.LOCATION[:] = [wp.LOCATION[0], dict(wp.LOCATION[0])]
        with self.assertRaises(ConfigError):
            build_cities()


class TestCommandLine(unittest.TestCase):
    def test_imagecount_changes_nothing(self):
        before = wp.CONFIG_INI.read_text(encoding="utf-8") if wp.CONFIG_INI.exists() else None
        with quiet():
            self.assertEqual(wp.main(["--imagecount"]), 0)
        self.assertEqual(before, wp.CONFIG_INI.read_text(encoding="utf-8") if wp.CONFIG_INI.exists() else None)

    def test_city_filter(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(wp.main(["--imagecount", "--location", LOCATION[-1]["name"].lower()]), 0)
        self.assertIn(f"Images: {build_cities()[-1].expected_images()}\n", out.getvalue())
        with quiet():
            self.assertEqual(wp.main(["--imagecount", "--city", "Atlantis"]), 1)

    def test_render_quality_reaches_the_plugin_and_is_checked(self):
        city = build_cities()[0]
        values = city.to_ini()
        self.assertEqual((values["RenderQuality"], values["CesiumMaximumScreenSpaceError"]),
                         (city.image["quality"], float(city.image["tile_error"])))
        for key, value in (("quality", 5), ("quality", 2.5), ("tile_error", 1.0), ("tile_error", 40.0)):
            city = build_cities()[0]
            city.image[key] = value
            self.assertTrue(any(key in p for p in city.validate()), (key, value))

    def test_open_editor_stops_the_run_before_anything_happens(self):
        original_open, original_find = wp.editor_is_open, wp.find_editor_cmd
        wp.editor_is_open, wp.find_editor_cmd = (lambda: True), (lambda: Path("UnrealEditor-Cmd.exe"))
        before = wp.CONFIG_INI.read_text(encoding="utf-8") if wp.CONFIG_INI.exists() else None
        try:
            with quiet():
                self.assertEqual(wp.main([]), 1)
        finally:
            wp.editor_is_open, wp.find_editor_cmd = original_open, original_find
        self.assertEqual(before, wp.CONFIG_INI.read_text(encoding="utf-8") if wp.CONFIG_INI.exists() else None)

    def test_failed_level_move_stops_the_capture(self):
        # A failed move once let a whole "Sydney" run photograph Ingolstadt.
        city = build_cities()[0]
        original_set, original_write = wp.set_level_location, City.write_ini
        wp.set_level_location = lambda *args: False
        City.write_ini = lambda self, path=wp.CONFIG_INI, sites=(): None
        try:
            with quiet():
                self.assertEqual(wp.run_cities([city], Path("unused")), 1)
        finally:
            wp.set_level_location, City.write_ini = original_set, original_write


class TestManualFrame(unittest.TestCase):
    """AUTOMATE = False: the user flies to the first view and presses P; the rest is drawn round it."""

    def manual_city(self):
        city = build_cities()[0]
        city.automate = False
        return city

    def test_ini_hands_the_first_spot_to_the_user(self):
        values = self.manual_city().to_ini()
        self.assertTrue(values["bManualFrame"])
        self.assertTrue(values["bUseFlightPath"])             # the spots, from the user's view
        self.assertTrue(values["bFollowTerrain"])
        self.assertTrue(values["bSpawnAtHeightAboveGround"])   # starts above the buildings, then free
        self.assertFalse(values["bLitmusOnPlay"])              # no weather changes before P
        self.assertFalse(values["bAutoStartPipeline"])

    def test_automated_ini_is_unchanged(self):
        values = build_cities()[0].to_ini()
        if build_cities()[0].automate:
            self.assertFalse(values["bManualFrame"])
            self.assertTrue(values["bUseFlightPath"])

    def test_every_spot_is_taken(self):
        city = self.manual_city()
        city.capture.update(spots=4, shot=orbit(pictures=3))
        self.assertEqual(city.expected_images(), len(city.weather) * 12)

    def test_manual_window_is_not_automatic(self):
        args = wp.launch_args(False, build_cities()[0].image)
        self.assertNotIn("-WeatherAutoStart", args)
        self.assertNotIn("-unattended", args)

    def test_the_users_view_is_recorded_not_judged(self):
        # Spot 1 is wherever the user pressed P; the area is centred there, and later spots keep
        # their limits.
        city = fixture_city()
        city.automate = False
        records = fixture_records(fixture_city())
        for record in records:
            if record["waypoint_index"] == 0:
                record.update(height_above_ground_m=300.0, altitude_m=round(record["ground_altitude_m"] + 300.0, 2),
                              camera_rotation=[-60.0, 12.0, 20.0], latitude_deg=50.0, longitude_deg=10.0)
            else:   # the rest round the user's start rather than the location's lat/lon
                record.update(latitude_deg=50.0 + (record["latitude_deg"] - city.lat), longitude_deg=10.0)
        self.assertEqual(validate_metadata(records, city), [])
        records[1]["altitude_m"] += 5.0                         # ... but the measured values must add up
        self.assertTrue(any("altitude_m - ground_altitude_m" in p for p in validate_metadata(records, city)))
        low = [r for r in copy.deepcopy(records) if r["waypoint_index"] == 2]
        for record in low:
            record.update(height_above_ground_m=10.0, altitude_m=round(record["ground_altitude_m"] + 10.0, 2))
        self.assertTrue(any("outside height_m" in p for p in validate_metadata(low, city)))


class TestGroundHeight(unittest.TestCase):
    """The calculated ground height is read from the run log and remembered per city."""

    LOG = ("[Site] Ground height calculated from the tileset: 41.37 m above the ellipsoid (19.05 m above "
           "sea level, lowest of 5 samples); georeference Origin Height 421.00 -> 41.37 m, so Z = 0 is the ground.")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.saved_file, self.saved_set = wp.GROUND_HEIGHTS, wp.set_level_location
        self.saved_log = wp.RUN_LOG
        wp.GROUND_HEIGHTS = Path(self.tmp.name) / "ground.json"
        wp.RUN_LOG = Path(self.tmp.name) / "run.log"
        self.level_saves = []
        wp.set_level_location = lambda city, editor, height=None: self.level_saves.append(height) or True

    def tearDown(self):
        wp.GROUND_HEIGHTS, wp.set_level_location, wp.RUN_LOG = self.saved_file, self.saved_set, self.saved_log
        self.tmp.cleanup()

    def test_parse(self):
        self.assertEqual(wp.parse_calculated_ground_height(self.LOG), 41.37)
        self.assertIsNone(wp.parse_calculated_ground_height("no such line"))

    def test_new_height_is_remembered_and_saved_into_the_level_once(self):
        city = build_cities()[0]
        wp.RUN_LOG.write_text(self.LOG, encoding="utf-8")
        with quiet():
            wp.save_calculated_ground_height(city, Path("unused"))
            wp.save_calculated_ground_height(city, Path("unused"))   # unchanged: no second save
        self.assertEqual(self.level_saves, [41.37])
        self.assertEqual(wp.known_ground_height(city), 41.37)

    def test_height_is_forgotten_when_the_coordinates_change(self):
        city = build_cities()[0]
        wp.RUN_LOG.write_text(self.LOG, encoding="utf-8")
        with quiet():
            wp.save_calculated_ground_height(city, Path("unused"))
        city.lat += 1.0
        self.assertIsNone(wp.known_ground_height(city))


class TestBugRegressions(unittest.TestCase):
    """One test per bug found in review - each failed before its fix."""

    def test_time_of_day_minutes_match_the_plugin(self):
        city = spot_city(time_of_day=[1200.0, 1259.5])          # the plugin rejects minutes above 59
        self.assertTrue(any("HHMM" in p for p in city.validate()))

    def test_counts_python_and_plugin_would_disagree_on_are_named(self):
        for key, value in (("spots", 2.5), ("spots", 0)):
            city = spot_city(shot=orbit(), **{key: value})
            self.assertTrue(any(key in p for p in city.validate()), (key, value))
        for value in (1, 2.5):
            city = spot_city(shot=orbit(pictures=value))
            self.assertTrue(any("pictures" in p for p in city.validate()), value)

    def test_wrong_types_are_named_not_crashed_on(self):
        city = spot_city()
        city.lat, city.name = "48.7", 7
        problems = city.validate()
        for fragment in ("lat/lon", "folder name"):
            self.assertTrue(any(fragment in p for p in problems), fragment)
        original = list(wp.LOCATION)
        try:
            wp.LOCATION[:] = [dict(original[0], weather="clear")]
            with self.assertRaises(ConfigError) as caught:
                build_cities()
            self.assertIn("weather must be a list", str(caught.exception))
        finally:
            wp.LOCATION[:] = original

    def test_a_city_the_session_never_reached_is_not_passed_on_old_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            saved = (wp.OUTPUT_ROOT, wp.RUN_LOG, wp.save_calculated_ground_height)
            wp.OUTPUT_ROOT, wp.RUN_LOG = Path(tmp), Path(tmp) / "run.log"
            wp.save_calculated_ground_height = lambda *args, **kwargs: None
            try:
                city = fixture_city()
                out = city.snapshot_dir()
                out.mkdir(parents=True)
                (out / "weather_metadata.json").write_text(json.dumps(fixture_records(city)), encoding="utf-8")
                for record in fixture_records(city):
                    write_png(out / record["image_file"], 64, 36)
                errors = io.StringIO()
                with contextlib.redirect_stderr(errors), contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(wp.check_session([city], Path("unused"), time.time() + 60), 1)
                self.assertIn("was not captured in this run", errors.getvalue())
            finally:
                wp.OUTPUT_ROOT, wp.RUN_LOG, wp.save_calculated_ground_height = saved

    def test_refused_tiles_stop_the_run_without_printing_the_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "run.log"
            log.write_text("LogCesium: Error: [error] Errors when lo", encoding="utf-8")
            watch = wp.LogWatch(log)
            self.assertIsNone(watch.tileset_failure())        # the message is split across two reads
            with open(log, "a", encoding="utf-8") as handle:
                handle.write("ading tileset:\n- Received status code 429 for tile content "
                             "https://tile.googleapis.com/v1/3dtiles/root.json?key=SECRETKEY\n")
            failure = watch.tileset_failure()
            self.assertIn("429", failure)
            self.assertIn("quota", failure)
            self.assertNotIn("SECRETKEY", failure)
            log.write_text("fresh log\n", encoding="utf-8")   # a new session starts the log afresh
            self.assertIsNone(watch.tileset_failure())

    def test_the_open_air_warning_is_not_counted_as_a_measured_point(self):
        city = spot_city(spots=1)
        warning = "[Terrain] No ground found under waypoint at (0, 0); its ground fields are recorded as null.\n"
        self.assertTrue(wp.check_terrain_log(warning, city))


class TestHelpers(unittest.TestCase):
    def test_a_picture_deleted_while_counting_is_skipped(self):
        # The plugin empties a city's folder as it starts there, between glob() and stat().
        from unittest import mock
        with tempfile.TemporaryDirectory() as tmp:
            kept = Path(tmp) / "clear_p00.png"
            kept.write_bytes(b"x")
            gone = Path(tmp) / "rain_wind_e_p17.png"
            with mock.patch.object(Path, "glob", return_value=[kept, gone]):
                self.assertEqual(wp.images_written_since(Path(tmp), 0.0), 1)

    def test_eta_ignores_startup(self):
        self.assertIsNone(estimate_remaining_seconds(1, 10, 50.0, 100.0))
        # first image at t=90 after boot, then 5 s each -> 7 left = 35 s
        self.assertAlmostEqual(estimate_remaining_seconds(3, 10, 90.0, 100.0), 35.0)

    def test_site_result(self):
        self.assertEqual(parse_site_result("OK 1 2 3\nTILESET Google Tiles 0.0"), (True, []))
        self.assertEqual(parse_site_result("OK 1 2 3\nTILESET Google Tiles -152500.0"),
                         (True, [("Google Tiles", -152500.0)]))
        self.assertEqual(parse_site_result(""), (False, []))


class TestSceneGeometry(unittest.TestCase):
    """Fog, atmosphere and clouds relative to the ground - checked from the plugin's [Scene] line."""

    # Real line format. GOOD is the tuned look (sky placed for CLEAR_VISIBILITY_KM, cloud base
    # CLOUD_BASE_ABOVE_GROUND_M above the ground); SKY_LEFT_BEHIND is the real failure where the terrain
    # moved and the sky did not, which lost the horizon haze.
    ANCHOR = round(wp.sky_anchor_above_ground_m())
    CLOUD_KM = round((wp.CLOUD_BASE_ABOVE_GROUND_M - ANCHOR) / 1000.0, 3)
    GOOD = ("[Scene] 'clear' first capture, heights above the ground: drone +236 m, clearance +250 m, "
            "world Z=0 +2000 m | "
            f"height fog plane {ANCHOR - 1:+d} m, density 0.0046, falloff 0.065, start 83 m, visible yes | "
            f"atmosphere surface {ANCHOR:+d} m (mode 1, radius 6360 km, aerial perspective x0.00), cloud "
            f"layer bottom {CLOUD_KM:.3f} km, height 5.000 km above that surface")
    SKY_LEFT_BEHIND = GOOD.replace(f"plane {ANCHOR - 1:+d} m", "plane -2050 m").replace(
        f"surface {ANCHOR:+d} m", "surface -2048 m")

    @staticmethod
    def city():
        """An automated location whose first spot was at the 250 m the GOOD line was recorded at."""
        return spot_city(height_m=[250.0, 250.0])

    def test_parse(self):
        line = wp.parse_scene_lines(self.GOOD)[0]
        self.assertEqual((line["drone"], line["clearance"], line["fog"], line["surface"], line["cloud_km"]),
                         (236, 250, self.ANCHOR - 1, self.ANCHOR, self.CLOUD_KM))
        self.assertEqual(line["cloud_height_km"], 5.0)

    def test_visibility_matches_unreal_fog(self):
        # The plane placed for the setting gives that visibility, with the clear fog's own values.
        self.assertAlmostEqual(wp.visibility_km(wp.CLEAR_FOG_DENSITY, wp.CLEAR_FOG_FALLOFF,
                                                wp.sky_anchor_above_ground_m()), wp.CLEAR_VISIBILITY_KM)
        # By hand: density 0.0046 at the ground is 0.00046 per m in base 2; 5% contrast is
        # log2(20) = 4.32 bits, so 4.32 / 0.00046 = 9.4 km. Higher plane = more haze.
        # Times the 0.1 calibration to what the pictures look like.
        self.assertAlmostEqual(wp.visibility_km(0.0046, 0.065, 0.0), 0.9395, places=3)
        self.assertLess(wp.visibility_km(0.0046, 0.065, 10.0), wp.visibility_km(0.0046, 0.065, -100.0))

    def test_clear_visibility_is_checked(self):
        hazy = self.GOOD.replace(f"plane {self.ANCHOR - 1:+d} m", "plane +10 m").replace(
            f"surface {self.ANCHOR:+d} m", "surface +11 m")
        found = wp.check_scene_log(hazy, self.city())
        self.assertTrue(any("visibility 0.9 km" in p for p in found), found)

    def test_each_location_has_its_own_time(self):
        saved = [dict(entry) for entry in wp.LOCATION]
        try:
            for index, entry in enumerate(wp.LOCATION):
                entry["time_of_day"] = [600.0 + 100.0 * index] * 2
            self.assertEqual([city.to_ini()["TimeOfDay"] for city in build_cities()],
                             [600.0 + 100.0 * index for index in range(len(wp.LOCATION))])
        finally:
            wp.LOCATION[:] = saved

    def test_sun_check_follows_the_day_night_cycle(self):
        saved = dict(wp.SUN)
        try:
            wp.SUN.update(real=False, sunrise=557.0, sunset=1742.0)
            self.assertEqual(wp._sun_problems(+25.0, 1200.0, "t"), [])
            self.assertTrue(wp._sun_problems(-3.0, 1200.0, "t"))                 # night at noon
            self.assertTrue(wp._sun_problems(+5.0, 2200.0, "t"))                 # day at night
            self.assertEqual(wp._sun_problems(-1.0, 1745.0, "t"), [])            # at the edge: either
            self.assertEqual(wp._sun_problems(+1.0, 1745.0, "t"), [])
            line = self.GOOD + " | sun -4.0 deg, from compass 200 deg at time 1300"
            parsed = wp.parse_scene_lines(line)[0]
            self.assertEqual((parsed["sun_deg"], parsed["sun_time"]), (-4.0, 1300.0))
            city = self.city()
            self.assertTrue(any("should be up" in p for p in wp.check_scene_log(line, city)))
            wp.SUN["real"] = True                                                # the real sun: not this check
            self.assertFalse(any("should be up" in p for p in wp.check_scene_log(line, city)))
        finally:
            wp.SUN.clear(); wp.SUN.update(saved)

    def test_lens_flare_switch(self):
        saved = dict(wp.SUN)
        try:
            for on in (True, False):
                wp.SUN["lens_flare"] = on
                self.assertIs(build_cities()[0].to_ini()["bSunLensFlare"], on)       # the capture
                self.assertIs(wp.sky_level_settings()["lens_flare"], on)            # the saved level
            wp.SUN["lens_flare"] = "off"
            self.assertTrue(any("SUN lens_flare must be True or False" in p for p in build_cities()[0].validate()))
        finally:
            wp.SUN.clear(); wp.SUN.update(saved)

    def test_sun_size(self):
        saved = dict(wp.SUN)
        try:
            wp.SUN["size"] = 2.5
            self.assertAlmostEqual(build_cities()[0].to_ini()["SunScale"], 2.5 * wp.SUN_SCALE_FOR_REAL_SIZE)
            for bad in (0.0, 25.0, "big"):
                wp.SUN["size"] = bad
                self.assertTrue(any("SUN size" in p for p in build_cities()[0].validate()), bad)
        finally:
            wp.SUN.clear(); wp.SUN.update(saved)

    def test_sunrise_and_sunset_are_checked(self):
        saved = dict(wp.SUN)
        try:
            values = build_cities()[0].to_ini()
            self.assertEqual((values["SunriseTime"], values["SunsetTime"]), (wp.SUN["sunrise"], wp.SUN["sunset"]))
            for key, value, fragment in (("sunrise", 2460.0, "SUN sunrise"), ("sunset", 1775.0, "SUN sunset"),
                                         ("sunrise", 1800.0, "before sunset"), ("sunset", "1742", "SUN sunset")):
                wp.SUN.clear(); wp.SUN.update(saved, **{key: value})
                self.assertTrue(any(fragment in p for p in build_cities()[0].validate()), (key, value))
        finally:
            wp.SUN.clear(); wp.SUN.update(saved)

    def test_unknown_city_key_is_named(self):
        saved = [dict(entry) for entry in wp.LOCATION]
        try:
            wp.LOCATION[0]["utc_offset"] = 2.0                  # the removed setting, left in a city line
            self.assertTrue(any("unknown key(s) utc_offset" in p for p in wp.setting_shape_problems()))
        finally:
            wp.LOCATION[:] = saved

    def test_level_save_gets_the_sky_settings_from_the_caller(self):
        # The headless editor re-reads the file from disk; what it writes must come from this process.
        saved = (wp.CLEAR_VISIBILITY_KM, dict(wp.CLOUDS))
        try:
            wp.CLEAR_VISIBILITY_KM, wp.CLOUDS["size"] = 40.0, 2.5
            sent = json.loads(json.dumps(wp.sky_level_settings()))
            self.assertAlmostEqual(sent["anchor_m"], wp.sky_anchor_above_ground_m())
            self.assertEqual(sent["clouds"]["Volumetric Clouds Scale"], 2.5)
            self.assertEqual(set(sent["clouds"]), {name for name, _ in wp.CLOUD_PROPERTIES.values()})
        finally:
            wp.CLEAR_VISIBILITY_KM = saved[0]
            wp.CLOUDS.clear(); wp.CLOUDS.update(saved[1])

    def test_wisps_switch(self):
        saved = dict(wp.CLOUDS)
        try:
            wp.CLOUDS.update(wisps=True, wisps_clear=0.3, wisps_cloudy=0.6)
            on = wp.sky_level_settings()["clouds"]
            self.assertEqual((on["Cloud Wisps Opacity (Clear)"], on["Cloud Wisps Opacity (Cloudy)"]), (0.3, 0.6))
            wp.CLOUDS["wisps"] = False
            off = wp.sky_level_settings()["clouds"]
            self.assertEqual((off["Cloud Wisps Opacity (Clear)"], off["Cloud Wisps Opacity (Cloudy)"]), (0.0, 0.0))
            self.assertEqual(off["Volumetric Clouds Scale"], on["Volumetric Clouds Scale"])   # nothing else changes
        finally:
            wp.CLOUDS.clear(); wp.CLOUDS.update(saved)

    def test_visibility_out_of_range_is_refused(self):
        saved = wp.CLEAR_VISIBILITY_KM
        try:
            for bad in (0.1, 1001.0):
                wp.CLEAR_VISIBILITY_KM = bad
                self.assertTrue(any("CLEAR_VISIBILITY_KM" in p for p in build_cities()[0].validate()), bad)
            wp.CLEAR_VISIBILITY_KM = "20"                       # text: named before anything reads it
            self.assertIn("CLEAR_VISIBILITY_KM must be a number", wp.setting_shape_problems())
        finally:
            wp.CLEAR_VISIBILITY_KM = saved

    def test_tuned_look_passes_in_any_city(self):
        for city in build_cities():
            city.capture["height_m"], city.automate = [250.0, 250.0], True
            self.assertEqual(wp.check_scene_log(self.GOOD, city), [], city.name)

    def test_sky_left_under_the_terrain_is_caught(self):
        found = wp.check_scene_log(self.SKY_LEFT_BEHIND, self.city())
        self.assertTrue(any("height fog plane -2050 m" in p for p in found))
        self.assertTrue(any("atmosphere surface -2048 m" in p for p in found))

    def test_sloping_terrain_is_not_a_failure(self):
        # Drone 236 m above the SPAWN ground but 250 m above the ground below it: terrain 14 m lower at
        # waypoint 0, as in Sydney. The clearance is what the height is about.
        self.assertEqual(wp.check_scene_log(self.GOOD, self.city()), [])

    def test_roof_biased_cloud_base_is_caught(self):
        # The single-trace cloud reference once landed on a 230 m tower: base 1622 m instead of 1375 m.
        roof = f"{self.CLOUD_KM + 0.247:.3f} km"                        # 247 m too high
        found = wp.check_scene_log(self.GOOD.replace(f"{self.CLOUD_KM:.3f} km", roof), self.city())
        self.assertTrue(any(f"cloud base {wp.CLOUD_BASE_ABOVE_GROUND_M + 247:.0f} m" in p for p in found), found)

    def test_wrong_cloud_base_and_missing_log(self):
        self.assertTrue(any("cloud base" in p for p in
                            wp.check_scene_log(self.GOOD.replace(f"{self.CLOUD_KM:.3f} km", "3.423 km"), self.city())))
        self.assertTrue(wp.check_scene_log("", self.city()))


# ==================================================================================================
# 2. The metadata schema is only measurements
# ==================================================================================================


class TestMetadataSchema(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ACTOR_CPP.read_text(encoding="utf-8")
        start = source.index("EVERY FIELD BELOW IS READ BACK FROM THE ENGINE")
        cls.writer = source[start:source.index("const FString NewEntry = E;", start)]
        cls.fields = tuple(re.findall(r'TEXT\("\s*\\"(\w+)\\":', cls.writer))

    def test_cpp_writes_exactly_these_fields(self):
        self.assertEqual(self.fields, METADATA_FIELDS)
        for name in RETIRED_FIELDS:
            self.assertNotIn(name, self.fields)

    def test_values_are_read_back_or_null(self):
        numeric = [f for f in METADATA_FIELDS if f not in (
            "image_file", "condition", "waypoint_index", "camera_location", "camera_rotation",
            "image_width", "image_height", "lens")]
        for name in numeric:
            line = next(l for l in self.writer.splitlines() if f'\\"{name}\\"' in l)
            self.assertIn("NumOrNull", line, name)
        self.assertNotIn("ActiveCaptureStep", self.writer)
        self.assertIn("GetLocalWeatherState", self.writer)


@unittest.skipUnless(PRESET_DUMP.exists(), "Saved/wp_presets.txt not present")
class TestPresetsAreOneToOne(unittest.TestCase):
    """PRESETS must be the assets' own values - the validator checks live weather against it."""

    NAMES = {"cloud_coverage": "Cloud Coverage", "rain": "Rain", "snow": "Snow", "fog": "Fog",
             "wind_intensity": "Wind Intensity",
             "material_snow_coverage": "Material Snow Coverage"}

    def test_every_channel_matches_the_asset(self):
        dump = {}
        for line in PRESET_DUMP.read_text(encoding="utf-8").splitlines():
            if line.strip():
                dump[line.split()[0]] = {k.strip(): float(v) for k, v in
                                         re.findall(r"\s{2}([A-Za-z /]+?)=([-\d.]+)", line)}
        for name, preset in PRESETS.items():
            self.assertIn(preset["asset"], dump, f"{preset['asset']} missing from the dump")
            for channel, key in self.NAMES.items():
                self.assertAlmostEqual(preset[channel], dump[preset["asset"]][key], places=2,
                                       msg=f"{name}.{channel}")


# ==================================================================================================
# 3. The metadata values are true
# ==================================================================================================


def write_png(path: Path, width: int, height: int) -> None:
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\x00" + b"\x80" * width for _ in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def fixture_city() -> City:
    return City(name="Ingolstadt", lat=48.7665, lon=11.4258, automate=True,
                weather=["clear", "rain_medium", "snow_fog", "rain_wind_e"],
                capture={"spots": 3, "area_km2": 1.0, "height_m": [40.0, 120.0],
                         "pitch_deg": [-30.0, -5.0], "yaw_deg": [300.0, 60.0], "roll_deg": [-5.0, 5.0],
                         "time_of_day": [900.0, 1700.0], "shot": {"type": "frame"}, "seed": 1},
                image={"width": 64, "height": 36, "fov_deg": 75.0, "fisheye": False, "auto_exposure": True,
                       "exposure_bias": 0, "quality": 3, "tile_error": 12.0},
                particles={"rain_drop_scale": 3.0, "snow_flake_scale": 2.0})


FIXTURE_COMPASS = (330.0, 0.0, 30.0)       # each spot's bearing, inside the fixture's [300, 60]


def fixture_records(city: City) -> list:
    """What a correct run writes: three spots inside the fixture's limits, each in every weather."""
    records = []
    for cond in city.weather:
        for w in range(city.waypoint_count()):
            ground, height = 374.0 + 3.0 * w, 50.0 + 20.0 * w
            r = {"image_file": f"{cond}_p{w:02d}.png", "condition": cond, "waypoint_index": w,
                 "camera_location": [0.0, 1000.0 * w, 25000.0 + 300.0 * w],
                 "camera_rotation": [-10.0 - 5.0 * w, wp.unreal_yaw(FIXTURE_COMPASS[w]), 2.0 * w - 2.0],
                 "camera_fov_deg": city.image["fov_deg"], "image_width": 64, "image_height": 36,
                 "latitude_deg": city.lat + 0.001 * w, "longitude_deg": city.lon,      # ~110 m apart
                 "altitude_m": round(ground + height, 2), "ground_altitude_m": ground,
                 "height_above_ground_m": height,
                 "wind_from_deg": wp.WIND_FROM_DEG.get(cond, 41.5), "time_of_day": 1000.0 + 100.0 * w,
                 "sun_elevation_deg": 30.0, "lens": "fisheye" if city.image["fisheye"] else "rectilinear",
                 "visibility_km": wp.CLEAR_VISIBILITY_KM * (1.0 if cond == "clear" else 0.4),
                 **city.particles}
            r.update({k: v for k, v in PRESETS[cond].items() if k != "asset"})
            records.append({k: r[k] for k in METADATA_FIELDS})
    return records


class TestMetadataValidator(unittest.TestCase):
    """Each mutation is a failure seen in a real run; the validator must name it."""

    def setUp(self):
        self.city = fixture_city()
        self.records = fixture_records(self.city)
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        for r in self.records:
            write_png(self.dir / r["image_file"], 64, 36)

    def tearDown(self):
        self.tmp.cleanup()

    def mutate(self, match, **changes):
        records = copy.deepcopy(self.records)
        for r in records:
            if all(r[k] == v for k, v in match.items()):
                r.update(changes)
        return records

    def caught(self, records, fragment):
        found = validate_metadata(records, self.city, self.dir)
        self.assertTrue(any(fragment in p for p in found), f"'{fragment}' not in {found[:4]}")

    def test_a_correct_run_passes(self):
        self.assertEqual(validate_metadata(self.records, self.city, self.dir), [])
        self.assertEqual(png_size(self.dir / "clear_p00.png"), (64, 36))

    def test_weather_not_the_preset(self):
        self.caught(self.mutate({"condition": "rain_medium"}, rain=0.0), "rain = 0.0")          # above the clouds
        self.caught(self.mutate({"condition": "snow_fog"}, fog=5.0), "fog = 5.0")         # fog not applied
        self.caught(self.mutate({"condition": "snow_fog"}, material_snow_coverage=1.0), "material_snow")  # full cover: only a blizzard

    def test_wind_from_the_wrong_direction(self):
        self.caught(self.mutate({"condition": "rain_wind_e"}, wind_from_deg=270.0), "wind from 270.0 deg")
        # 359.6 is 0.4 deg from north, not 359.6 - the check wraps around the compass.
        records = self.mutate({"condition": "rain_wind_e"}, wind_from_deg=90.4)
        self.assertEqual(validate_metadata(records, self.city, self.dir), [])

    def test_particle_size_not_the_setting(self):
        # Wrong by 1 whatever the particles are set to, so the test never depends on the settings.
        self.caught(self.mutate({"condition": "rain_medium"}, rain_drop_scale=self.city.particles["rain_drop_scale"] + 1),
                    "rain_drop_scale")
        self.caught(self.mutate({"condition": "snow_fog"}, snow_flake_scale=self.city.particles["snow_flake_scale"] + 1),
                    "snow_flake_scale")

    def test_visibility(self):
        self.caught(self.mutate({"condition": "clear"}, visibility_km=wp.CLEAR_VISIBILITY_KM * 0.45),
                    "on a clear day")
        self.caught(self.mutate({"condition": "rain_medium"}, visibility_km=wp.CLEAR_VISIBILITY_KM * 2.0),
                    "further than a clear day")
        records = self.mutate({"condition": "clear"}, visibility_km=wp.CLEAR_VISIBILITY_KM * 1.05)
        self.assertFalse(any("visibility" in p for p in validate_metadata(records, self.city, self.dir)))

    def test_unmeasured_value(self):
        self.caught(self.mutate({"condition": "clear"}, wind_from_deg=None), "not measured")

    def test_offset_terrain(self):
        records = copy.deepcopy(self.records)
        for r in records:
            r["ground_altitude_m"] -= 1525.0
            r["altitude_m"] = round(r["altitude_m"] - 1525.0, 2)
        self.caught(records, "not ground anywhere")

    def test_height_outside_its_limits(self):
        for height in (20.0, 300.0):        # below the range; raised past it over a tower
            self.caught(self.mutate({"waypoint_index": 2}, ground_altitude_m=480.0, height_above_ground_m=height,
                                    altitude_m=480.0 + height), "outside height_m [40.0, 120.0]")

    def test_inconsistent_heights(self):
        self.caught(self.mutate({"waypoint_index": 1}, altitude_m=999.0), "altitude_m - ground_altitude_m")

    def test_wrong_city(self):
        self.caught(self.mutate({}, latitude_deg=-33.8688, longitude_deg=151.2093), "outside the 1.0 km2 area")
        self.caught(self.mutate({"waypoint_index": 2}, latitude_deg=self.city.lat + 0.006), "outside the 1.0 km2 area")

    def test_camera(self):
        self.caught(self.mutate({"condition": "snow_fog", "waypoint_index": 1},
                                camera_location=[0.0, 1000.0, 26000.0]), "matched pair")
        self.caught(self.mutate({"condition": "clear"}, camera_fov_deg=90.0), "camera_fov_deg")
        self.caught(self.mutate({"waypoint_index": 0}, camera_rotation=[-50.0, wp.unreal_yaw(330.0), -2.0]),
                    "camera pitch -50.0 is outside pitch_deg")
        self.caught(self.mutate({"waypoint_index": 0}, camera_rotation=[-10.0, wp.unreal_yaw(330.0), 9.0]),
                    "camera roll 9.0 is outside roll_deg")
        self.caught(self.mutate({"waypoint_index": 0}, camera_rotation=[-10.0, wp.unreal_yaw(180.0), -2.0]),
                    "faces compass 180.0")

    def test_orbit_pictures_face_their_centre_not_the_yaw_range(self):
        city = fixture_city()
        city.capture.update(spots=1, shot=orbit(pictures=3))
        records = [r for r in fixture_records(city)]
        for record in records:          # one spot: pictures 1 and 2 face the circle's centre
            record["camera_rotation"][1] = wp.unreal_yaw((330.0 + 120.0 * record["waypoint_index"]) % 360.0)
        self.assertFalse(any("faces compass" in p for p in validate_metadata(records, city)))

    def test_image_files(self):
        write_png(self.dir / "rain_medium_p01.png", 32, 18)
        (self.dir / "clear_p02.png").unlink()
        found = validate_metadata(self.records, self.city, self.dir)
        self.assertTrue(any("PNG is 32x18" in p for p in found))
        self.assertTrue(any("missing or not a PNG" in p for p in found))

    def test_schema_and_completeness(self):
        self.caught(self.mutate({}, animate_time_of_day=False), "undeclared fields")
        self.caught([r for r in self.records if (r["condition"], r["waypoint_index"]) != ("rain_medium", 1)],
                    "no record for waypoint(s) [1]")
        self.caught(self.records + [copy.deepcopy(self.records[0])], "recorded twice")
        self.caught(self.mutate({"condition": "clear", "waypoint_index": 0}, condition="fog_heavy",
                                image_file="fog_heavy_p00.png"), "was not requested")
        self.caught(self.mutate({"condition": "rain_medium"}, time_of_day=1830.0), "outside [900.0, 1700.0]")

    def test_the_sun_follows_the_spots_time(self):
        self.caught(self.mutate({"waypoint_index": 1}, sun_elevation_deg=-5.0), "should be up")

    def test_file_errors(self):
        self.assertTrue(validate_metadata_file(self.dir / "absent.json", self.city))
        bad = self.dir / "weather_metadata.json"
        bad.write_text("[{", encoding="utf-8")
        self.assertIn("not valid JSON", validate_metadata_file(bad, self.city)[0])


class TestLastRunMetadata(unittest.TestCase):
    """The latest output on disk, judged against the current settings - when it was made with them.
    Output from before a settings change is skipped: it was checked when its own run finished."""

    def test_latest_output(self):
        checked = 0
        for city in build_cities():
            path = city.snapshot_dir() / "weather_metadata.json"
            if not path.exists():
                continue
            records = json.loads(path.read_text(encoding="utf-8") or "[]")
            if not records or tuple(records[0]) != METADATA_FIELDS:
                continue       # written by an older plugin build
            same_plan = ({r["condition"] for r in records} == set(city.weather) and
                         len({r["waypoint_index"] for r in records}) == city.waypoint_count())
            if not same_plan:
                continue       # made with other conditions or another path
            with self.subTest(city=city.name):
                self.assertEqual(validate_metadata_file(path, city), [])
            checked += 1
        if not checked:
            self.skipTest("no output on disk made with the current settings")



class TestAnyInputIsNamedNotCrashed(unittest.TestCase):
    """Every user setting, given every kind of bad value, ends in "Invalid settings" - never a traceback."""

    BAD = (None, "x", [], True, math.nan, math.inf, -1e12, 1e12)
    SETTINGS = ("WEATHER_ALL", "LOCATION", "AUTOMATE",
                "OUTPUT_DIR", "SUN", "CLOUD_BASE_ABOVE_GROUND_M", "CLOUDS", "CLEAR_VISIBILITY_KM",
                "WIND_FROM_DEG", "WIND_TOLERANCE_DEG")

    def setUp(self):
        self.original = {name: copy.deepcopy(getattr(wp, name)) for name in self.SETTINGS}

    def tearDown(self):
        for name, value in self.original.items():
            setattr(wp, name, copy.deepcopy(value))

    def mutations(self):
        """(where, apply(value)): each setting, each of its keys, each key of its entries - and each
        key removed."""
        for name in self.SETTINGS:
            yield name, lambda v, n=name: setattr(wp, n, v)
            value = self.original[name]
            entries = [(f"{name}[{k!r}]", value[k]) for k in value] if isinstance(value, dict) else \
                      [(f"{name}[0]", value[0])] if isinstance(value, list) and value else []
            for where, entry in entries:
                yield where, lambda v, w=where: self.set(w, v)
                for key in entry if isinstance(entry, dict) else ():
                    yield f"{where}[{key!r}]", lambda v, w=f"{where}[{key!r}]": self.set(w, v)
                    yield f"{where} without {key!r}", lambda v, w=where, k=key: self.get(w).pop(k)
                    for sub in entry[key] if isinstance(entry[key], dict) else ():   # e.g. a location's image
                        inner = f"{where}[{key!r}]"
                        yield f"{inner}[{sub!r}]", lambda v, w=f"{inner}[{sub!r}]": self.set(w, v)
                        yield f"{inner} without {sub!r}", lambda v, w=inner, k=sub: self.get(w).pop(k)

    def get(self, where):
        return eval(where, {}, vars(wp))               # e.g. "LOCATION[0]" in the module's namespace

    def set(self, where, value):
        container, key = where.rsplit("[", 1)
        self.get(container)[eval(key[:-1])] = value

    def test_every_bad_value_is_named(self):
        tried = 0
        for where, apply in self.mutations():
            for bad in self.BAD:
                self.tearDown()
                try:
                    apply(bad)
                except (KeyError, IndexError):
                    continue
                tried += 1
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    try:
                        code = wp.main(["--imagecount"])
                    except Exception as error:        # noqa: BLE001 - any crash is the failure
                        self.fail(f"{where} = {bad!r} crashed: {type(error).__name__}: {error}")
                self.assertIn(code, (0, 1), f"{where} = {bad!r}")
        self.assertGreater(tried, 500)


class TestCesiumIonToken(unittest.TestCase):
    """The capture's tiles need a Cesium ion token. It comes from the pipeline - never only from what the
    editor happened to save - is checked before Unreal starts, and is never printed."""

    TOKEN = "eyJhbGciOiJIUzI1NiJ9.eyJqdGkiOiJ0ZXN0In0.c2lnbmF0dXJl"

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.saved = (wp.ION_TOKEN_FILE, wp.SAVED_ION_SERVER, os.environ.pop(wp.ION_TOKEN_ENV, None),
                      wp.urllib.request.urlopen)
        wp.ION_TOKEN_FILE = self.dir / wp.ION_TOKEN_FILE_NAME
        wp.SAVED_ION_SERVER = self.dir / "CesiumIonSaaS.uasset"

    def tearDown(self):
        wp.ION_TOKEN_FILE, wp.SAVED_ION_SERVER, env, wp.urllib.request.urlopen = self.saved
        if env is not None:
            os.environ[wp.ION_TOKEN_ENV] = env
        else:
            os.environ.pop(wp.ION_TOKEN_ENV, None)

    def answer(self, status):
        """Cesium ion answers every request with `status` (an exception for a lost connection)."""
        def urlopen(url, timeout):
            self.asked = url
            if isinstance(status, Exception):
                raise status
            if status != 200:
                raise wp.urllib.error.HTTPError(url, status, "", {}, io.BytesIO(url.encode()))
            return contextlib.nullcontext()
        wp.urllib.request.urlopen = urlopen

    def test_the_token_comes_from_env_then_file_then_the_saved_project(self):
        self.assertEqual(wp.find_ion_token(), (None, ""))
        wp.SAVED_ION_SERVER.write_bytes(b"\x00binary" + self.TOKEN.encode() + b"\x00more")
        self.assertEqual(wp.find_ion_token()[0], self.TOKEN)
        wp.ION_TOKEN_FILE.write_text("   \n", encoding="utf-8")            # empty: not a token
        self.assertEqual(wp.find_ion_token()[0], self.TOKEN)
        wp.ION_TOKEN_FILE.write_text("\ufefffile-token\n", encoding="utf-8")  # Notepad's BOM and newline
        self.assertEqual(wp.find_ion_token(), ("file-token", wp.ION_TOKEN_FILE_NAME))
        os.environ[wp.ION_TOKEN_ENV] = " env-token "
        self.assertEqual(wp.find_ion_token()[0], "env-token")

    def test_each_failure_is_named_without_the_token(self):
        self.answer(200)
        self.assertIsNone(wp.ion_token_problem(self.TOKEN, "test"))
        self.assertIn(f"/assets/{wp.GOOGLE_TILES_ION_ASSET}/endpoint", self.asked)
        for status, fragment in ((401, "not a valid token"), (404, "My Assets"), (500, "HTTP 500")):
            self.answer(status)
            problem = wp.ion_token_problem(self.TOKEN, "test")
            self.assertIn(fragment, problem)
            self.assertNotIn(self.TOKEN, problem)
        self.assertIn(wp.ION_TOKEN_FILE_NAME, wp.ion_token_problem(None, ""))

    def test_no_connection_warns_and_tries_anyway(self):
        self.answer(wp.urllib.error.URLError("offline"))
        with quiet():
            self.assertIsNone(wp.ion_token_problem(self.TOKEN, "test"))

    def test_the_capture_session_gets_the_token(self):
        self.assertNotIn(wp.UNREAL_ION_TOKEN_ENV, wp.unreal_env())
        wp.ION_TOKEN_FILE.write_text(self.TOKEN, encoding="utf-8")
        self.assertEqual(wp.unreal_env()[wp.UNREAL_ION_TOKEN_ENV], self.TOKEN)
        module = (SOURCE / "Private" / "WeatherPipelineModule.cpp").read_text(encoding="utf-8")
        self.assertIn(f'TEXT("{wp.UNREAL_ION_TOKEN_ENV}")', module)     # the name the plugin reads
        source = Path(wp.__file__).read_text(encoding="utf-8")
        self.assertIn("env=unreal_env()", source)                       # the capture's Popen passes it

    def test_a_bad_token_stops_the_run_before_unreal_starts(self):
        self.answer(401)
        wp.ION_TOKEN_FILE.write_text("revoked", encoding="utf-8")
        patched = {"find_editor_cmd": lambda: Path("UnrealEditor-Cmd.exe"), "editor_is_open": lambda: False,
                   "session_is_running": lambda: False, "run_cities": lambda *args: self.fail("Unreal started")}
        originals = {name: getattr(wp, name) for name in patched}
        for name, value in patched.items():
            setattr(wp, name, value)
        err = io.StringIO()
        try:
            with contextlib.redirect_stderr(err):
                self.assertEqual(wp._capture(build_cities()), 1)
        finally:
            for name, value in originals.items():
                setattr(wp, name, value)
        self.assertIn("Not capturing: Cesium ion rejected the access token from cesium_ion_token.txt", err.getvalue())

    def test_the_token_file_is_never_committed(self):
        root = TOOLS.parent
        try:
            result = subprocess.run(["git", "check-ignore", "-q", wp.ION_TOKEN_FILE_NAME], cwd=root)
        except FileNotFoundError:
            self.skipTest("git not installed")
        self.assertEqual(result.returncode, 0, f"{wp.ION_TOKEN_FILE_NAME} would be committed")

    def test_tile_errors_name_the_cesium_token_not_a_google_key(self):
        reasons = " ".join(wp.TILE_STATUS_REASONS.values())
        self.assertNotIn("Google API key", reasons)
        self.assertIn(wp.ION_TOKEN_FILE_NAME, wp.TILE_STATUS_REASONS["401"])



class TestAnyUltraDynamicSkyWorks(unittest.TestCase):
    """A user may place their own Ultra Dynamic Sky and Weather, or add them next to the shipped ones: the
    level save keeps one of each and writes the tuned look onto it, so the pictures do not change."""

    def level_save_reports(self, report):
        """Runs set_level_location with the headless editor replaced by one that reports `report`."""
        original = wp.subprocess.run
        wp.subprocess.run = lambda *args, **kwargs: wp.SITE_RESULT.write_text(report, encoding="utf-8")
        out, err = io.StringIO(), io.StringIO()
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                saved = wp.set_level_location(build_cities()[0], Path("unused"))
        finally:
            wp.subprocess.run = original
            wp.SITE_RESULT.unlink(missing_ok=True)
        return saved, out.getvalue() + err.getvalue()

    def test_the_look_gives_the_clear_fog_the_visibility_is_computed_for(self):
        look = wp.SKY_LOOK
        self.assertAlmostEqual(look["Base Fog Density"] * look["Scale Fog Density"], wp.CLEAR_FOG_DENSITY, places=4)
        settings = wp.sky_level_settings()
        self.assertEqual((settings["sky_look"], settings["weather_look"]), (wp.SKY_LOOK, wp.WEATHER_LOOK))

    def test_extra_skies_are_removed_and_named(self):
        saved, said = self.level_save_reports("OK 1 2\nREMOVED Ultra_Dynamic_Sky 'Ultra_Dynamic_Sky2'\n"
                                              "REMOVED Ultra_Dynamic_Weather 'Ultra_Dynamic_Weather2'")
        self.assertTrue(saved)
        self.assertIn("removed the extra Ultra_Dynamic_Sky 'Ultra_Dynamic_Sky2'", said)
        self.assertIn("removed the extra Ultra_Dynamic_Weather 'Ultra_Dynamic_Weather2'", said)

    def test_a_missing_sky_or_weather_stops_with_what_to_do(self):
        saved, said = self.level_save_reports("MISSING Ultra_Dynamic_Weather")
        self.assertFalse(saved)
        self.assertIn("the level has no Ultra_Dynamic_Weather", said)
        self.assertIn("Content/UltraDynamicSky/Blueprints", said)

    def test_one_of_each_says_nothing_extra(self):
        saved, said = self.level_save_reports("OK 1 2\nTILESET Google 0.0")
        self.assertTrue(saved)
        self.assertNotIn("removed", said)



class TestUltraDynamicSkyIsInPlace(unittest.TestCase):
    """UDS is not in the repo. From Fab or copied in by hand, it must be whole at Content/UltraDynamicSky -
    a capture without it, or with it elsewhere, says so before Unreal starts."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.saved = (wp.PROJECT_ROOT, wp.CONTENT_DIR, wp.UDS_DIR, wp.UDS_PRESETS_DIR, wp.PROJECT_PRESETS_DIR)
        wp.PROJECT_ROOT, wp.CONTENT_DIR = self.root, self.root / "Content"
        self.place(wp.CONTENT_DIR / "UltraDynamicSky")
        wp.PROJECT_PRESETS_DIR = wp.CONTENT_DIR / "WeatherPipeline" / "Presets"

    def tearDown(self):
        (wp.PROJECT_ROOT, wp.CONTENT_DIR, wp.UDS_DIR, wp.UDS_PRESETS_DIR, wp.PROJECT_PRESETS_DIR) = self.saved

    def place(self, uds_dir):
        wp.UDS_DIR = uds_dir
        wp.UDS_PRESETS_DIR = uds_dir / "Blueprints" / "Weather_Effects" / "Weather_Presets"

    def install(self, uds_dir, skip_preset=None):
        """A UDS copy at uds_dir: its blueprints and every preset the conditions use."""
        for part in wp.UDS_BLUEPRINTS:
            (uds_dir / part).parent.mkdir(parents=True, exist_ok=True)
            (uds_dir / part).write_bytes(b"uasset")
        presets = uds_dir / "Blueprints" / "Weather_Effects" / "Weather_Presets"
        presets.mkdir(parents=True, exist_ok=True)
        for asset in {preset["asset"] for preset in PRESETS.values()} - {skip_preset}:
            (presets / f"{asset}.uasset").write_bytes(b"uasset")

    def test_a_whole_copy_in_place_is_enough(self):
        self.install(wp.UDS_DIR)
        self.assertIsNone(wp.uds_problem())

    def test_no_uds_says_both_ways_to_get_it(self):
        problem = wp.uds_problem()
        self.assertIn("not in this project", problem)
        self.assertIn("Add to Project", problem)
        self.assertIn("copy the whole UltraDynamicSky folder", problem)

    def test_a_copy_in_the_wrong_folder_is_found_and_named(self):
        self.install(wp.CONTENT_DIR / "Imported" / "UltraDynamicSky")
        self.assertIn("is at Content/Imported/UltraDynamicSky, but it must be exactly Content/UltraDynamicSky",
                      wp.uds_problem())

    def test_a_partial_copy_names_what_is_missing(self):
        self.install(wp.UDS_DIR, skip_preset="Clear_Skies")
        self.assertIn("preset(s) Clear_Skies not found", wp.uds_problem())



class TestThisProjectsUltraDynamicSky(unittest.TestCase):
    def test_installed_here_means_ready(self):
        if not wp.UDS_DIR.is_dir():
            self.skipTest("Ultra Dynamic Sky not installed in this checkout")
        self.assertIsNone(wp.uds_problem())


if __name__ == "__main__":
    unittest.main(verbosity=2)
