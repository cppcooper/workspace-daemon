#!/usr/bin/python3
"""Private adapter to the pinned manager. Only invoked as an owned PID-namespace init.

No sockets, shell parsing, Paperclip credentials, provider selection or model calls.
The C++ service selects commands and owns mount/process namespaces.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import time


class Cancelled(Exception):
    pass


def stop(_sig, _frame):
    raise Cancelled("operation cancelled")


def drain():
    """Keep the manager's writer context until every owned descendant is reaped."""
    assert os.getpid() == 1, "namespace init required before process-tree signals"
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    for sig, seconds in [(signal.SIGTERM, 0.5), (signal.SIGKILL, 2.0)]:
        try:
            os.kill(-1, sig)
        except ProcessLookupError:
            pass
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                pid, _ = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                return
            if pid == 0:
                time.sleep(0.01)
    raise RuntimeError("owned children did not drain; writer remains uncertain")


def capture(argv, cwd, seconds, home):
    process = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               env={"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "HOME": home},
                               start_new_session=True)
    selector = selectors.DefaultSelector()
    streams = {"stdout": bytearray(), "stderr": bytearray()}
    truncated = False
    for name, stream in [("stdout", process.stdout), ("stderr", process.stderr)]:
        os.set_blocking(stream.fileno(), False)
        selector.register(stream, selectors.EVENT_READ, name)
    deadline = time.monotonic() + seconds
    try:
        while selector.get_map():
            if time.monotonic() >= deadline:
                raise TimeoutError("approved command deadline")
            for key, _ in selector.select(0.05):
                data = os.read(key.fileobj.fileno(), 4096)
                if not data:
                    selector.unregister(key.fileobj)
                    key.fileobj.close()
                    continue
                target = streams[key.data]
                remaining = 8192 - len(target)
                target.extend(data[:max(0, remaining)])
                truncated |= len(data) > remaining
        code = process.wait(timeout=max(0.01, deadline-time.monotonic()))
        return {"exit_code": code, "stdout": streams["stdout"].decode("utf-8", "replace"),
                "stderr": streams["stderr"].decode("utf-8", "replace"), "output_truncated": truncated}
    finally:
        selector.close()
        # All descendants, including ones with new process groups/closed pipes,
        # are still in this operation's PID namespace. No source is released yet.
        drain()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    if os.getpid() != 1:
        raise RuntimeError("manager adapter must run as private PID-namespace init")
    config = json.loads(args.config.read_text())
    raw = sys.stdin.buffer.readline(65538)
    if len(raw) > 65536 or not raw.endswith(b"\n"):
        raise ValueError("bounded JSON request required")
    request = json.loads(raw)
    sys.path.insert(0, config["manager_path"])
    from portable_coordination.workspaces import Workspaces
    manager = Workspaces(json.loads(Path(config["profile"]).read_text())["workspace_root"],
                         config["registry"], profile=config["profile"])
    task, owner = request["task"], config["owner"]
    op = request["op"]
    if op == "seed":
        # The C++ owner holds the real writer lock across this check and the
        # source conversion. Never turn an existing edited task into a shared base.
        from portable_coordination.workspace_store import git
        folder, manifest = manager.load(task, owner)
        if manifest["status"] not in ("paused", "checkpointed"):
            raise RuntimeError("workspace not drained for source conversion")
        if git(folder / "source", "rev-parse", "HEAD") != manifest["source_pin"]:
            raise RuntimeError("private commits precede overlay setup; preserve for host review")
        if git(folder / "source", "status", "--porcelain=v1", "--untracked-files=all"):
            raise RuntimeError("dirty source precedes overlay setup; preserve for host review")
        return {"clean": True}
    if op == "prepare":
        entry = manager.registry(request["project"], owner,
                                 json.loads(Path(config["registry"]).read_text())["projects"][request["project"]]["pin"])
        pin = entry["pin"]
        binding = {"project": request["project"], "task": task, "owner": owner, "pin": pin}
        digest = hashlib.sha256(json.dumps(binding, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        manager.create(request["project"], task, owner, pin, request_digest=digest)
        return manager.resume(task, owner)
    if op == "run":
        with manager.writer(task, owner) as folder:
            try:
                result = capture(request["argv"], str(folder / "source"), float(request["timeout_seconds"]), "/tmp")
                result["writer_held_through_drain"] = True
                if result["exit_code"]:
                    raise RuntimeError("approved command exited " + str(result["exit_code"]) + ": " + result["stderr"][:512])
                return result
            finally:
                drain()
    if op == "checkpoint":
        return manager.checkpoint(task, owner, include_dirty=request.get("dirty", False))
    if op == "inspect":
        folder, manifest = manager.load(task, owner)
        return {"task": task, "source": str(folder / "source"), "status": manifest["status"],
                "recovery_required": manifest["status"] == "active", "checkpoint": manifest.get("checkpoint")}
    if op == "restore":
        return manager.restore_checkpoint(task, owner, request["restore_id"])
    if op == "release":
        return manager.finish(task, owner)
    if op == "recover":
        return manager.resume(task, owner, reconcile=True)
    raise ValueError("unknown manager operation")


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        result = main()
        drain()
        print(json.dumps({"ok": True, "result": result}), flush=True)
    except BaseException as error:
        # A crash before normal writer teardown leaves native manager state
        # active/uncertain. The C++ parent waits for namespace exit before recovery.
        try:
            drain()
        except BaseException:
            os._exit(125)
        print(json.dumps({"ok": False, "error": str(error)[:2048]}), flush=True)
