#!/usr/bin/env python3
"""
libsavesync IPC client example

Demonstrates: init -> manifest_load -> register -> save -> pull
via subprocess + newline-delimited JSON. No external dependencies.
"""

import json
import subprocess
import sys
import tempfile
import os

IPC_BIN = "build/savesync-ipc"


def send_request(proc, method, params=None):
    """Send a JSON request and return the parsed response."""
    msg = {"id": method, "method": method}
    if params:
        msg["params"] = params
    line = json.dumps(msg) + "\n"
    proc.stdin.write(line)
    proc.stdin.flush()
    resp_line = proc.stdout.readline()
    if not resp_line:
        raise RuntimeError(f"No response for {method}")
    return json.loads(resp_line)


def main():
    with tempfile.TemporaryDirectory() as tmpdir:
        # Create a synthetic manifest
        manifest_path = os.path.join(tmpdir, "test.cfg")
        with open(manifest_path, "w") as f:
            f.write("platform=ps2\n")
            f.write("emulator=testemu\n")
            f.write("shape=file\n")
            f.write("identity=none\n")

        # Create a live save directory with a test file
        live_path = os.path.join(tmpdir, "live_save")
        os.makedirs(live_path)
        with open(os.path.join(live_path, "data.bin"), "wb") as f:
            f.write(b"SYNTHETIC_SAVE_V1")

        # Start the IPC binary
        proc = subprocess.Popen(
            [IPC_BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            # 1. Initialize the engine
            resp = send_request(proc, "init", {"base_path": os.path.join(tmpdir, "data")})
            print(f"init: {resp}")

            # 2. Load a manifest
            resp = send_request(proc, "manifest_load", {"path": manifest_path})
            manifest_id = resp["result"]["manifest_id"]
            print(f"manifest_load: {resp}")

            # 3. Register with the manifest
            resp = send_request(proc, "register_with_manifest", {
                "manifest_id": manifest_id,
                "live_path": live_path,
                "shape": "file",
                "retention_count": 5,
            })
            reg_id = resp["result"]["registration_id"]
            print(f"register: {resp}")

            # 4. Save (snapshot) the current state
            resp = send_request(proc, "save", {"registration_id": reg_id})
            print(f"save: {resp}")

            # 5. List entries
            resp = send_request(proc, "list_entries", {"registration_id": reg_id})
            print(f"list_entries: {resp}")

            # 6. Pull (restore) the latest entry
            resp = send_request(proc, "pull", {"registration_id": reg_id})
            print(f"pull: {resp}")

        finally:
            # 7. Shut down cleanly
            resp = send_request(proc, "shutdown")
            print(f"shutdown: {resp}")
            proc.wait()


if __name__ == "__main__":
    main()
