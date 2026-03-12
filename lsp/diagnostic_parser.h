#ifndef LSP_DIAGNOSTIC_PARSER_H
#define LSP_DIAGNOSTIC_PARSER_H

#include <string>
#include <vector>

namespace lsp {

struct Diagnostic {
    std::string file;   // Filesystem path (Unix).
    int line;           // 0-based line number.
    int column;         // 0-based column number.
    int severity;       // 1=Error, 2=Warning, 3=Information, 4=Hint.
    std::string message;
};

// Convert a Mac colon-separated path to a Unix path.
std::string macToUnixPath(const std::string &macPath);

// Parse SC/SCpp compiler stderr output into diagnostics.
std::vector<Diagnostic> parseSCDiagnostics(const std::string &output);

// Parse Asm assembler stderr output into diagnostics.
std::vector<Diagnostic> parseAsmDiagnostics(const std::string &output);

// Parse Rez resource compiler stderr output into diagnostics.
std::vector<Diagnostic> parseRezDiagnostics(const std::string &output);

// Parse diagnostics using the appropriate parser for the given tool.
std::vector<Diagnostic> parseDiagnostics(const std::string &toolName,
                                         const std::string &output);

} // namespace lsp

#endif
