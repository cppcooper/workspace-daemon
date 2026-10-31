#!/usr/bin/python3
"""Real disposable Git/OverlayFS/socket/process tests; no models or Paperclip calls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import stat
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
MANAGER = Path(os.environ.get("WORKSPACE_MANAGER_PATH", "/home/builder/agents/releases/coordination/9e344cac8e5aead21d306996db87bf44d419279a"))


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True, stderr=subprocess.PIPE).strip()


class Fixture:
    def __init__(self, daemon, client):
        self.base = Path(tempfile.mkdtemp(prefix="wd-"))
        self.daemon, self.client = daemon, client
        self.socket = str(self.base / "wd.sock")
        self.source = self.base / "repo"
        self.source.mkdir()
        git(self.source, "init", "-q", "--template=", "--initial-branch=base")
        git(self.source, "config", "user.name", "Workspace test")
        git(self.source, "config", "user.email", "test@example.invalid")
        (self.source / "tracked.txt").write_text("base\n")
        (self.source / "unchanged.bin").write_bytes(b"x" * 1048576)
        git(self.source, "add", ".")
        git(self.source, "commit", "-qm", "fixture")
        self.pin = git(self.source, "rev-parse", "HEAD")
        self.owner = {"host": socket.gethostname(), "epoch": 1}
        profile = {"schema": 1, "backend": "shared-history-v1", "host": socket.gethostname(), "uid": os.getuid(),
                   "workspace_root": str(self.base / "workspaces"), "cache_root": str(self.base / "history"),
                   "recovery_root": str(self.base / "recovery"), "repositories": {"demo": {"sources": [str(self.source)]}},
                   "git_identity": {"name": "Workspace test", "email": "test@example.invalid"}}
        registry = {"schema": 1, "projects": {"demo": {"selected": True, "source": str(self.source), "pin": self.pin,
                                                    "owner": self.owner, "required_artifacts": []}}}
        self.config = self.base / "config.json"
        self.state = self.base / "state"
        probe = '''import os,json,subprocess
from pathlib import Path
p=Path.cwd(); p.joinpath('tracked.txt').write_text('edited\\n')
for name in ['build','install','cache','tmp']: (p.parent/name/'out.txt').write_text(name)
denied=[]
for name in DENIED:
 try: Path(name).write_text('bad')
 except OSError: denied.append(name)
 else: raise RuntimeError('write accepted:'+name)
subprocess.run(['/usr/bin/git','add','tracked.txt'],check=True)
subprocess.run(['/usr/bin/git','commit','-qm','owned edit'],check=True)
print(json.dumps({'denied':denied,'cwd':str(p),'branch':subprocess.check_output(['/usr/bin/git','symbolic-ref','HEAD'],text=True).strip()}))
'''.replace("DENIED", repr([str(self.base / "workspaces/tasks/peer/source/tracked.txt"),
                           str(self.base / "profile.json"), str(self.state / "lowers" / ("demo-" + self.pin) / "tracked.txt"),
                           str(self.base / "history/forbidden")]))
        slow = '''import os,subprocess,time
from pathlib import Path
Path('job-namespace.txt').write_text(os.readlink('/proc/self/ns/pid')+'\\n'+str(time.monotonic_ns()))
subprocess.Popen(['/usr/bin/python3','-c','import os,time; os.setsid(); time.sleep(60)'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
time.sleep(60)
'''
        commands = {
            "inspect": {"argv": ["/usr/bin/python3", "-c", "import os,json;from pathlib import Path;print(json.dumps({'cwd':os.getcwd(),'content':Path('tracked.txt').read_text()}))"], "timeout_seconds": 5},
            "edit": {"argv": ["/usr/bin/python3", "-c", probe], "timeout_seconds": 10},
            "slow": {"argv": ["/usr/bin/python3", "-c", slow], "timeout_seconds": 20},
            "deadline": {"argv": ["/usr/bin/python3", "-c", slow], "timeout_seconds": 0.5},
            "bad": {"argv": ["/usr/bin/python3", "-c", "raise SystemExit(23)"], "timeout_seconds": 5},
        }
        for name, value in [("profile.json", profile), ("registry.json", registry), ("config.json", {
                "schema": 1, "allowed_uid": os.getuid(), "socket": self.socket, "state_root": str(self.state),
                "manager_path": str(MANAGER), "bridge": str(ROOT / "bridge/manager.py"), "python": "/usr/bin/python3",
                "profile": str(self.base / "profile.json"), "registry": str(self.base / "registry.json"),
                "owner": self.owner, "commands": commands, "read_only_roots": [str(self.base / "profile.json")],
                "max_operation_seconds": 30, "max_records": 128})]:
            (self.base / name).write_text(json.dumps(value, indent=2) + "\n")
        self.process = None

    def start(self, activated=False):
        self.log = open(self.base / ("daemon-" + str(time.time_ns()) + ".log"), "w+")
        self.activation = None
        if activated:
            self.activation = socket.socket(socket.AF_UNIX)
            self.activation.bind(self.socket)
            os.chmod(self.socket, 0o600)
            self.activation.listen(16)
            activate = "import os,sys;fd=int(sys.argv[1]);os.dup2(fd,3);os.set_inheritable(3,True);os.environ['LISTEN_FDS']='1';os.environ['LISTEN_PID']=str(os.getpid());os.execv(sys.argv[2],sys.argv[2:])"
            self.process = subprocess.Popen(["/usr/bin/python3", "-c", activate, str(self.activation.fileno()),
                                             self.daemon, "--config", str(self.config)],
                                            pass_fds=(self.activation.fileno(),), stdout=self.log, stderr=self.log)
        else:
            self.process = subprocess.Popen(["/usr/bin/setpriv", "--no-new-privs", self.daemon, "--config", str(self.config)], stdout=self.log, stderr=self.log)
        for _ in range(100):
            if self.process.poll() is not None:
                self.log.seek(0)
                raise RuntimeError("daemon failed: " + self.log.read())
            if Path(self.socket).exists():
                try:
                    if self.rpc({"op": "list"})["ok"]:
                        return
                except OSError:
                    pass
            time.sleep(0.03)
        raise TimeoutError("daemon startup")

    def stop(self, hard=False):
        if self.process and self.process.poll() is None:
            self.process.kill() if hard else self.process.terminate()
            self.process.wait(timeout=8)
        if self.log:
            self.log.close()
        if self.activation:
            self.activation.close()
            self.activation = None

    def rpc(self, request):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(5)
            s.connect(self.socket)
            s.sendall(json.dumps(request).encode() + b"\n")
            result = bytearray()
            while not result.endswith(b"\n"):
                piece = s.recv(65536)
                assert piece, "unexpected socket close"
                result.extend(piece)
            return json.loads(result)

    def submit(self, op, task="one", **kw):
        q = {"op": op, "request_id": str(uuid.uuid4()), "task": task, **kw}
        r = self.rpc(q)
        assert r["ok"], r
        return q, r["operation_id"]

    def wait(self, ident):
        for _ in range(600):
            r = self.rpc({"op": "operation", "operation_id": ident})
            if r["status"] not in ["queued", "running"]:
                return r
            time.sleep(0.03)
        raise TimeoutError(ident)

    def good(self, op, task="one", **kw):
        q, ident = self.submit(op, task, **kw)
        r = self.wait(ident)
        assert r["status"] == "succeeded", r
        return q, r

    def no_processes(self, namespace):
        for p in Path("/proc").iterdir():
            if p.name.isdigit():
                try:
                    assert os.readlink(p / "ns/pid") != namespace, "owned child survived"
                except (OSError, PermissionError):
                    pass

    def host_worker(self, internal_pid):
        """Find only this fixture's worker in the host's /proc view."""
        for p in Path("/proc").iterdir():
            if not p.name.isdigit():
                continue
            try:
                text = (p / "status").read_text()
                ids = next(line.split()[1:] for line in text.splitlines() if line.startswith("NSpid:"))
                if len(ids) != 3 or int(ids[1]) != internal_pid or ids[-1] != "1":
                    continue
                parent = int(next(line.split()[1] for line in text.splitlines() if line.startswith("PPid:")))
                parent_text = Path('/proc', str(parent), 'status').read_text()
                owner = int(next(line.split()[1] for line in parent_text.splitlines() if line.startswith("PPid:")))
                if owner == self.process.pid:
                    return int(p.name)
            except (OSError, StopIteration):
                continue
        raise RuntimeError("owned host worker PID unavailable")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--daemon", required=True)
    p.add_argument("--client", required=True)
    args = p.parse_args()
    f = Fixture(args.daemon, args.client)
    passed = []
    try:
        f.start()
        assert stat.S_IMODE(f.state.stat().st_mode) == 0o700
        assert stat.S_IMODE(Path(f.socket).stat().st_mode) == 0o600
        outside = f.base.parent / (f.base.name + '-readonly-probe')
        outside.write_text('preserve\n')
        try:
            children = Path('/proc', str(f.process.pid), 'task', str(f.process.pid), 'children').read_text().split()
            assert len(children) == 1
            probe = 'import errno,sys;from pathlib import Path\ntry: Path(sys.argv[1]).write_text("bad")\nexcept OSError as error: assert error.errno == errno.EROFS\nelse: raise RuntimeError("daemon filesystem is writable outside storage")'
            subprocess.run(['/usr/bin/nsenter', '--user', '--mount', '--preserve-credentials',
                            '--target', children[0], '--', '/usr/bin/python3', '-c', probe, str(outside)],
                           check=True, timeout=10, capture_output=True)
            assert outside.read_text() == 'preserve\n'
        finally:
            outside.unlink(missing_ok=True)
        passed.append('native read-only filesystem enforcement without host mount propagation')
        q, first = f.good("prepare", project="demo")
        passed.append("real manager preparation and rootless overlay")
        assert f.rpc(q)["operation_id"] == first["operation_id"]
        assert not f.rpc({**q, "task": "different"})["ok"]
        assert not f.rpc({"op": "prepare", "request_id": str(uuid.uuid4()), "task": "../escape", "project": "demo"})["ok"]
        assert not f.rpc({"op": "run", "request_id": str(uuid.uuid4()), "task": "one", "command": "shell", "argv": ["/bin/sh"]})["ok"]
        passed.append("durable idempotency, changed requests and arbitrary argv refusal")
        f.good("prepare", "peer", project="demo")
        one, peer = [f.rpc({"op": "status", "task": t})["workspace"] for t in ["one", "peer"]]
        assert one["lower"] == peer["lower"]
        assert not Path(one["upper"], "unchanged.bin").exists()
        assert not Path(peer["upper"], "unchanged.bin").exists()
        before = hashlib.sha256((f.source / "tracked.txt").read_bytes()).hexdigest()
        _, edited = f.good("run", command="edit")
        evidence = json.loads(edited["result"]["stdout"])
        assert len(evidence["denied"]) == 4 and evidence["branch"] == "refs/heads/task/one"
        _, inspected = f.good("run", "peer", command="inspect")
        assert json.loads(inspected["result"]["stdout"])["content"] == "base\n"
        assert hashlib.sha256((f.source / "tracked.txt").read_bytes()).hexdigest() == before
        passed.append("shared working-file lower, private Git/edits/outputs, actual denied peer/control/history/lower writes")
        f.good("prepare", project="demo")
        _, inspected = f.good("run", command="inspect")
        assert json.loads(inspected["result"]["stdout"])["content"] == "edited\n"
        passed.append("repeat preparation preserves private commits")
        _, ident = f.submit("run", command="slow")
        marker = Path(one["upper"], "job-namespace.txt")
        for _ in range(200):
            if marker.exists():
                break
            time.sleep(0.02)
        assert marker.exists()
        namespace = marker.read_text().splitlines()[0]
        assert not f.rpc({"op": "prepare", "request_id": str(uuid.uuid4()), "project": "demo", "task": "blocked"})["ok"]
        f.rpc({"op": "cancel", "operation_id": ident})
        assert f.wait(ident)["status"] == "cancelled"
        f.no_processes(namespace)
        f.good("run", command="inspect")
        passed.append("cancellation drains detached descendants, excludes another operation, and permits later healthy work")
        _, ident = f.submit("run", command="deadline")
        assert f.wait(ident)["status"] == "failed"
        f.no_processes(marker.read_text().splitlines()[0])
        f.good("run", command="inspect")
        passed.append("command deadline drains namespace before later admission")
        previous_namespace = marker.read_text()
        _, ident = f.submit("run", command="slow")
        for _ in range(200):
            running = f.rpc({"op": "operation", "operation_id": ident})
            if marker.exists() and marker.read_text() != previous_namespace and running.get("owned_active_writer"):
                break
            time.sleep(0.02)
        assert running.get("owned_active_writer")
        namespace = marker.read_text().splitlines()[0]
        assert marker.read_text() != previous_namespace
        os.kill(f.host_worker(running["worker_pid"]), signal.SIGKILL)
        assert f.wait(ident)["status"] == "failed"
        f.no_processes(namespace)
        manifest = f.base / "workspaces/tasks/one/workspace.json"
        original = manifest.read_text()
        changed = json.loads(original)
        changed["writer_identity"]["ticks"] = "0"
        changed["writer_identity"]["boot"] = "unrelated-boot"
        manifest.write_text(json.dumps(changed) + "\n")
        _, refused = f.submit("recover")
        assert f.wait(refused)["status"] == "failed"
        manifest.write_text(original)
        f.good("recover")
        f.good("run", command="inspect")
        passed.append("forced supervisor death drains detached child and permits explicit owned recovery")
        f.stop()
        pending = f.state / "server.json.pending-interrupted"
        pending.write_text('{"unfinished":')
        f.start()
        assert pending.read_text() == '{"unfinished":'
        tracked = f.rpc({"op": "list"})["workspaces"]
        assert any(t["task"] == "one" and t["tracked"] and not t["loaded"] for t in tracked)
        manifest.rename(manifest.with_suffix(".unavailable"))
        try:
            status = f.rpc({"op": "status", "task": "one"})["workspace"]
            assert status["tracked"] and status["recovery_required"] and not status["native_manifest_available"]
            assert f.rpc({"op": "list"})["ok"]
        finally:
            manifest.with_suffix(".unavailable").rename(manifest)
        f.good("reload")
        _, inspected = f.good("run", command="inspect")
        assert json.loads(inspected["result"]["stdout"])["content"] == "edited\n"
        passed.append("persistent tracking and explicit restart reload preserve private Git and edits")
        for task in ["one", "peer"]:
            f.good("prepare", task, project="demo")
            f.good("checkpoint", task, dirty=True)
        _, refused = f.submit("release")
        assert f.wait(refused)["status"] == "failed", "dirty work was released"
        assert f.rpc({"op": "status", "task": "one"})["mounted"]
        # The cancellation marker is intentionally untracked: release must refuse
        # silently discarding it. A separate clean task proves actual finish.
        f.good("prepare", "clean", project="demo")
        f.good("release", "clean")
        assert f.rpc({"op": "status", "task": "clean"})["workspace"]["phase"] == "released"
        passed.append("native checkpoints and normal unmount/retained release")
        client = subprocess.run([args.client, "--socket", f.socket, "status", "one"], capture_output=True, text=True)
        assert client.returncode == 0 and json.loads(client.stdout)["ok"]
        passed.append("native C++ client")
        previous_marker = marker.read_text()
        _, ident = f.submit("run", command="slow")
        for _ in range(200):
            running = f.rpc({"op": "operation", "operation_id": ident})
            if marker.exists() and marker.read_text() != previous_marker and running.get("owned_active_writer"):
                break
            time.sleep(0.02)
        assert marker.read_text() != previous_marker
        assert running.get("owned_active_writer")
        namespace = marker.read_text().splitlines()[0]
        f.stop(hard=True)
        for attempt in range(100):
            try:
                f.no_processes(namespace)
                break
            except AssertionError:
                time.sleep(0.02)
        else:
            raise AssertionError("daemon crash left an owned child")
        # Simulate lost/unavailable live view and cache after a crash. Restore
        # must use the independent durable checkpoint, never overwrite either.
        upper = Path(one["upper"])
        upper.rename(upper.with_name("upper-retained"))
        history = f.base / "history"
        history.rename(f.base / "history-retained")
        f.start()
        assert f.rpc({"op": "operation", "operation_id": ident})["status"] == "interrupted"
        assert not f.rpc({"op": "status", "task": "one"})["workspace"]["loaded"]
        _, restored = f.good("restore", restore_id="crash-inspection")
        restored_source = Path(restored["result"]["directory"]) / "source"
        assert (restored_source / "tracked.txt").read_text() == "edited\n"
        assert upper.with_name("upper-retained").exists()
        (f.base / "history-retained").rename(history)
        upper.with_name("upper-retained").rename(upper)
        f.good("reload")
        assert f.rpc({"op": "status", "task": "one"})["workspace"]["recovery_required"]
        f.good("recover")
        f.good("run", command="inspect")
        passed.append("whole-daemon crash retains tracking, independent checkpoint restore and explicit proven recovery")
        f.stop()
        Path(f.socket).unlink(missing_ok=True)
        f.start(activated=True)
        assert f.rpc({"op": "list"})["ok"]
        passed.append("systemd-compatible inherited Unix socket activation")
        print(json.dumps({"passed": passed, "fixture": str(f.base)}, indent=2))
    finally:
        f.stop()
        # A failure keeps its fixture and diagnostic logs. Successful fixtures
        # are disposable and leave no model, service, mount or Paperclip changes.
        if len(passed) == 13:
            # OverlayFS leaves a mode-000 work/work directory owned by this UID.
            # Normalize only this disposable fixture after its namespace is gone.
            for directory, children, _ in os.walk(f.base):
                for child in children:
                    path = Path(directory) / child
                    info = path.lstat()
                    if stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid():
                        path.chmod(stat.S_IMODE(info.st_mode) | 0o700)
            shutil.rmtree(f.base)
        else:
            print("retained fixture:", f.base, flush=True)


if __name__ == "__main__":
    main()
