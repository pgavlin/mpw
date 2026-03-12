#ifndef LSP_DOCUMENT_STORE_H
#define LSP_DOCUMENT_STORE_H

#include <map>
#include <string>

namespace lsp {

struct Document {
    std::string uri;
    std::string text;
    int version;
};

class DocumentStore {
public:
    void open(const std::string &uri, const std::string &text, int version);
    void change(const std::string &uri, const std::string &text, int version);
    void close(const std::string &uri);

    const Document *get(const std::string &uri) const;

    // Convert a file:// URI to a filesystem path.
    static std::string uriToPath(const std::string &uri);

private:
    std::map<std::string, Document> docs_;
};

} // namespace lsp

#endif
