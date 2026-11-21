from pathlib import Path
import subprocess
import tempfile
import unittest


class WebConsoleTest(unittest.TestCase):
    def test_session_and_http_routes(self):
        source = Path(__file__).with_suffix(".cpp")
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "web_console"
            subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined", str(source),
                            "-o", str(binary)], check=True)
            subprocess.run([binary], check=True)
