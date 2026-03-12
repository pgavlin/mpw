#ifndef LSP_JSONRPC_H
#define LSP_JSONRPC_H

#include <nlohmann/json.hpp>
#include <string>

namespace lsp {

using json = nlohmann::json;

struct Request {
    std::string method;
    json id;     // null for notifications
    json params; // may be null

    bool isNotification() const { return id.is_null(); }
};

struct Response {
    json id;
    json result; // set on success
    json error;  // set on error

    json toJson() const;
};

// Standard JSON-RPC error codes.
enum ErrorCode {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
};

// Parse a JSON-RPC request from a JSON body string.
Request parseRequest(const std::string &body);

// Create a success response.
Response makeResult(const json &id, const json &result);

// Create an error response.
Response makeError(const json &id, int code, const std::string &message);

} // namespace lsp

#endif
