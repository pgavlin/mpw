#include "diagnostic_parser.h"

#include <cstdlib>
#include <sstream>

namespace lsp {

std::string macToUnixPath(const std::string &macPath) {
    // If it contains colons but no slashes, treat as Mac path.
    if (macPath.find(':') != std::string::npos &&
        macPath.find('/') == std::string::npos) {
        std::string result;
        for (size_t i = 0; i < macPath.size(); ++i) {
            if (macPath[i] == ':')
                result += '/';
            else
                result += macPath[i];
        }
        // A leading colon means relative path (no leading slash).
        // No leading colon means volume-rooted — keep the leading slash.
        if (!result.empty() && result[0] == '/')
            result = result.substr(1); // relative
        return result;
    }
    return macPath;
}

static std::vector<std::string> splitLines(const std::string &s) {
    std::vector<std::string> lines;
    std::istringstream stream(s);
    std::string line;
    while (std::getline(stream, line)) {
        // Strip trailing \r.
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);
        lines.push_back(line);
    }
    return lines;
}

// Try to extract column from a caret line (all spaces/tabs then ^).
static int caretColumn(const std::string &line) {
    size_t pos = line.find('^');
    if (pos != std::string::npos) {
        // Check that everything before ^ is whitespace.
        for (size_t i = 0; i < pos; ++i) {
            if (line[i] != ' ' && line[i] != '\t')
                return -1;
        }
        return (int)pos;
    }
    return -1;
}

// Parse: File "filename"; line 12 #Error: message
// Also handles: File "filename"; line 12 #Warning: message
static bool parseSCFileLine(const std::string &line,
                            std::string &file, int &lineNum,
                            int &severity, std::string &message) {
    // Look for 'File "'
    size_t fileStart = line.find("File \"");
    if (fileStart == std::string::npos)
        return false;
    fileStart += 6; // skip 'File "'

    size_t fileEnd = line.find('"', fileStart);
    if (fileEnd == std::string::npos)
        return false;

    file = line.substr(fileStart, fileEnd - fileStart);

    // Look for '; line '
    size_t linePos = line.find("; line ", fileEnd);
    if (linePos == std::string::npos)
        return false;
    linePos += 7; // skip '; line '

    lineNum = std::atoi(line.c_str() + linePos);

    // Look for '#Error:' or '#Warning:'
    size_t hashPos = line.find('#', linePos);
    if (hashPos == std::string::npos)
        return false;

    if (line.compare(hashPos, 7, "#Error:") == 0) {
        severity = 1;
        message = line.substr(hashPos + 7);
    } else if (line.compare(hashPos, 9, "#Warning:") == 0) {
        severity = 2;
        message = line.substr(hashPos + 9);
    } else {
        severity = 1;
        size_t colonPos = line.find(':', hashPos);
        if (colonPos != std::string::npos)
            message = line.substr(colonPos + 1);
        else
            message = line.substr(hashPos + 1);
    }

    // Trim leading space from message.
    if (!message.empty() && message[0] == ' ')
        message = message.substr(1);

    return true;
}

std::vector<Diagnostic> parseSCDiagnostics(const std::string &output) {
    std::vector<Diagnostic> diags;
    std::vector<std::string> lines = splitLines(output);

    int lastCaretCol = -1;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];

        // Check for caret line.
        int col = caretColumn(line);
        if (col >= 0) {
            lastCaretCol = col;
            continue;
        }

        // Check for separator.
        if (line.find("#-----") != std::string::npos) {
            lastCaretCol = -1;
            continue;
        }

        // Try to parse a File/line diagnostic.
        std::string file;
        int lineNum;
        int severity;
        std::string message;
        if (parseSCFileLine(line, file, lineNum, severity, message)) {
            Diagnostic d;
            d.file = macToUnixPath(file);
            d.line = lineNum > 0 ? lineNum - 1 : 0; // 0-based
            d.column = lastCaretCol >= 0 ? lastCaretCol : 0;
            d.severity = severity;
            d.message = message;
            diags.push_back(d);
            lastCaretCol = -1;
        }
    }

    return diags;
}

