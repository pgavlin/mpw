#ifndef LSP_TOOL_RUNNER_H
#define LSP_TOOL_RUNNER_H

#include <string>
#include <vector>

namespace lsp {

struct ToolResult {
    int exitCode;
    std::string stdoutOutput;
    std::string stderrOutput;
};

struct ToolConfig {
    std::string mpwPath;   // Path to mpw binary.
    std::string mpwRoot;   // MPW environment root ($MPW).
    std::vector<std::string> extraFlags; // Per-tool flags.
};

// Determine the MPW tool name from a file extension.
// Returns empty string if the extension is not recognized.
std::string toolForExtension(const std::string &ext);

// Get the lowercase file extension from a path (including the dot).
std::string getExtension(const std::string &path);

// Run an MPW tool on the given file. Returns captured output and exit code.
// Timeout is in seconds; 0 means no timeout.
ToolResult runTool(const std::string &mpwPath,
                   const std::string &mpwRoot,
                   const std::string &toolName,
                   const std::vector<std::string> &flags,
                   const std::string &filePath,
                   int timeoutSeconds = 30);

} // namespace lsp

#endif
