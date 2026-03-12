#ifndef LSP_TRANSPORT_H
#define LSP_TRANSPORT_H

#include <string>

namespace lsp {

// Read one LSP message from stdin. Returns the JSON body.
// Returns empty string on EOF.
std::string readMessage();

// Write one LSP message to stdout with Content-Length framing.
void writeMessage(const std::string &body);

// Log a message to stderr.
void log(const std::string &msg);

} // namespace lsp

#endif
