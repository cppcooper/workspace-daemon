#include "common.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace wd;
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout
                << "workspacectl [--socket PATH] [--request-id ID] [--wait]\n  prepare PROJECT "
                   "TASK\n  run TASK APPROVED_COMMAND\n  status TASK | operation ID | cancel ID\n  "
                   "checkpoint TASK [--dirty] | release TASK | reload TASK | recover TASK | "
                   "restore TASK RESTORE_ID | list [OFFSET]\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << "workspacectl 0.1.0\n";
            return 0;
        }
        std::string socket;
        if (auto v = getenv("WORKSPACE_DAEMON_SOCKET"))
            socket = v;
        std::vector<std::string> args;
        bool wait = false, dirty = false;
        std::string request_id;
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--socket" || a == "--request-id") {
                check(i + 1 < argc, "option requires value");
                (a == "--socket" ? socket : request_id) = argv[++i];
            } else if (a == "--wait")
                wait = true;
            else if (a == "--dirty")
                dirty = true;
            else
                args.push_back(a);
        }
        if (socket.empty()) {
            auto v = getenv("XDG_RUNTIME_DIR");
            check(v, "set WORKSPACE_DAEMON_SOCKET or --socket");
            socket = std::string(v) + "/workspace-daemon.sock";
        }
        check(
            !args.empty(),
            "usage: workspacectl [--socket PATH] prepare PROJECT TASK | run TASK COMMAND | status "
            "TASK | operation ID | cancel ID | checkpoint TASK [--dirty] | release TASK [--wait]");
        auto op = args[0];
        json q = {{"op", op}};
        if (op == "prepare") {
            check(args.size() == 3, "prepare PROJECT TASK");
            q["project"] = args[1];
            q["task"] = args[2];
        } else if (op == "run") {
            check(args.size() == 3, "run TASK APPROVED_COMMAND");
            q["task"] = args[1];
            q["command"] = args[2];
        } else if (op == "status" || op == "checkpoint" || op == "release" || op == "recover" ||
                   op == "reload") {
            check(args.size() == 2, "operation requires TASK");
            q["task"] = args[1];
        } else if (op == "restore") {
            check(args.size() == 3, "restore TASK RESTORE_ID");
            q["task"] = args[1];
            q["restore_id"] = args[2];
        } else if (op == "operation" || op == "cancel") {
            check(args.size() == 2, "operation requires ID");
            q["operation_id"] = args[1];
        } else {
            check(op == "list" && args.size() <= 2, "unknown operation");
            if (args.size() == 2) {
                size_t consumed = 0;
                auto offset = std::stoull(args[1], &consumed);
                check(consumed == args[1].size(), "invalid offset");
                q["offset"] = offset;
            }
        }
        bool mutation = op == "prepare" || op == "run" || op == "checkpoint" || op == "release" ||
                        op == "recover" || op == "reload" || op == "restore";
        if (mutation)
            q["request_id"] = request_id.empty() ? random_id() : request_id;
        if (dirty) {
            check(op == "checkpoint", "--dirty requires checkpoint");
            q["dirty"] = true;
        }
        if (mutation)
            std::cerr << "request_id=" << q["request_id"].get<std::string>()
                      << " (reuse --request-id after an uncertain response)\n";
        json r = rpc(socket, q);
        if (wait && r.value("ok", false) && r.contains("operation_id")) {
            auto id = r["operation_id"];
            auto end = std::chrono::steady_clock::now() + std::chrono::minutes(5);
            do {
                r = rpc(socket, {{"op", "operation"}, {"operation_id", id}});
                auto state = r.value("status", "");
                if (state != "queued" && state != "running")
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } while (std::chrono::steady_clock::now() < end);
            check(r.value("status", "") != "running" && r.value("status", "") != "queued",
                  "client wait expired; operation remains owned, inspect/cancel by request ID");
        }
        std::cout << r.dump(2) << '\n';
        return r.value("ok", false) && r.value("status", "") != "failed" &&
                       (r.value("status", "") != "cancelled" || op == "cancel") &&
                       r.value("status", "") != "interrupted"
                   ? 0
                   : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
