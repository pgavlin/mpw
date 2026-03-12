#include "document_store.h"

#include <cstdio>

namespace lsp {

void DocumentStore::open(const std::string &uri, const std::string &text, int version) {
    Document doc;
    doc.uri = uri;
    doc.text = text;
    doc.version = version;
    docs_[uri] = doc;
}

void DocumentStore::change(const std::string &uri, const std::string &text, int version) {
    std::map<std::string, Document>::iterator it = docs_.find(uri);
    if (it != docs_.end()) {
        it->second.text = text;
        it->second.version = version;
    }
}

void DocumentStore::close(const std::string &uri) {
    docs_.erase(uri);
}

const Document *DocumentStore::get(const std::string &uri) const {
    std::map<std::string, Document>::const_iterator it = docs_.find(uri);
    if (it != docs_.end())
        return &it->second;
    return NULL;
}

std::string DocumentStore::uriToPath(const std::string &uri) {
    // Strip file:// prefix.
    const std::string prefix = "file://";
    if (uri.compare(0, prefix.size(), prefix) == 0) {
        std::string path = uri.substr(prefix.size());
        // Decode percent-encoded characters.
        std::string decoded;
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '%' && i + 2 < path.size()) {
                char hex[3] = {path[i + 1], path[i + 2], '\0'};
                char c = (char)strtol(hex, NULL, 16);
                decoded += c;
                i += 2;
            } else {
                decoded += path[i];
            }
        }
        return decoded;
    }
    return uri;
}

} // namespace lsp
