#include "common.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sched.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>

using namespace wd;
namespace {
std::atomic<bool> stopping = false;
static_assert(std::atomic<bool>::is_always_lock_free);
volatile sig_atomic_t supervised_child = 0;
void stop_signal(int sig) {
    stopping = 1;
    if (supervised_child > 0)
        kill(supervised_child, sig);
}
void install_signals() {
    struct sigaction sa{};
    sa.sa_handler = stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}
void kernel_file(const char *path, const std::string &s) {
    FD f(open(path, O_WRONLY | O_CLOEXEC));
    system_check(f.value >= 0, "open namespace map");
    write_all(f.value, s);
}
int namespace_supervisor() {
    check(getuid() != 0 && geteuid() == getuid(), "run as an ordinary, non-setuid host account");
    auto uid = getuid();
    auto gid = getgid();
    system_check(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0,
                 "preserve no-new-privileges boundary");
    system_check(unshare(CLONE_NEWUSER) == 0, "create user namespace");
    kernel_file("/proc/self/setgroups", "deny\n");
    kernel_file("/proc/self/uid_map", std::to_string(uid) + " " + std::to_string(uid) + " 1\n");
    kernel_file("/proc/self/gid_map", std::to_string(gid) + " " + std::to_string(gid) + " 1\n");
    system_check(unshare(CLONE_NEWNS | CLONE_NEWPID) == 0, "create service namespaces");
    system_check(mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == 0,
                 "make mounts private");
    auto child = fork();
    system_check(child >= 0, "fork namespace init");
    if (child == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        system_check(mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, nullptr) == 0,
                     "mount private proc");
        return -1;
    }
    supervised_child = child;
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            throw Error("wait namespace init");
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
void copy_tree(const fs::path &from, const fs::path &to, bool git_metadata,
               const std::function<void()> &checkpoint) {
    checkpoint();
    directory(to);
    for (const auto &e : fs::directory_iterator(from)) {
        checkpoint();
        if (!git_metadata && e.path().filename() == ".git")
            continue;
        auto dest = to / e.path().filename();
        auto kind = e.symlink_status();
        if (fs::is_symlink(kind))
            fs::create_symlink(fs::read_symlink(e.path()), dest);
        else if (fs::is_directory(kind)) {
            copy_tree(e.path(), dest, git_metadata, checkpoint);
            fs::permissions(dest, kind.permissions());
        } else if (fs::is_regular_file(kind)) {
            fs::copy_file(e.path(), dest);
            fs::permissions(dest, kind.permissions());
        } else
            throw Error("special file in source; work retained");
    }
}
bool is_mounted(const fs::path &p) {
    // Control paths disallow whitespace/escaping; paths from the manager are under them.
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string s;
        for (int i = 0; i < 5; i++)
            fields >> s;
        if (s == p.string())
            return true;
    }
    return false;
}
struct ChildLaunch {
    int input, output, error;
    char **argv;
    char **env;
};
int child_entry(void *raw) {
    auto *x = static_cast<ChildLaunch *>(raw);
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0 ||
        mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, nullptr) != 0)
        _exit(125);
    if (dup2(x->input, STDIN_FILENO) < 0 || dup2(x->output, STDOUT_FILENO) < 0 ||
        dup2(x->error, STDERR_FILENO) < 0)
        _exit(126);
    execve(x->argv[0], x->argv, x->env);
    _exit(127);
}
struct Job {
    json record;
    pid_t pid = 0;
    bool cancelled = false;
    std::chrono::steady_clock::time_point deadline;
};
struct ScopeExit {
    std::function<void()> action;
    ~ScopeExit() { action(); }
};

