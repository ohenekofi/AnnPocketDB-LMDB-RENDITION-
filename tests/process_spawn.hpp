#pragma once
//
// AnnPocketDb tests :: process_spawn.hpp
//
// Minimal real multi-process spawn/wait helper, used only by
// test_mplay_fuzzer.cpp -- CreateProcess on Windows, fork+exec on POSIX.
// Deliberate design choice (explicitly requested over an in-process thread
// simulation): the fuzzer must exercise genuine cross-process contention on
// the .lock file's shared-memory reader table (see
// platform/interprocess_lock.hpp), which a thread-based simulation cannot.
//
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#endif

namespace mplay {

struct ChildProcess {
#if defined(_WIN32)
    HANDLE processHandle = nullptr;
    HANDLE threadHandle = nullptr;
#else
    pid_t pid = -1;
#endif
};

// Spawns `exePath` with `args` (argv[0] is exePath itself; `args` are the
// remaining argv entries). Returns false if the OS could not create the
// process at all (out is left default).
inline bool spawnProcess(const std::string& exePath, const std::vector<std::string>& args,
                         ChildProcess& out) {
#if defined(_WIN32)
    std::string cmdLine = "\"" + exePath + "\"";
    for (const auto& a : args) cmdLine += " \"" + a + "\"";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back('\0');
    // bInheritHandles=TRUE so the child's stdout/stderr merge into ours (and
    // therefore into ctest's captured output) -- diagnostic value (worker
    // errors/traces are otherwise silently invisible) outweighs the minor
    // extra handle inheritance for a short-lived test worker.
    BOOL ok = CreateProcessA(nullptr, cmdLineBuf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    out.processHandle = pi.hProcess;
    out.threadHandle = pi.hThread;
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exePath.c_str()));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(exePath.c_str(), argv.data());
        _exit(127);  // execv itself failed
    }
    out.pid = pid;
    return true;
#endif
}

// Hard-kills the child (TerminateProcess / SIGKILL) -- simulates a real OS
// crash, not a graceful shutdown (no destructors, no flush, no atexit
// handlers run in the child). Safe to call on an already-exited child (the
// signal/call is simply a no-op or fails harmlessly); the caller must still
// reap it via waitForExit afterward. Returns false only if the underlying
// OS call itself reports an error (not if the process had already exited).
inline bool killProcess(ChildProcess& child) {
#if defined(_WIN32)
    if (child.processHandle == nullptr) return false;
    return TerminateProcess(child.processHandle, 1) != 0;
#else
    if (child.pid < 0) return false;
    return kill(child.pid, SIGKILL) == 0;
#endif
}

// Waits for the child to exit and returns its exit code, or -1 on a wait
// failure (child handle/pid invalid, OS wait call itself failed).
inline int waitForExit(ChildProcess& child) {
#if defined(_WIN32)
    if (child.processHandle == nullptr) return -1;
    WaitForSingleObject(child.processHandle, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(child.processHandle, &code);
    CloseHandle(child.processHandle);
    CloseHandle(child.threadHandle);
    child.processHandle = nullptr;
    child.threadHandle = nullptr;
    return static_cast<int>(code);
#else
    if (child.pid < 0) return -1;
    int status = 0;
    if (waitpid(child.pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

}  // namespace mplay
