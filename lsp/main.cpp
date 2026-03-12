#include "server.h"
#include "transport.h"

int main() {
    lsp::log("mpw-lsp starting");
    lsp::Server server;
    return server.run();
}
