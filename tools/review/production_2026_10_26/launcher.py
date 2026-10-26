"""Observe readiness after failed Lua bootstrap and sqlite::memory: startup."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build-dir", type=Path, default=Path(".dev/build/release"))
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
build = args.build_dir if args.build_dir.is_absolute() else repo / args.build_dir
sys.path.insert(0, str(repo / "tests"))
from certificates import certificates

openssl = shutil.which("openssl")
if not openssl or not (build / "fsnet-service").is_file():
    parser.error("needs openssl and an existing fsnet-service build with Lua/SQLite")
results = []
with tempfile.TemporaryDirectory(prefix="fsnet-launch-review-") as folder:
    temporary = Path(folder)
    certificates(openssl, temporary)
    der = subprocess.run([openssl, "x509", "-in", str(temporary / "client.pem"),
                          "-outform", "DER"], check=True, capture_output=True,
                         timeout=5).stdout
    (temporary / "pins.tsv").write_text("sha256:" + hashlib.sha256(der).hexdigest()
                                       + " gateway-fixture\n")
    env = {key: value for key, value in os.environ.items() if not key.startswith("FSNET_")}
    env.update(FSNET_TLS_CA=str(temporary / "ca.pem"),
               FSNET_TLS_CERTIFICATE=str(temporary / "server.pem"),
               FSNET_TLS_KEY=str(temporary / "server.key"),
               FSNET_BIND_ADDRESS="127.0.0.1")
    for name, uri in [("failed_bootstrap", "file:" + str(temporary / "durable")),
                      ("volatile_sqlite", "sqlite::memory:")]:
        script = temporary / (name + ".lua")
        script.write_text('runtime.start(function() print("BOOTSTRAP_ENTER"); '
                          'error("fixture bootstrap failed") end)\n')
        metrics = temporary / (name + ".json")
        current = env | {"FSNET_STORAGE_URI": uri, "FSNET_METRICS_FILE": str(metrics)}
        child = subprocess.Popen([str(build / "fsnet-service"), "owner", str(script),
                                  "0", "fixture-owner", str(temporary / "pins.tsv")],
                                 env=current, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        ready, reads = None, 0
        deadline = time.monotonic() + 2
        try:
            while time.monotonic() < deadline and child.poll() is None:
                if metrics.exists():
                    try:
                        ready = json.loads(metrics.read_text())
                        reads += 1
                    except (json.JSONDecodeError, OSError):
                        pass
                if reads >= 5:
                    break
                time.sleep(.08)
            alive = child.poll() is None
        finally:
            if child.poll() is None:
                child.send_signal(signal.SIGTERM)
            try:
                out, err = child.communicate(timeout=4)
            except subprocess.TimeoutExpired:
                child.kill()
                out, err = child.communicate(timeout=2)
        results.append({"case": name, "process_alive_at_observation": alive,
                        "reads": reads, "metrics": ready, "exit_code": child.returncode,
                        "stdout": out.strip(), "stderr": err.strip()})
print(json.dumps(results, indent=2))
