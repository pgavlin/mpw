#include "transport.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lsp {

std::string readMessage() {
    // Read headers until empty line.
    int contentLength = -1;
    for (;;) {
        char buf[256];
        if (!std::fgets(buf, sizeof(buf), stdin))
            return std::string();

        // Strip trailing \r\n
        size_t len = std::strlen(buf);
        while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n'))
            buf[--len] = '\0';

        if (len == 0)
            break; // End of headers.

        if (std::strncmp(buf, "Content-Length:", 15) == 0) {
            contentLength = std::atoi(buf + 15);
        }
        // Ignore other headers.
    }

    if (contentLength <= 0)
        return std::string();

    std::string body(contentLength, '\0');
    size_t read = std::fread(&body[0], 1, contentLength, stdin);
    if ((int)read != contentLength)
        return std::string();

    return body;
}

void writeMessage(const std::string &body) {
    std::fprintf(stdout, "Content-Length: %d\r\n\r\n", (int)body.size());
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fflush(stdout);
}

void log(const std::string &msg) {
    std::fprintf(stderr, "[mpw-lsp] %s\n", msg.c_str());
}

} // namespace lsp
