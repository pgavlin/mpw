#include "server.h"
#include "diagnostic_parser.h"
#include "tool_runner.h"
#include "transport.h"

#include <cstdlib>

namespace lsp {

Server::Server()
    : initialized_(false), shutdown_(false) {
    // Defaults from environment.
    const char *mpw = std::getenv("MPW");
    if (mpw) mpwRoot_ = mpw;
    mpwPath_ = "mpw"; // Assume on $PATH.
}

int Server::run() {
    for (;;) {
        std::string body = readMessage();
        if (body.empty())
            return shutdown_ ? 0 : 1;

        Request req;
        try {
            req = parseRequest(body);
        } catch (...) {
            sendResponse(makeError(json(), ParseError, "Parse error"));
            continue;
        }

        log("Received: " + req.method);

        if (req.method == "initialize")
            handleInitialize(req);
        else if (req.method == "initialized")
            handleInitialized(req);
        else if (req.method == "shutdown")
            handleShutdown(req);
        else if (req.method == "exit")
            handleExit(req);
        else if (req.method == "textDocument/didOpen")
            handleDidOpen(req);
        else if (req.method == "textDocument/didChange")
            handleDidChange(req);
        else if (req.method == "textDocument/didSave")
            handleDidSave(req);
        else if (req.method == "textDocument/didClose")
            handleDidClose(req);
        else if (!req.isNotification()) {
            sendResponse(makeError(req.id, MethodNotFound,
                                   "Method not found: " + req.method));
        }
    }
}

void Server::handleInitialize(const Request &req) {
    // Read configuration from initializationOptions.
    if (req.params.count("initializationOptions")) {
        const json &opts = req.params["initializationOptions"];
        if (opts.count("mpwPath"))
            mpwPath_ = opts["mpwPath"].get<std::string>();
        if (opts.count("mpwRoot"))
            mpwRoot_ = opts["mpwRoot"].get<std::string>();
        if (opts.count("toolFlags")) {
            const json &tf = opts["toolFlags"];
            for (json::const_iterator it = tf.begin(); it != tf.end(); ++it) {
                std::vector<std::string> flags;
                for (size_t i = 0; i < it.value().size(); ++i)
                    flags.push_back(it.value()[i].get<std::string>());
                toolFlags_[it.key()] = flags;
            }
        }
    }

    json capabilities;
    // Full document sync + save notifications.
    capabilities["textDocumentSync"] = {
        {"openClose", true},
        {"change", 1}, // Full sync.
        {"save", {{"includeText", false}}},
    };

    json result;
    result["capabilities"] = capabilities;
    result["serverInfo"] = {
        {"name", "mpw-lsp"},
        {"version", "0.1.0"},
    };

    sendResponse(makeResult(req.id, result));
    log("Initialized");
}

void Server::handleInitialized(const Request &) {
    initialized_ = true;
}

void Server::handleShutdown(const Request &req) {
    shutdown_ = true;
    sendResponse(makeResult(req.id, json()));
}

void Server::handleExit(const Request &) {
    std::exit(shutdown_ ? 0 : 1);
}

void Server::handleDidOpen(const Request &req) {
    const json &td = req.params["textDocument"];
    std::string uri = td["uri"].get<std::string>();
    std::string text = td["text"].get<std::string>();
    int version = td.value("version", 0);
    store_.open(uri, text, version);
    runDiagnostics(uri);
}

void Server::handleDidChange(const Request &req) {
    const json &td = req.params["textDocument"];
    std::string uri = td["uri"].get<std::string>();
    int version = td.value("version", 0);
    // Full sync: take the last content change.
    const json &changes = req.params["contentChanges"];
    if (!changes.empty()) {
        std::string text = changes[changes.size() - 1]["text"].get<std::string>();
        store_.change(uri, text, version);
    }
}

void Server::handleDidSave(const Request &req) {
    const json &td = req.params["textDocument"];
    std::string uri = td["uri"].get<std::string>();
    runDiagnostics(uri);
}

void Server::handleDidClose(const Request &req) {
    const json &td = req.params["textDocument"];
    std::string uri = td["uri"].get<std::string>();
    // Clear diagnostics.
    publishDiagnostics(uri, std::vector<json>());
    store_.close(uri);
}

void Server::runDiagnostics(const std::string &uri) {
    std::string path = DocumentStore::uriToPath(uri);
    std::string ext = getExtension(path);
    std::string toolName = toolForExtension(ext);

    if (toolName.empty()) {
        // No tool for this file type.
        return;
    }

    // Get per-tool flags.
    std::vector<std::string> flags;
    std::map<std::string, std::vector<std::string> >::const_iterator it =
        toolFlags_.find(toolName);
    if (it != toolFlags_.end())
        flags = it->second;

    log("Running " + toolName + " on " + path);

    ToolResult result = runTool(mpwPath_, mpwRoot_, toolName, flags, path, 30);

    // Parse diagnostics from stderr.
    std::vector<Diagnostic> parsed = parseDiagnostics(toolName, result.stderrOutput);

    // Convert to LSP diagnostics.
    std::vector<json> lspDiags;
    for (size_t i = 0; i < parsed.size(); ++i) {
        const Diagnostic &d = parsed[i];
        json diag;
        diag["range"] = {
            {"start", {{"line", d.line}, {"character", d.column}}},
            {"end", {{"line", d.line}, {"character", d.column}}},
        };
        diag["severity"] = d.severity;
        diag["source"] = toolName;
        diag["message"] = d.message;
        lspDiags.push_back(diag);
    }

    // If the tool failed but we got no parsed diagnostics, report the error.
    if (result.exitCode != 0 && lspDiags.empty() &&
        !result.stderrOutput.empty()) {
        json diag;
        diag["range"] = {
            {"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 0}}},
        };
        diag["severity"] = 1;
        diag["source"] = toolName;
        diag["message"] = result.stderrOutput;
        lspDiags.push_back(diag);
    }

    publishDiagnostics(uri, lspDiags);
}

void Server::publishDiagnostics(const std::string &uri,
                                const std::vector<json> &diagnostics) {
    json params;
    params["uri"] = uri;
    params["diagnostics"] = diagnostics;
    sendNotification("textDocument/publishDiagnostics", params);
}

void Server::sendResponse(const Response &resp) {
    writeMessage(resp.toJson().dump());
}

void Server::sendNotification(const std::string &method, const json &params) {
    json msg;
    msg["jsonrpc"] = "2.0";
    msg["method"] = method;
    msg["params"] = params;
    writeMessage(msg.dump());
}

} // namespace lsp
