#ifndef __mpw_pef_loader_h__
#define __mpw_pef_loader_h__

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace PEFLoader {

struct SectionInfo {
	uint32_t address;       // allocated emulated memory address
	uint32_t size;          // total size in memory
	uint8_t  kind;
	std::string name;
};

struct ExportedSymbolInfo {
	std::string name;
	uint32_t sectionIndex;
	uint32_t offset;        // offset within section
	uint8_t  symbolClass;
};

struct ImportedSymbolInfo {
	std::string library;
	std::string symbol;
	uint8_t symbolClass;
	uint32_t resolvedAddress;
};

struct LoadResult {
	uint32_t entryPoint;    // main entry TVector address (0 if none)
	uint32_t initPoint;     // init entry TVector address (0 if none)
	uint32_t termPoint;     // term entry TVector address (0 if none)
	uint32_t tocBase;       // TOC base for r2
	std::vector<SectionInfo> sections;
	std::vector<ImportedSymbolInfo> imports;
	std::vector<ExportedSymbolInfo> exports;
};

// Callback for resolving imported symbols.
// Returns the address to patch (typically a TVector address for code/tvector
// symbols, or a data address for data symbols). Returns 0 if unresolved.
using ImportResolver = std::function<uint32_t(
	const std::string &library,
	const std::string &symbol,
	uint8_t symbolClass)>;

// Returns true if the given data begins with a PEF container header.
bool IsPEF(const uint8_t *data, size_t size);

// Load a PEF from a memory buffer.
bool LoadPEF(const uint8_t *data, size_t size,
             ImportResolver resolver, LoadResult &result);

// Load a PEF file from disk.
bool LoadPEFFile(const std::string &path,
                 ImportResolver resolver, LoadResult &result);

// Look up an exported symbol's absolute address in a loaded fragment.
// Returns 0 if not found.
uint32_t FindExport(const LoadResult &result, const std::string &name);

// Enable/disable verbose trace logging to stderr.
void SetTrace(bool trace);

}

#endif
