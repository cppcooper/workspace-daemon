# workspace-daemon

A local C++20 service and client for preparing shared-base Git workspaces and
running **operator-approved commands** inside them. It wraps the existing Python
workspace manager; it does not replace its Git, lease or checkpoint logic.

C++ is used for native Linux sockets, namespaces, file-descriptor ownership and
process supervision. Python remains only as the adapter to an already tested
Python manager, avoiding a rewrite of its storage/recovery behavior. This is an
I/O-bound service; no language-speed or production-throughput claim is made.

There is no model, agent, scheduler, network listener, Board credential or
Paperclip dependency in this service. It accepts local Unix-socket requests and
does work only in response to them. **Building and testing do not install a
persistent service or connect an orchestrator.**

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Dependencies: Linux user/mount/PID namespaces, OverlayFS, bubblewrap, Git,
CMake, a C++20 compiler, Python 3, and the pinned manager module path. The
vendored nlohmann/json 3.12.0 header has its license and hashes under `vendor/`.
The default integration fixture uses the installed manager revision
`9e344cac8e5aead21d306996db87bf44d419279a`; set `WORKSPACE_MANAGER_PATH` when testing
another reviewed installation. Tests use disposable repositories, real mounts,
real processes and Unix sockets. They do not call Paperclip or launch models.

## Client contract

```sh
export WORKSPACE_DAEMON_SOCKET="$XDG_RUNTIME_DIR/workspace-daemon.sock"
workspacectl prepare PROJECT TASK --wait
workspacectl run TASK APPROVED_COMMAND --wait
workspacectl list
workspacectl status TASK
workspacectl reload TASK --wait
workspacectl operation OPERATION_ID
workspacectl cancel OPERATION_ID
workspacectl checkpoint TASK --dirty --wait
workspacectl recover TASK --wait
workspacectl restore TASK NEW_RESTORE_ID --wait
workspacectl release TASK --wait
```

`PROJECT` is an operator-registered repository alias. `TASK` is the stable task
identifier, not a path or shell command. An accepted mutation returns an operation
ID; use it to inspect or cancel. `--request-id ID` makes retries explicitly
idempotent. Reusing that ID with changed arguments is rejected. A different
request ID never permits a changed workspace/project binding. Preparation itself
is repeatable and retains existing edits and private commits.
The client prints its request ID before sending, including when a response is
lost. Reuse that ID after an uncertain result; a new ID means a new operation.

Only one mutation runs at a time. Status and cancellation remain available while
it runs. Requests cannot provide executable paths, argv, environment, sources,
pins, mount options or output directories. Only the host-owned configuration can
define executable/argv profiles. Source/history/control/peer access is constrained
by bubblewrap; the command runs with writable source and four private outputs,
read-only shared Git objects and no network. No shell is involved in dispatch.

### Important execution boundary

The daemon owns a private mount and PID namespace. **Use `run` to execute in its
workspace view.** A host process that simply changes directory to the returned
path will not see that overlay. Paperclip integration must call this client from
its approved provisioning/execution/teardown paths; a provisioning hook alone
does not move an unrelated provider process into the daemon's namespace.

The current executable profiles support fixed commands only. Model/network/
credential forwarding and a native Paperclip execution adapter are not provided.
This repository is not evidence of completed SOP-43 runtime acceptance.

## Configuration and startup

`examples/` contains matching configuration/profile/registry templates. Replace
the example source and exact commit with a clean, explicitly selected repository;
update paths, host, UID and git identity. Keep them outside every task write root.
The manager validates owner, source, pin, repository and storage contracts.
Profile, registry and service configuration are snapshotted at startup; changing
them requires an operator-reviewed restart. Drain work before changing these
files. Existing registry bindings stay immutable while their tasks are retained;
automatic Git fetching/base advancement is outside this initial version. Native
manager recovery/publication rules remain in force.

```sh
build/workspace-daemon --config /absolute/path/config.json
```

Startup uses an unprivileged Linux user namespace mapping the current host UID
to itself. Mount privileges apply only inside that namespace. The host account
does not gain sudo or host-wide mount rights. Failure to establish isolation is
fatal; there is no uncontained fallback. The service must run as an ordinary
account and always enables `NoNewPrivileges`. Its local authorization boundary is that configured Unix UID, with a
0600 socket and `SO_PEERCRED`, **not individual Paperclip agent identities**.
Expose the socket to the trusted integration, not arbitrary worker sandboxes.

