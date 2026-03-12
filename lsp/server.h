#ifndef LSP_SERVER_H
#define LSP_SERVER_H

#include "document_store.h"
#include "jsonrpc.h"

#include <map>
#include <string>
#include <vector>

namespace lsp {

class Server {
public:
    Server();

    // Run the main loop. Returns process exit code.
    int run();

private:
    void handleInitialize(const Request &req);
    void handleInitialized(const Request &req);
    void handleShutdown(const Request &req);
    void handleExit(const Request &req);
    void handleDidOpen(const Request &req);
    void handleDidChange(const Request &req);
    void handleDidSave(const Request &req);
    void handleDidClose(const Request &req);

    void runDiagnostics(const std::string &uri);
    void publishDiagnostics(const std::string &uri,
                            const std::vector<json> &diagnostics);
    void sendResponse(const Response &resp);
    void sendNotification(const std::string &method, const json &params);

    DocumentStore store_;
    std::string mpwPath_;
    std::string mpwRoot_;
    std::map<std::string, std::vector<std::string> > toolFlags_;
    bool initialized_;
    bool shutdown_;
};

} // namespace lsp

#endif
