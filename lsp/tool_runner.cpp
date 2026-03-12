#include "tool_runner.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lsp {

std::string toolForExtension(const std::string &ext) {
    if (ext == ".c") return "SC";
    if (ext == ".cp" || ext == ".cpp" || ext == ".cc") return "SCpp";
    if (ext == ".a" || ext == ".s") return "Asm";
    if (ext == ".r") return "Rez";
    return std::string();
}

std::string getExtension(const std::string &path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos)
        return std::string();
    std::string ext = path.substr(dot);
    // Lowercase.
    for (size_t i = 0; i < ext.size(); ++i)
        ext[i] = (char)std::tolower((unsigned char)ext[i]);
    return ext;
}

static std::string readAll(int fd) {
    std::string result;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        result.append(buf, n);
    }
    return result;
}

ToolResult runTool(const std::string &mpwPath,
                   const std::string &mpwRoot,
                   const std::string &toolName,
                   const std::vector<std::string> &flags,
                   const std::string &filePath,
                   int timeoutSeconds) {
    ToolResult result;
    result.exitCode = -1;

    // Create pipes for stdout and stderr.
    int outPipe[2], errPipe[2];
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        result.stderrOutput = "Failed to create pipes";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.stderrOutput = "fork() failed";
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        return result;
    }

    if (pid == 0) {
        // Child process.
        close(outPipe[0]);
        close(errPipe[0]);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[1]);
        close(errPipe[1]);

        // Close stdin so the tool doesn't hang reading.
        close(STDIN_FILENO);

        // Set MPW environment variable.
        if (!mpwRoot.empty())
            setenv("MPW", mpwRoot.c_str(), 1);

        // Build argv: mpw <tool> [-c] [flags...] <file>
        std::vector<const char *> argv;
        argv.push_back(mpwPath.c_str());
        argv.push_back(toolName.c_str());

        // Add -c for compilers (not Rez).
        bool isCompiler = (toolName == "SC" || toolName == "SCpp" ||
                           toolName == "Asm");
        if (isCompiler)
            argv.push_back("-c");

        for (size_t i = 0; i < flags.size(); ++i)
            argv.push_back(flags[i].c_str());

        argv.push_back(filePath.c_str());

        // For compilers, direct object output to /dev/null.
        std::string objFlag;
        if (isCompiler) {
            objFlag = "-o /dev/null";
            argv.push_back("-o");
            argv.push_back("/dev/null");
        }

        argv.push_back(NULL);

        execvp(argv[0], const_cast<char *const *>(&argv[0]));
        _exit(127);
    }

    // Parent process.
    close(outPipe[1]);
    close(errPipe[1]);

    result.stdoutOutput = readAll(outPipe[0]);
    result.stderrOutput = readAll(errPipe[0]);
    close(outPipe[0]);
    close(errPipe[0]);

    // Wait for child with timeout.
    if (timeoutSeconds > 0) {
        int elapsed = 0;
        int status;
        while (elapsed < timeoutSeconds) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                return result;
            }
            if (w < 0) {
                result.exitCode = -1;
                return result;
            }
            usleep(100000); // 100ms
            elapsed++;
            if (elapsed >= timeoutSeconds) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                result.stderrOutput += "\n[mpw-lsp] Tool execution timed out";
                result.exitCode = -1;
                return result;
            }
        }
    }

    int status;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

} // namespace lsp
