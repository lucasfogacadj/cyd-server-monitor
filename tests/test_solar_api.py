import importlib.util
import io
import json
import pathlib
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "tft_dashboard_api", pathlib.Path(__file__).parents[1] / "api" / "tft_dashboard_api.py"
)
api = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(api)


class SolarApiTest(unittest.TestCase):
    def test_units_history_and_missing_optional_metric(self):
        queries = dict.fromkeys(api.SOLAR_QUERIES, "")
        queries.update({"p": "power", "d": "today", "t": "total", "l": "load"})
        readings = {"power": 2.35, "today": 0.006, "total": 1.2, "load": 0.9}
        with patch.object(api, "SOLAR_QUERIES", queries), \
             patch.object(api, "SOLAR_POWER_SCALE", 1000), \
             patch.object(api, "SOLAR_ENERGY_SCALE", 1000), \
             patch.object(api, "scalar", side_effect=lambda q: readings[q]), \
             patch.object(api, "solar_history", return_value=[0, 2350]):
            result = api.collect_solar()
        self.assertTrue(result["ok"])
        self.assertEqual(result["p"], 2350)
        self.assertEqual(result["d"], 6)
        self.assertEqual(result["t"], 1200)
        self.assertIsNone(result["g"])
        self.assertEqual(result["h"], [0, 2350])

    def test_unconfigured_power_is_not_reported_as_zero(self):
        with patch.object(api, "SOLAR_QUERIES", dict.fromkeys(api.SOLAR_QUERIES, "")):
            result = api.collect_solar()
        self.assertFalse(result["ok"])
        self.assertIsNone(result["p"])
        self.assertEqual(result["h"], [])

    def test_night_keeps_totals_without_inventing_power(self):
        queries = dict.fromkeys(api.SOLAR_QUERIES, "")
        queries.update({"d": "today", "daylight": "daylight", "live": "live"})
        with patch.object(api, "SOLAR_QUERIES", queries), \
             patch.object(api, "scalar", side_effect=lambda q: {"today": 38.1, "daylight": 0, "live": 0}[q]):
            result = api.collect_solar()
        self.assertTrue(result["ok"])
        self.assertIsNone(result["p"])
        self.assertEqual(result["daylight"], 0)
        self.assertEqual(result["d"], 38.1)

    def test_history_preserves_missing_time_slots_and_measured_zero(self):
        payload = {"status": "success", "data": {"result": [{"values": [
            [1800, "0"], [5400, "1234"], [7200, "NaN"], [86400, "+Inf"]
        ]}]}}
        with patch.object(api.time, "time", return_value=86400), \
             patch.object(api, "SOLAR_POWER_SCALE", 1), \
             patch.object(api.urllib.request, "urlopen", return_value=io.BytesIO(json.dumps(payload).encode())):
            history = api.solar_history("power")
        self.assertEqual(len(history), 49)
        self.assertEqual(history[:5], [None, 0, None, 1234, None])
        self.assertIsNone(history[-1])


if __name__ == "__main__":
    unittest.main()