`systemd/` supplies optional user socket/service units. After reviewing the
configuration, install the binaries/bridge with CMake, create the configured
state root, place the units in the user unit directory, and enable only the
socket. The supplied units keep the service and all its descendants in
`paperclip.slice`, capped at 512 MiB / 50% CPU, with no swap and 128 tasks.
They do not edit the existing Paperclip application or shared-slice limits.
No installation/enabling is performed by the build or tests.

The daemon makes its private filesystem read-only, permitting writes only to
the private common parent of manager/control storage and the socket directory.
The common parent must be owned by the service UID with mode 0700; manager stores
must be below it so retained-seed renames stay on one mount. Its private proc
instance remains writable for namespace helpers. This uses Linux
`mount_setattr(2)` and fails closed if unsupported. These restrictions are applied
after host ownership validation: systemd's `ProtectSystem`/`ProtectHome` options
for user services otherwise introduce an earlier user namespace that hides
root ownership and makes that validation fail. Worker commands retain their
separate bubblewrap restrictions. Mount changes never propagate to the host.

## Storage and failure behavior

Unchanged working files share an immutable read-only lower per project/pin.
Each task has a private upper, private Git metadata, work directory and output
directories. Initial manager seed copies are retained for recovery. Measurements
must count those copies; this is not a zero-duplication or production benchmark.
Tasks may use ordinary Git, including private commits; the selected original
repository and other task working files remain unchanged.

The Python adapter acquires the real manager writer guard and keeps it until
its command and every descendant have drained. Each operation is also the init
of its own PID namespace: a crashed/killed supervisor cannot leave detached
children running on the host. Cancellation and deadlines first request normal
drainage; the daemon can terminate that exact owned namespace after its grace
period. It never signals unrelated host process groups.

The workspace catalog and operation journal are persistent files, written with
file/directory fsync and atomic rename. Restart does **not** reload workspaces,
retry interrupted commands, reconcile writers or remove anything. `list` and
`status TASK` distinguish **tracked**, **loaded**, and **recovery_required**.
Tracking remains visible even when a native manager manifest is unavailable;
the status reports the missing/damaged state. `list OFFSET` pages larger catalogs.

Choose the action appropriate to the retained state:

| Request | Result |
| --- | --- |
| `reload TASK` | Mount the retained exact lower/upper/work layers at the original logical path. Does not clear an uncertain writer. Caller-requested `prepare`/`run` also load their workspace as needed. |
| `recover TASK` | Reconcile only a recorded owned writer whose process namespace is proved stopped; preserves edits and the existing lease/owner rules. |
| `restore TASK NEW_RESTORE_ID` | Restore the latest completed checkpoint to a **new independent inspection directory**, without mounting the original workspace or overwriting it. Returns that directory; this is not automatic adoption as another writable task. |
| `release TASK` | Checkpoint, unmount normally and release through the existing manager; retain data. This is a caller's explicit lifecycle decision. |

Recovery matches the current native writer's exact recorded identity. In this
service instance the helper must have been reaped. Across restarts the prior
namespace-init process must be proved exited using its original host PID,
start ticks and kernel boot ID; a different recorded boot proves it cannot still
run after reboot. Missing ownership proof refuses recovery and retains work for
host review. An unrelated failure or a changed writer identity is insufficient.
Incomplete setup/release also remains available for host review rather than
being automatically repaired. Unfinished journal temporaries are preserved and
ignored, not interpreted as committed state or allowed to block new writes.

**The last completed checkpoint is the recovery guarantee.** Existing retained
uppers commonly survive a restart, but uncheckpointed changes and unflushed writes
cannot be guaranteed through a power loss. Clients should request a dirty-inclusive
checkpoint after useful edits and before planned shutdown. Preserve the catalog,
native workspace manifests, uppers/seeds and recovery store in host backups;
shared history alone is not a backup. Tests exercise clean restart, whole-daemon
SIGKILL, denied unproved recovery, and independent restore with the live upper
and history cache unavailable. They do not physically reboot or power-cut the host.

Release checkpoints the selected workspace, retains a complete merged inspection
view, normally unmounts its source, and delegates lease release to the manager.
Dirty/untracked work, required artifacts, publication uncertainty and other
borrowers' checkpoint requirements can prevent release. Preserve the error and
work; do not clear uppers or force/lazy-unmount. No workspace data is deleted.
Shared lowers are removed from the namespace when the service exits.
The daemon never decides to retire a workspace. External callers decide when to
request release; destructive data deletion is not exposed by this version.

Operation records/logs are private and bounded by `max_records` (default 128).
The service refuses additional mutations at capacity; archival is an explicit
host maintenance action so idempotency evidence is not silently discarded.
There is no automatic deletion, recurring execution or expiry renewal.
