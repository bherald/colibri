import os
import subprocess
import tempfile
import unittest
from pathlib import Path


class RouteSimulatorTest(unittest.TestCase):
    def run_sim(self, trace: Path, usage: Path) -> str:
        exe = Path("tools") / ("route_sim.exe" if os.name == "nt" else "route_sim")
        result = subprocess.run(
            [str(exe), "-c", "1", "--pin-usage", str(usage), "--pin-count", "1", str(trace)],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout

    @staticmethod
    def probes(output: str) -> int:
        total = next(line for line in output.splitlines() if line.lstrip().startswith("TOTAL"))
        return int(total.split()[1])

    def test_exact_global_pins_come_from_usage_not_evaluation_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = root / "route.trace"
            trace.write_text(
                "0 0 0 0:0.5 1:0.5\n"
                "1 0 0 0:0.5 1:0.5\n"
                "2 0 0 0:0.5 1:0.5\n",
                encoding="utf-8",
            )

            cold_usage = root / "cold.usage"
            cold_usage.write_text("-1 0 3\n0 2 100\n0 0 10\n", encoding="utf-8")
            cold = self.run_sim(trace, cold_usage)
            self.assertIn("exact_global_pins=1", cold)
            self.assertEqual(self.probes(cold), 6)

            hot_usage = root / "hot.usage"
            hot_usage.write_text("-1 0 3\n0 0 100\n0 2 10\n", encoding="utf-8")
            hot = self.run_sim(trace, hot_usage)
            self.assertEqual(self.probes(hot), 3)


if __name__ == "__main__":
    unittest.main()