class Server {
    fs::path config_file, state, root;
    json config, profile, registry;
    std::string config_text, profile_text, registry_text, instance = random_id();
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Job>> jobs;
    std::vector<std::jthread> threads;
    FD lock;
    FD host_proc{open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    json server_identity;
    std::set<std::string> lower_mounts;
    size_t maximum_records = 128;
    double maximum_seconds = 120;
    bool owns_socket = false;
    dev_t socket_device = 0;
    ino_t socket_inode = 0;
    std::string socket_path;
    std::string host_path(const std::string &suffix) const {
        check(host_proc.value >= 0, "original process namespace unavailable");
        return "/proc/self/fd/" + std::to_string(host_proc.value) + "/" + suffix;
    }
    json process_identity(const std::string &text) const {
        std::istringstream fields(text.substr(text.rfind(')') + 2));
        std::string value, state_value, ticks;
        for (int i = 0; i <= 19; ++i) {
            fields >> value;
            if (i == 0)
                state_value = value;
            if (i == 19)
                ticks = value;
        }
        return {{"host_pid", std::stol(text)}, {"ticks", ticks}, {"state", state_value}};
    }
    bool prior_server_exited(const json &owner) const {
        if (!owner.is_object() || !owner.contains("boot") || !owner.contains("host_pid") ||
            !owner.contains("ticks"))
            return false;
        if (owner["boot"] != server_identity["boot"])
            return true;
        auto name = std::to_string(owner["host_pid"].get<long>()) + "/stat";
        FD fd(openat(host_proc.value, name.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd.value < 0)
            return errno == ENOENT || errno == ESRCH;
        char buffer[4096];
        auto n = read(fd.value, buffer, sizeof buffer);
        if (n <= 0)
            return false;
        auto observed = process_identity(std::string(buffer, static_cast<size_t>(n)));
        return observed["ticks"] != owner["ticks"] || observed["state"] == "Z" ||
               observed["state"] == "X";
    }
    json tracked(const json &l) const {
        json result = l;
        auto task = l["task"].get<std::string>();
        result["tracked"] = true;
        result["loaded"] = is_mounted(l["descriptor"]["source"].get<std::string>());
        result["recovery_required"] = true;
        result["native_manifest_available"] = false;
        try {
            auto manifest = json::parse(read_text(root / "tasks" / task / "workspace.json"));
            result["native_manifest_available"] = true;
            result["recovery_required"] =
                manifest["status"] == "active" || manifest["status"] == "preparing" ||
                l["phase"] == "preparing" || l["phase"] == "release_copy_ready";
        } catch (const std::exception &error) {
            // The durable catalog is independent of the live manager files.
            // Missing/damaged live state must not erase a tracked workspace.
            result["native_manifest_error"] = std::string(error.what()).substr(0, 512);
        }
        return result;
    }
    void save(Job &j) {
        atomic_json(state / "operations" / (j.record["operation_id"].get<std::string>() + ".json"),
                    j.record);
    }
    fs::path layout_path(const std::string &task) { return state / "tasks" / (task + ".json"); }
    json layout(const std::string &task) { return json::parse(read_text(layout_path(task))); }
    void validate_controls() {
        check(read_text(config_file) == config_text &&
                  read_text(config["profile"].get<std::string>()) == profile_text &&
                  read_text(config["registry"].get<std::string>()) == registry_text,
              "operator configuration changed; restart after review");
    }
    void check_running(const std::shared_ptr<Job> &job) {
        std::lock_guard guard(mutex);
        check(!job->cancelled && !stopping, "operation cancelled");
        check(std::chrono::steady_clock::now() < job->deadline, "operation deadline");
    }
    json bridge(const std::shared_ptr<Job> &job, json request, unsigned step) {
        validate_controls();
        check_running(job);
        request["instance_id"] = instance;
        auto id = job->record["operation_id"].get<std::string>();
        auto prefix = state / "logs" / (id + "-" + std::to_string(step));
        FD out(open((prefix.string() + ".out").c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        FD err(open((prefix.string() + ".err").c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        system_check(out.value >= 0 && err.value >= 0, "create operation output");
        int pipefd[2];
        system_check(pipe2(pipefd, O_CLOEXEC) == 0, "create request pipe");
        FD readfd(pipefd[0]), writefd(pipefd[1]);
        std::vector<std::string> argv = {config["python"], "-I",       "-B",
                                         config["bridge"], "--config", config_file.string()};
        std::vector<char *> av;
        for (auto &s : argv)
            av.push_back(s.data());
        av.push_back(nullptr);
        std::vector<std::string> environment = {"PATH=/usr/bin:/bin", "LANG=C.UTF-8",
                                                "HOME=" + (state / "home").string()};
        std::vector<char *> ev;
        for (auto &s : environment)
            ev.push_back(s.data());
        ev.push_back(nullptr);
        ChildLaunch launch{readfd.value, out.value, err.value, av.data(), ev.data()};
        std::vector<char> stack(1024 * 1024);
        auto pid = clone(child_entry, stack.data() + stack.size(),
                         CLONE_NEWPID | CLONE_NEWNS | SIGCHLD, &launch);
        system_check(pid > 0, "create isolated operation process");
        bool reaped = false;
        ScopeExit child_owner{[&] {
            if (!reaped) {
                kill(pid, SIGKILL);
                int status = 0;
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
            }
            std::lock_guard guard(mutex);
            job->pid = 0;
            job->record["namespace_reaped"] = true;
        }};
        {
            std::lock_guard guard(mutex);
            job->pid = pid;
            job->record["namespace_reaped"] = false;
            job->record["worker_pid"] = pid;
            save(*job);
        }
        auto proc_stat = read_text("/proc/" + std::to_string(pid) + "/stat");
        std::istringstream proc_fields(proc_stat.substr(proc_stat.rfind(')') + 2));
        std::string start_ticks;
        for (int field = 0; field <= 19; field++)
            proc_fields >> start_ticks;
        close(readfd.release());
        write_all(writefd.value, request.dump() + "\n");
        close(writefd.release());
        std::optional<std::chrono::steady_clock::time_point> term;
        int code = 0;
        while (true) {
            auto w = waitpid(pid, &code, WNOHANG);
            if (w == pid) {
                reaped = true;
                break;
            }
            if (w < 0 && errno == EINTR)
                continue;
            system_check(w >= 0, "wait owned namespace");
            bool cancel;
            {
                std::lock_guard guard(mutex);
                cancel = job->cancelled;
            }
            if (request["op"] == "run") {
                auto manifest =
                    root / "tasks" / request["task"].get<std::string>() / "workspace.json";
                try {
                    auto m = json::parse(read_text(manifest));
                    auto identity = m.value("writer_identity", json::object());
                    if (m["status"] == "active" && identity.value("pid", 0) == 1 &&
                        identity.value("ticks", std::string()) == start_ticks) {
                        std::lock_guard guard(mutex);
                        bool first = !job->record.value("owned_active_writer", false);
                        job->record["owned_active_writer"] = true;
                        job->record["owned_writer_identity"] = identity;
                        if (first)
                            save(*job);
                    }
                } catch (const std::exception
                             &) { /* A native atomic transition may not be published yet. */
                }
            }
            if (cancel || stopping || std::chrono::steady_clock::now() >= job->deadline) {
                if (!term) {
                    kill(pid, SIGTERM);
                    term = std::chrono::steady_clock::now();
                } else if (std::chrono::steady_clock::now() - *term > std::chrono::seconds(2))
                    kill(pid, SIGKILL);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(WIFEXITED(code) && WEXITSTATUS(code) == 0,
              "manager helper interrupted; retained state may require recovery");
        auto response = json::parse(read_text(prefix.string() + ".out"));
        check(response.value("ok", false), response.value("error", "manager operation refused"));
        return response.at("result");
    }
    void bind_lower(const fs::path &p) {
        if (lower_mounts.contains(p.string()))
            return;
        system_check(mount(p.c_str(), p.c_str(), nullptr, MS_BIND | MS_REC, nullptr) == 0,
                     "bind shared lower");
        system_check(
            mount(nullptr, p.c_str(), nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr) == 0,
            "protect shared lower");
        lower_mounts.insert(p.string());
    }
    void mount_layout(json &l) {
        check(l["phase"] == "overlay", "workspace requires retained setup/release review");
        fs::path source = l["descriptor"]["source"].get<std::string>();
        if (is_mounted(source))
            return;
        check(fs::is_empty(source),
              "unmounted source contains unexpected data; review before remount");
        auto lower = l["lower"].get<std::string>();
        bind_lower(lower);
        auto options = "lowerdir=" + lower + ",upperdir=" + l["upper"].get<std::string>() +
                       ",workdir=" + l["work"].get<std::string>() +
                       ",userxattr,index=off,metacopy=off,redirect_dir=off";
        system_check(mount("overlay", source.c_str(), "overlay", 0, options.c_str()) == 0,
                     "mount task overlay");
    }
    json prepare_overlay(const std::shared_ptr<Job> &job, const std::string &task,
                         const std::string &project, const json &descriptor) {
        auto folder = root / "tasks" / task;
        fs::path source = descriptor.at("source").get<std::string>();
        check(source == folder / "source", "manager source binding differs");
        no_links(source);
        FD writer(open((folder / "writer.lock").c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
        system_check(writer.value >= 0 && flock(writer.value, LOCK_EX | LOCK_NB) == 0,
                     "protect source conversion with manager writer lock");
        bridge(job, {{"op", "seed"}, {"task", task}}, 2);
        auto base = state / "overlays" / task;
        directory(base);
        auto pin = registry["projects"][project]["pin"].get<std::string>();
        identifier(pin);
        auto lower = state / "lowers" / (project + "-" + pin);
        if (!fs::exists(lower)) {
            auto pending = lower.string() + ".pending-" + random_id();
            copy_tree(source, pending, false, [&] { check_running(job); });
            fs::rename(pending, lower);
        }
        json l = {{"task", task},
                  {"project", project},
                  {"descriptor", descriptor},
                  {"lower", lower.string()},
                  {"upper", (base / "upper").string()},
                  {"work", (base / "work").string()},
                  {"seed", (base / "seed").string()},
                  {"phase", "preparing"}};
        atomic_json(layout_path(task), l);
        directory(base / "upper");
        directory(base / "work");
        fs::rename(source, base / "seed");
        directory(source);
        for (const auto &raw : descriptor.at("worker_access").at("private_git_metadata")) {
            auto rel = fs::path(raw.get<std::string>()).lexically_relative(source);
            check(!rel.empty() && !rel.is_absolute() && *rel.begin() != "..",
                  "private git path differs");
            directory((base / "upper" / rel).parent_path());
            fs::rename(base / "seed" / rel, base / "upper" / rel);
        }
        l["phase"] = "overlay";
        atomic_json(layout_path(task), l);
        mount_layout(l);
        return l;
    }
    std::vector<std::string> sandbox(const json &l, const json &command) {
        std::vector<std::string> a = {"/usr/bin/bwrap",
                                      "--die-with-parent",
                                      "--new-session",
                                      "--unshare-pid",
                                      "--unshare-ipc",
                                      "--unshare-uts",
                                      "--unshare-net",
                                      "--tmpfs",
                                      "/",
                                      "--proc",
                                      "/proc",
                                      "--dev",
                                      "/dev",
                                      "--tmpfs",
                                      "/tmp",
                                      "--ro-bind",
                                      "/usr",
                                      "/usr",
                                      "--ro-bind",
                                      "/etc",
                                      "/etc"};
        for (const auto *p : {"/bin", "/sbin", "/lib", "/lib64"}) {
            if (fs::is_symlink(p)) {
                a.insert(a.end(), {"--symlink", fs::read_symlink(p).string(), p});
            } else if (fs::exists(p))
                a.insert(a.end(), {"--ro-bind", p, p});
        }
        std::set<std::string> reads;
        for (const auto &p : l["descriptor"]["worker_access"]["read_only_roots"])
            reads.insert(p.get<std::string>());
        reads.insert(l["lower"].get<std::string>());
        for (const auto &p : config.value("read_only_roots", json::array()))
            reads.insert(p.get<std::string>());
        for (const auto &p : reads) {
            no_links(p);
            a.insert(a.end(), {"--ro-bind", p, p});
        }
        for (const auto &p : l["descriptor"]["worker_access"]["write_roots"]) {
            auto s = p.get<std::string>();
            no_links(s);
            a.insert(a.end(), {"--bind", s, s});
        }
        a.insert(a.end(), {"--remount-ro", "/", "--chdir",
                           l["descriptor"]["source"].get<std::string>(), "--"});
        for (const auto &s : command.at("argv"))
            a.push_back(s.get<std::string>());
        return a;
    }
    json execute(const std::shared_ptr<Job> &j) {
        auto q = j->record.at("request");
        auto op = q["op"].get<std::string>(), task = q["task"].get<std::string>();
        json l;
        if (fs::exists(layout_path(task)))
            l = layout(task);
        if (op == "prepare") {
            auto project = q["project"].get<std::string>();
            if (!l.is_null()) {
                check(l["project"] == project, "task belongs to another project");
                mount_layout(l);
            }
            auto d = bridge(j, {{"op", "prepare"}, {"project", project}, {"task", task}}, 0);
            if (l.is_null())
                l = prepare_overlay(j, task, project, d);
            auto verified = bridge(j, {{"op", "prepare"}, {"project", project}, {"task", task}}, 1);
            check(verified["source"] == l["descriptor"]["source"], "source changed");
            l["descriptor"] = verified;
            atomic_json(layout_path(task), l);
            return {{"workspace", l["descriptor"]},
                    {"shared_lower", l["lower"]},
                    {"namespace_required", true},
                    {"instance_id", instance}};
        }
        check(!l.is_null(), "workspace is not prepared");
        if (op == "release" && l["phase"] == "materialized") {
            auto r = bridge(j, {{"op", "release"}, {"task", task}}, 0);
            l["phase"] = "released";
            atomic_json(layout_path(task), l);
            return r;
        }
        if (op == "restore")
            return bridge(j, {{"op", "restore"}, {"task", task}, {"restore_id", q["restore_id"]}},
                          0);
        mount_layout(l);
        if (op == "reload")
            return bridge(j, {{"op", "inspect"}, {"task", task}}, 0);
        if (op == "run") {
            auto command = config["commands"].at(q["command"].get<std::string>());
            auto r = bridge(j,
                            {{"op", "run"},
                             {"task", task},
                             {"argv", sandbox(l, command)},
                             {"timeout_seconds", command["timeout_seconds"]}},
                            0);
            return r;
        }
        if (op == "checkpoint")
            return bridge(j, {{"op", op}, {"task", task}, {"dirty", q.value("dirty", false)}}, 0);
        if (op == "recover") {
            bool drained = false;
            auto current_writer = json::parse(read_text(root / "tasks" / task / "workspace.json"))
                                      .value("writer_identity", json::object());
            {
                std::lock_guard guard(mutex);
                for (const auto &[id, p] : jobs) {
                    if (p == j || p->record["request"].value("task", "") != task ||
                        !p->record.value("owned_active_writer", false) ||
                        p->record.value("owned_writer_identity", json::object()) != current_writer)
                        continue;
                    bool current = p->record.value("instance_id", "") == instance;
                    bool exited = current ? p->record.value("namespace_reaped", false)
                                          : prior_server_exited(
                                                p->record.value("server_identity", json::object()));
                    if (exited &&
                        (p->record["status"] == "failed" || p->record["status"] == "cancelled" ||
                         p->record["status"] == "interrupted"))
                        drained = true;
                }
            }
            check(drained, "prior writer drainage is unproved; retain work for host review");
            return bridge(j, {{"op", "recover"}, {"task", task}}, 0);
        }
        check(op == "release", "unknown mutation");
        bridge(j, {{"op", "checkpoint"}, {"task", task}, {"dirty", false}}, 0);
        auto source = fs::path(l["descriptor"]["source"].get<std::string>());
        FD writer(
            open((source.parent_path() / "writer.lock").c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
        system_check(writer.value >= 0 && flock(writer.value, LOCK_EX | LOCK_NB) == 0,
                     "protect retained release view");
        check(json::parse(read_text(source.parent_path() / "workspace.json"))["status"] ==
                  "checkpointed",
              "workspace changed after checkpoint; retry release");
        auto retained =
            source.parent_path() / ("retained-" + j->record["operation_id"].get<std::string>());
        copy_tree(source, retained, true, [&] { check_running(j); });
        check_running(j);
        l["retained_view"] = retained.string();
        l["phase"] = "release_copy_ready";
        atomic_json(layout_path(task), l);
        system_check(umount2(source.c_str(), 0) == 0, "normal source unmount");
        check(fs::is_empty(source), "unexpected data below mount; retained view preserved");
        fs::remove(source);
        fs::rename(retained, source);
        l["phase"] = "materialized";
        atomic_json(layout_path(task), l);
        close(writer.release());
        auto result = bridge(j, {{"op", "release"}, {"task", task}}, 1);
        l["phase"] = "released";
        atomic_json(layout_path(task), l);
        return result;
    }
    void job_main(std::shared_ptr<Job> j) {
        try {
            {
                std::lock_guard guard(mutex);
                j->record["status"] = "running";
                j->record["started_at"] = now();
                save(*j);
            }
            auto result = execute(j);
            std::lock_guard guard(mutex);
            j->record["result"] = result;
            j->record["status"] = j->cancelled ? "cancelled" : "succeeded";
        } catch (const std::exception &e) {
            std::lock_guard guard(mutex);
            j->record["status"] = j->cancelled ? "cancelled" : "failed";
            j->record["error"] = std::string(e.what()).substr(0, 2048);
        }
        std::lock_guard guard(mutex);
        j->record["finished_at"] = now();
        j->record["worker_pid"] = nullptr;
        try {
            save(*j);
        } catch (const std::exception &) {
            std::cerr << "operation journal unavailable; stopping with work retained\n";
            stopping = true;
        }
    }

  public:
    explicit Server(fs::path file) : config_file(std::move(file)) {
        trusted(config_file);
        config_text = read_text(config_file);
        config = json::parse(config_text);
        check(config.at("schema") == 1, "unsupported config schema");
        check(config.at("allowed_uid") == getuid(), "daemon UID must equal configured caller UID");
        for (const auto *name : {"profile", "registry", "bridge"})
            trusted(config.at(name).get<std::string>());
        config["python"] = fs::canonical(config.at("python").get<std::string>()).string();
        trusted(config["python"].get<std::string>());
        no_links(config.at("manager_path").get<std::string>());
        profile_text = read_text(config["profile"].get<std::string>());
        registry_text = read_text(config["registry"].get<std::string>());
        profile = json::parse(profile_text);
        registry = json::parse(registry_text);
        root = profile.at("workspace_root").get<std::string>();
        state = config.at("state_root").get<std::string>();
        socket_path = config.at("socket");
        for (const auto &p : {root, state, fs::path(profile.at("cache_root").get<std::string>()),
                              fs::path(profile.at("recovery_root").get<std::string>())}) {
            no_links(p);
            for (unsigned char ch : p.string())
                check(ch > 32 && ch != 44 && ch != 58 && ch != 92,
                      "unsupported mount-option path characters");
        }
        for (const auto &p : {root, fs::path(profile.at("cache_root").get<std::string>()),
                              fs::path(profile.at("recovery_root").get<std::string>())})
            check(state != p && !state.string().starts_with(p.string() + "/") &&
                      !p.string().starts_with(state.string() + "/"),
                  "daemon controls overlap manager storage");
        check(config["owner"]["host"] == profile["host"], "owner host differs");
        maximum_records = config.value("max_records", size_t(128));
        check(maximum_records >= 1 && maximum_records <= 1024, "invalid record capacity");
        maximum_seconds = config.value("max_operation_seconds", 120.0);
        check(maximum_seconds >= 1 && maximum_seconds <= 600, "invalid operation deadline");
        check(config["commands"].is_object() && config["commands"].size() <= 64,
              "invalid approved command table");
        for (const auto &[key, c] : config["commands"].items()) {
            identifier(key);
            check(c.at("argv").is_array() && !c["argv"].empty() && c["argv"].size() <= 64,
                  "invalid approved argv");
            for (const auto &a : c["argv"])
                check(a.is_string() && a.get<std::string>().find('\0') == std::string::npos &&
                          a.get<std::string>().size() <= 4096,
                      "invalid approved argument");
            check(fs::path(c["argv"][0].get<std::string>()).is_absolute(),
                  "approved executable must be absolute");
            check(c.at("timeout_seconds").is_number() && c["timeout_seconds"].get<double>() > 0 &&
                      c["timeout_seconds"].get<double>() <= maximum_seconds,
                  "invalid command deadline");
        }
        for (const auto &p : config.value("read_only_roots", json::array()))
            no_links(p.get<std::string>());
        directory(state);
        check((fs::status(state).permissions() & (fs::perms::group_all | fs::perms::others_all)) ==
                  fs::perms::none,
              "daemon state root must have private 0700 permissions");
        for (const auto *name : {"operations", "logs", "tasks", "overlays", "lowers", "home"})
            directory(state / name);
        lock.value =
            open((state / "daemon.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        system_check(lock.value >= 0 && flock(lock.value, LOCK_EX | LOCK_NB) == 0,
                     "exclusive daemon ownership");
        for (const auto &entry : fs::directory_iterator(state / "operations"))
            if (entry.path().extension() == ".json") {
                auto j = std::make_shared<Job>();
                j->record = json::parse(read_text(entry.path()));
                auto id = identifier(j->record["operation_id"]);
                if (j->record["status"] == "running" || j->record["status"] == "queued") {
                    j->record["status"] = "interrupted";
                    j->record["namespace_reaped"] = false;
                    j->record["error"] = "daemon restarted; retained work requires review";
                    save(*j);
                }
                jobs[id] = j;
                check(jobs.size() <= maximum_records, "operation capacity exceeded");
            }
    }
    ~Server() {
        if (owns_socket) {
            struct stat s{};
            if (lstat(socket_path.c_str(), &s) == 0 && S_ISSOCK(s.st_mode) &&
                s.st_dev == socket_device && s.st_ino == socket_inode)
                unlink(socket_path.c_str());
        }
    }
    json handle(json q) {
        check(q.contains("op") && q["op"].is_string(), "operation required");
        auto op = q["op"].get<std::string>();
        std::set<std::string> fields = {"op"};
        if (op == "prepare")
            fields.insert({"request_id", "project", "task"});
        else if (op == "run")
            fields.insert({"request_id", "task", "command"});
        else if (op == "checkpoint")
            fields.insert({"request_id", "task", "dirty"});
        else if (op == "release" || op == "recover" || op == "reload")
            fields.insert({"request_id", "task"});
        else if (op == "restore")
            fields.insert({"request_id", "task", "restore_id"});
        else if (op == "status")
            fields.insert("task");
        else if (op == "operation" || op == "cancel")
            fields.insert("operation_id");
        else if (op == "list")
            fields.insert("offset");
        else
            throw Error("unknown operation");
        for (const auto &[k, v] : q.items()) {
            static_cast<void>(v);
            check(fields.contains(k), "unrecognized request field");
        }
        if (op == "status") {
            auto task = identifier(q.at("task"));
            auto l = layout(task);
            json active = json::array();
            {
                std::lock_guard guard(mutex);
                for (const auto &[id, j] : jobs)
                    if (j->record["request"].value("task", "") == task &&
                        (j->record["status"] == "running" || j->record["status"] == "queued"))
                        active.push_back(id);
            }
            return {{"ok", true},
                    {"workspace", tracked(l)},
                    {"active_operations", active},
                    {"mounted", is_mounted(l["descriptor"]["source"].get<std::string>())},
                    {"namespace_required", true},
                    {"instance_id", instance}};
        }
        std::lock_guard guard(mutex);
        if (op == "list") {
            check(!q.contains("offset") ||
                      (q["offset"].is_number_unsigned() ||
                       (q["offset"].is_number_integer() && q["offset"].get<long>() >= 0)),
                  "invalid offset");
            size_t offset = q.value("offset", size_t(0));
            check(offset <= maximum_records, "offset exceeds capacity");
            std::vector<fs::path> paths;
            for (const auto &e : fs::directory_iterator(state / "tasks"))
                if (e.path().extension() == ".json")
                    paths.push_back(e.path());
            std::sort(paths.begin(), paths.end());
            json rows = json::array();
            size_t end = std::min(paths.size(), offset + 100);
            for (size_t i = offset; i < end; ++i) {
                auto t = tracked(json::parse(read_text(paths[i])));
                json row;
                for (const auto *key :
                     {"task", "project", "phase", "tracked", "loaded", "recovery_required"})
                    row[key] = t[key];
                rows.push_back(row);
            }
            return {{"ok", true},
                    {"workspaces", rows},
                    {"total", paths.size()},
                    {"next_offset", end < paths.size() ? json(end) : json(nullptr)}};
        }
        if (op == "operation" || op == "cancel") {
            auto id = identifier(q.at("operation_id"));
            check(jobs.contains(id), "operation not found");
            auto j = jobs.at(id);
            if (op == "cancel" &&
                (j->record["status"] == "running" || j->record["status"] == "queued")) {
                j->cancelled = true;
                j->record["cancel_requested"] = true;
                if (j->pid > 0)
                    kill(j->pid, SIGTERM);
                save(*j);
            }
            auto response = j->record;
            response["ok"] = true;
            return response;
        }
        auto id = identifier(q.at("request_id"));
        auto task = identifier(q.at("task"));
        if (jobs.contains(id)) {
            check(jobs.at(id)->record["request"] == q,
                  "request ID reused with different arguments");
            auto r = jobs.at(id)->record;
            r["ok"] = true;
            return r;
        }
        for (const auto &[key, j] : jobs) {
            static_cast<void>(key);
            check(j->record["status"] != "running" && j->record["status"] != "queued",
                  "another operation is active; retry with same request ID");
        }
        check(jobs.size() < maximum_records, "operation history full; archive through host owner");
        if (op == "prepare") {
            auto p = identifier(q.at("project"));
            check(registry.at("projects").contains(p), "project not registered");
        }
        if (op == "run")
            check(config["commands"].contains(identifier(q.at("command"))), "command not approved");
        if (op == "restore")
            identifier(q.at("restore_id"));
        if (q.contains("dirty"))
            check(q["dirty"].is_boolean(), "dirty must be boolean");
        validate_controls();
        auto j = std::make_shared<Job>();
        j->deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(static_cast<int64_t>(maximum_seconds * 1000));
        j->record = {{"operation_id", id},      {"request", q},
                     {"status", "queued"},      {"created_at", now()},
                     {"instance_id", instance}, {"namespace_reaped", false}};
        j->record["server_identity"] = server_identity;
        save(*j);
        jobs[id] = j;
        threads.emplace_back([this, j] { job_main(j); });
        return {{"ok", true}, {"operation_id", id}, {"status", "queued"}};
    }
    void serve(int inherited) {
        server_identity = process_identity(read_text(host_path("self/stat")));
        auto boot = read_text(host_path("sys/kernel/random/boot_id"));
        if (!boot.empty() && boot.back() == '\n')
            boot.pop_back();
        server_identity["boot"] = boot;
        server_identity["instance_id"] = instance;
        atomic_json(state / "server.json", server_identity);
        FD socket;
        if (inherited >= 0) {
            socket.value = inherited;
            system_check(fcntl(socket.value, F_SETFD, FD_CLOEXEC) == 0,
                         "protect activation descriptor");
        } else {
            no_links(socket_path);
            directory(fs::path(socket_path).parent_path());
            if (fs::exists(socket_path) || fs::is_socket(socket_path)) {
                struct stat st{};
                system_check(lstat(socket_path.c_str(), &st) == 0, "stat existing socket");
                check(S_ISSOCK(st.st_mode) && st.st_uid == getuid(),
                      "refuse to replace non-owned socket");
                FD probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
                auto a = address(socket_path);
                check(connect(probe.value, reinterpret_cast<sockaddr *>(&a), sizeof a) < 0 &&
                          errno == ECONNREFUSED,
                      "socket already active");
                unlink(socket_path.c_str());
            }
            socket.value = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            system_check(socket.value >= 0, "listen socket");
            auto a = address(socket_path);
            system_check(bind(socket.value, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0,
                         "bind socket");
            struct stat created{};
            system_check(lstat(socket_path.c_str(), &created) == 0, "stat bound socket");
            socket_device = created.st_dev;
            socket_inode = created.st_ino;
            owns_socket = true;
            system_check(chmod(socket_path.c_str(), 0600) == 0, "protect socket");
            system_check(listen(socket.value, 16) == 0, "listen");
        }
        std::cout << json({{"ready", true},
                           {"socket", socket_path},
                           {"instance_id", instance},
                           {"pid_namespace", true}})
                         .dump()
                  << std::endl;
        while (!stopping) {
            pollfd p{socket.value, POLLIN, 0};
            auto n = poll(&p, 1, 100);
            if (n < 0 && errno == EINTR)
                continue;
            system_check(n >= 0, "poll socket");
            if (!n)
                continue;
            FD client(accept4(socket.value, nullptr, nullptr, SOCK_CLOEXEC));
            if (client.value < 0 && errno == EINTR)
                continue;
            system_check(client.value >= 0, "accept");
            timeval tv{2, 0};
            setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            try {
                ucred peer{};
                socklen_t size = sizeof peer;
                system_check(getsockopt(client.value, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0,
                             "peer identity");
                check(peer.uid == config["allowed_uid"].get<uid_t>(),
                      "caller UID is not authorized");
                auto r = handle(receive(client.value));
                check(r.dump().size() <= max_message, "response too large");
                write_all(client.value, r.dump() + "\n");
            } catch (const std::exception &e) {
                try {
                    write_all(client.value, json({{"ok", false},
                                                  {"error", std::string(e.what()).substr(0, 2048)}})
                                                    .dump() +
                                                "\n");
                } catch (...) {
                }
            }
        }
        {
            std::lock_guard guard(mutex);
            for (auto &[id, j] : jobs) {
                static_cast<void>(id);
                j->cancelled = true;
                if (j->pid > 0)
                    kill(j->pid, SIGTERM);
            }
        }
        threads.clear();
        // Namespace exit removes mounts; durable uppers/seeds/checkpoints remain.
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        umask(0077);
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "workspace-daemon --config ABSOLUTE_CONFIG\nRuns as an ordinary host "
                         "account in private user/mount/PID namespaces.\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << "workspace-daemon 0.1.0\n";
            return 0;
        }
        check(argc == 3 && std::string(argv[1]) == "--config",
              "usage: workspace-daemon --config ABSOLUTE_CONFIG");
        fs::path file = argv[2];
        trusted(file);
        install_signals();
        int inherited = -1;
        if (auto count = getenv("LISTEN_FDS")) {
            check(std::string(count) == "1", "one activation socket required");
            auto pid = getenv("LISTEN_PID");
            check(pid && std::stol(pid) == getpid(), "activation PID differs");
            inherited = 3;
        }
        // Validate ownership before user namespace UID translation; validate again inside.
        Server server(file);
        auto status = namespace_supervisor();
        if (status >= 0)
            return status;
        server.serve(inherited);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "workspace-daemon: " << e.what() << '\n';
        return 1;
    }
}