std::vector<Diagnostic> parseAsmDiagnostics(const std::string &output) {
    std::vector<Diagnostic> diags;
    std::vector<std::string> lines = splitLines(output);

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];

        // Look for: ### Error <ID> ### message
        // or:       ### Warning <ID> ### message
        size_t pos = line.find("### ");
        if (pos == std::string::npos)
            continue;

        int severity = 0;
        size_t afterType = 0;
        if (line.find("### Error", pos) == pos) {
            severity = 1;
            afterType = pos + 9; // skip "### Error"
        } else if (line.find("### Warning", pos) == pos) {
            severity = 2;
            afterType = pos + 11; // skip "### Warning"
        } else {
            continue;
        }

        // Find the second ###.
        size_t msgStart = line.find("###", afterType);
        std::string message;
        if (msgStart != std::string::npos) {
            message = line.substr(msgStart + 3);
            if (!message.empty() && message[0] == ' ')
                message = message.substr(1);
        }

        // Next line should be: File "filename"; Line N
        if (i + 1 >= lines.size())
            continue;

        const std::string &fileLine = lines[i + 1];
        size_t fStart = fileLine.find("File \"");
        if (fStart == std::string::npos)
            continue;
        fStart += 6;

        size_t fEnd = fileLine.find('"', fStart);
        if (fEnd == std::string::npos)
            continue;

        std::string file = fileLine.substr(fStart, fEnd - fStart);

        // Note capital 'L' in Line for Asm.
        size_t lPos = fileLine.find("; Line ", fEnd);
        int lineNum = 0;
        if (lPos != std::string::npos)
            lineNum = std::atoi(fileLine.c_str() + lPos + 7);

        Diagnostic d;
        d.file = macToUnixPath(file);
        d.line = lineNum > 0 ? lineNum - 1 : 0;
        d.column = 0;
        d.severity = severity;
        d.message = message;
        diags.push_back(d);

        ++i; // Skip the file/line line.
    }

    return diags;
}

std::vector<Diagnostic> parseRezDiagnostics(const std::string &output) {
    std::vector<Diagnostic> diags;
    std::vector<std::string> lines = splitLines(output);

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];

        // Pattern: File "pathname"; Line N ### message
        size_t fStart = line.find("File \"");
        if (fStart == std::string::npos)
            continue;
        fStart += 6;

        size_t fEnd = line.find('"', fStart);
        if (fEnd == std::string::npos)
            continue;

        std::string file = line.substr(fStart, fEnd - fStart);

        size_t lPos = line.find("; Line ", fEnd);
        if (lPos == std::string::npos)
            continue;
        int lineNum = std::atoi(line.c_str() + lPos + 7);

        // Find ### for the message.
        size_t msgPos = line.find("###", lPos);
        std::string message;
        if (msgPos != std::string::npos) {
            message = line.substr(msgPos + 3);
            if (!message.empty() && message[0] == ' ')
                message = message.substr(1);
        }

        Diagnostic d;
        d.file = macToUnixPath(file);
        d.line = lineNum > 0 ? lineNum - 1 : 0;
        d.column = 0;
        d.severity = 1; // Rez errors are always errors.
        d.message = message;
        diags.push_back(d);
    }

    return diags;
}

std::vector<Diagnostic> parseDiagnostics(const std::string &toolName,
                                         const std::string &output) {
    if (toolName == "SC" || toolName == "SCpp" || toolName == "MrC")
        return parseSCDiagnostics(output);
    if (toolName == "Asm")
        return parseAsmDiagnostics(output);
    if (toolName == "Rez")
        return parseRezDiagnostics(output);
    return std::vector<Diagnostic>();
}

} // namespace lsp
