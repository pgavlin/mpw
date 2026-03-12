#include "jsonrpc.h"

namespace lsp {

Request parseRequest(const std::string &body) {
    Request req;
    json j = json::parse(body);

    req.method = j.value("method", "");

    if (j.count("id"))
        req.id = j["id"];

    if (j.count("params"))
        req.params = j["params"];

    return req;
}

json Response::toJson() const {
    json j;
    j["jsonrpc"] = "2.0";
    j["id"] = id;
    if (!error.is_null())
        j["error"] = error;
    else
        j["result"] = result;
    return j;
}

Response makeResult(const json &id, const json &result) {
    Response r;
    r.id = id;
    r.result = result;
    return r;
}

Response makeError(const json &id, int code, const std::string &message) {
    Response r;
    r.id = id;
    r.error = {{"code", code}, {"message", message}};
    return r;
}

} // namespace lsp
