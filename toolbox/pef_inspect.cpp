/*
 * pef_inspect.cpp
 *
 * Interactive PEF inspector. Loads a PEF into emulated memory and provides
 * structured queries for debugging StdCLib init and PPC tool execution.
 */

#include <toolbox/pef_loader.h>
#include <toolbox/pef.h>
#include <toolbox/mm.h>

#include <cpu/m68k/defs.h>
#include <cpu/m68k/CpuModule.h>
#include <cpu/m68k/fmem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <iostream>
#include <sstream>

#include <capstone.h>

static uint8_t *Memory = nullptr;
static const uint32_t MemorySize = 16 * 1024 * 1024;
static const uint32_t kGlobalSize = 0x10000;
static const uint32_t kStackSize = 32 * 1024;

static PEFLoader::LoadResult result;
static std::map<uint32_t, std::string> addrToSymbol; // address → name
static csh cs;

static const char *sectionKindName(uint8_t kind) {
	switch (kind) {
	case PEF::kCode: return "code";
	case PEF::kUnpackedData: return "data";
	case PEF::kPatternInitData: return "pidata";
	case PEF::kConstant: return "const";
	case PEF::kLoader: return "loader";
	case PEF::kDebug: return "debug";
	default: return "?";
	}
}

static const char *symClassName(uint8_t cls) {
	switch (cls & 0x0F) {
	case PEF::kCodeSymbol: return "code";
	case PEF::kDataSymbol: return "data";
	case PEF::kTVectorSymbol: return "tvector";
	case PEF::kTOCSymbol: return "toc";
	case PEF::kGlueSymbol: return "glue";
	default: return "?";
	}
}

// Find which section contains an address
static int findSection(uint32_t addr) {
	for (size_t i = 0; i < result.sections.size(); i++) {
		auto &s = result.sections[i];
		if (s.address && addr >= s.address && addr < s.address + s.size)
			return (int)i;
	}
	return -1;
}

static std::string describeAddr(uint32_t addr) {
	// Check symbol table
	auto it = addrToSymbol.find(addr);
	if (it != addrToSymbol.end())
		return it->second;

	// Check section
	int sec = findSection(addr);
	if (sec >= 0) {
		char buf[64];
		snprintf(buf, sizeof(buf), "section %d (%s) + 0x%X",
		         sec, sectionKindName(result.sections[sec].kind),
		         addr - result.sections[sec].address);
		return buf;
	}

	return "";
}

static uint32_t parseAddr(const std::string &s) {
	// Try as hex number (with or without 0x prefix)
	if (s.size() > 0) {
		char *end;
		uint32_t val = strtoul(s.c_str(), &end, 0);
		if (*end == '\0') return val;
	}

	// Try as export symbol name
	uint32_t addr = PEFLoader::FindExport(result, s);
	if (addr) return addr;

	return 0;
}

// -- Commands --

static void cmdSections() {
	printf("Sections (%zu):\n", result.sections.size());
	for (size_t i = 0; i < result.sections.size(); i++) {
		auto &s = result.sections[i];
		printf("  [%zu] %-8s addr=0x%08X size=0x%X (%u)\n",
		       i, sectionKindName(s.kind), s.address, s.size, s.size);
	}
}

static void cmdImports() {
	printf("Imports (%zu):\n", result.imports.size());
	std::string lastLib;
	for (size_t i = 0; i < result.imports.size(); i++) {
		auto &imp = result.imports[i];
		if (imp.library != lastLib) {
			printf("  %s:\n", imp.library.c_str());
			lastLib = imp.library;
		}
		printf("    [%zu] %s (%s) -> 0x%08X\n",
		       i, imp.symbol.c_str(), symClassName(imp.symbolClass),
		       imp.resolvedAddress);
	}
}

static void cmdExports(const std::string &pattern) {
	printf("Exports");
	if (!pattern.empty()) printf(" matching \"%s\"", pattern.c_str());
	printf(":\n");
	int count = 0;
	for (auto &exp : result.exports) {
		if (!pattern.empty() && exp.name.find(pattern) == std::string::npos)
			continue;
		uint32_t addr = 0;
		if (exp.sectionIndex < result.sections.size())
			addr = result.sections[exp.sectionIndex].address + exp.offset;
		printf("  %s (%s) = 0x%08X (section %u + 0x%X)\n",
		       exp.name.c_str(), symClassName(exp.symbolClass),
		       addr, exp.sectionIndex, exp.offset);
		count++;
	}
	printf("(%d entries)\n", count);
}

static void cmdEntry() {
	printf("Entry points:\n");
	if (result.entryPoint) {
		uint32_t code = memoryReadLong(result.entryPoint);
		uint32_t toc = memoryReadLong(result.entryPoint + 4);
		printf("  main:  TVector @ 0x%08X → code=0x%08X toc=0x%08X\n",
		       result.entryPoint, code, toc);
		auto d = describeAddr(code);
		if (!d.empty()) printf("         code → %s\n", d.c_str());
	} else {
		printf("  main:  (none)\n");
	}
	if (result.initPoint) {
		uint32_t code = memoryReadLong(result.initPoint);
		uint32_t toc = memoryReadLong(result.initPoint + 4);
		printf("  init:  TVector @ 0x%08X → code=0x%08X toc=0x%08X\n",
		       result.initPoint, code, toc);
		auto d = describeAddr(code);
		if (!d.empty()) printf("         code → %s\n", d.c_str());
	} else {
		printf("  init:  (none)\n");
	}
	if (result.termPoint) {
		printf("  term:  TVector @ 0x%08X\n", result.termPoint);
	}
}

static void cmdToc() {
	printf("TOC base (RTOC): 0x%08X\n", result.tocBase);
	auto d = describeAddr(result.tocBase);
	if (!d.empty()) printf("  → %s\n", d.c_str());
}

static void cmdSym(const std::string &name) {
	uint32_t addr = PEFLoader::FindExport(result, name);
	if (!addr) {
		printf("Symbol \"%s\" not found\n", name.c_str());
		return;
	}
	printf("%s = 0x%08X\n", name.c_str(), addr);
	auto d = describeAddr(addr);
	if (!d.empty()) printf("  %s\n", d.c_str());

	// If it's a TVector, decode it
	int sec = findSection(addr);
	if (sec >= 0 && result.sections[sec].kind != PEF::kCode) {
		uint32_t code = memoryReadLong(addr);
		uint32_t toc = memoryReadLong(addr + 4);
		int csec = findSection(code);
		if (csec >= 0 && result.sections[csec].kind == PEF::kCode) {
			printf("  TVector: code=0x%08X (%s + 0x%X), toc=0x%08X\n",
			       code, sectionKindName(result.sections[csec].kind),
			       code - result.sections[csec].address, toc);
		}
	}
}

static void cmdAddr(uint32_t addr) {
	printf("0x%08X:\n", addr);
	auto d = describeAddr(addr);
	if (!d.empty()) printf("  %s\n", d.c_str());
	else printf("  (not in any loaded section)\n");

	// Show nearby symbols
	auto it = addrToSymbol.lower_bound(addr);
	if (it != addrToSymbol.begin()) {
		--it;
		if (addr - it->first < 0x10000)
			printf("  nearest symbol below: %s @ 0x%08X (+0x%X)\n",
			       it->second.c_str(), it->first, addr - it->first);
	}
}

static void cmdTocOff(int32_t offset) {
	uint32_t addr = result.tocBase + offset;
	uint32_t val = memoryReadLong(addr);

	printf("RTOC%+d = absolute 0x%08X\n", offset, addr);
	printf("  value: 0x%08X\n", val);

	// Check address-to-symbol for the slot itself
	auto it = addrToSymbol.find(addr);
	if (it != addrToSymbol.end())
		printf("  slot: %s\n", it->second.c_str());

	// Describe where the value points
	auto d = describeAddr(val);
	if (!d.empty()) printf("  → %s\n", d.c_str());

	// Check if it looks like a TVector
	uint32_t word2 = memoryReadLong(addr + 4);
	int sec0 = findSection(val);
	int sec1 = findSection(word2);
	if (sec0 >= 0 && sec1 >= 0 &&
	    result.sections[sec0].kind == PEF::kCode &&
	    (result.sections[sec1].kind == PEF::kUnpackedData ||
	     result.sections[sec1].kind == PEF::kPatternInitData)) {
		printf("  TVector: code=0x%08X (section %d + 0x%X), toc=0x%08X\n",
		       val, sec0, val - result.sections[sec0].address, word2);
	}
}

static void cmdTvec(uint32_t addr) {
	uint32_t code = memoryReadLong(addr);
	uint32_t toc = memoryReadLong(addr + 4);
	printf("TVector @ 0x%08X:\n", addr);
	printf("  code: 0x%08X", code);
	auto d = describeAddr(code);
	if (!d.empty()) printf(" (%s)", d.c_str());
	printf("\n");
	printf("  toc:  0x%08X", toc);
	d = describeAddr(toc);
	if (!d.empty()) printf(" (%s)", d.c_str());
	printf("\n");
}

static void cmdDisasm(uint32_t addr, int count) {
	int sec = findSection(addr);
	if (sec < 0) {
		printf("Address 0x%08X not in any loaded section\n", addr);
		return;
	}

	uint32_t secBase = result.sections[sec].address;
	uint32_t secSize = result.sections[sec].size;
	uint32_t maxBytes = (addr + count * 4 <= secBase + secSize)
	                    ? count * 4
	                    : secBase + secSize - addr;

	uint8_t *code = memoryPointer(addr);
	cs_insn *insn;
	size_t n = cs_disasm(cs, code, maxBytes, addr, count, &insn);
	if (n == 0) {
		printf("Failed to disassemble at 0x%08X\n", addr);
		return;
	}

	printf("Disassembly at 0x%08X (section %d + 0x%X):\n",
	       addr, sec, addr - secBase);
	for (size_t i = 0; i < n; i++) {
		// Raw bytes
		uint32_t raw = memoryReadLong(insn[i].address);
		printf("  %08X: %08X  %-8s %s",
		       (uint32_t)insn[i].address, raw,
		       insn[i].mnemonic, insn[i].op_str);

		// Annotate with symbol if known
		auto it = addrToSymbol.find((uint32_t)insn[i].address);
		if (it != addrToSymbol.end())
			printf("  ; %s", it->second.c_str());

		// Annotate TOC-relative loads: lwz rN, offset(r2)
		if (strstr(insn[i].op_str, "(r2)") || strstr(insn[i].op_str, "(RTOC)")) {
			// Parse the offset from the operand
			const char *p = insn[i].op_str;
			// Find the second operand (after comma)
			while (*p && *p != ',') p++;
			if (*p == ',') p++;
			while (*p == ' ') p++;
			// Parse the displacement
			char *end;
			long disp = strtol(p, &end, 0);
			if (end != p) {
				uint32_t tocAddr = result.tocBase + (int32_t)disp;
				uint32_t tocVal = memoryReadLong(tocAddr);
				auto sym = addrToSymbol.find(tocAddr);
				if (sym != addrToSymbol.end())
					printf("  ; [%s] = 0x%08X", sym->second.c_str(), tocVal);
				else
					printf("  ; [RTOC%+ld] = 0x%08X", disp, tocVal);
			}
		}

		printf("\n");
	}
	cs_free(insn, n);
}

static void cmdMem(uint32_t addr, int count) {
	for (int i = 0; i < count; i += 16) {
		printf("  %08X:", addr + i);
		for (int j = 0; j < 16 && i + j < count; j += 4) {
			printf(" %08X", memoryReadLong(addr + i + j));
		}
		printf("  ");
		for (int j = 0; j < 16 && i + j < count; j++) {
			uint8_t c = memoryReadByte(addr + i + j);
			printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
		}
		printf("\n");
	}
}

static void cmdRead32(uint32_t addr) {
	uint32_t val = memoryReadLong(addr);
	printf("0x%08X: 0x%08X (%d)\n", addr, val, (int32_t)val);
	auto d = describeAddr(val);
	if (!d.empty()) printf("  → %s\n", d.c_str());
}

static void cmdHelp() {
	printf("Commands:\n");
	printf("  sections              List sections\n");
	printf("  imports               List imports with library and symbol\n");
	printf("  exports [pattern]     List exports (optional filter)\n");
	printf("  entry                 Show entry points\n");
	printf("  toc                   Show TOC base\n");
	printf("  sym <name>            Look up export by name\n");
	printf("  addr <address>        Identify address (section, nearest symbol)\n");
	printf("  tocoff <offset>       Resolve TOC-relative offset (e.g. tocoff -0x18)\n");
	printf("  tvec <address>        Decode TVector at address\n");
	printf("  disasm <addr> [n]     Disassemble n instructions (default 20)\n");
	printf("  disasm <symbol> [n]   Disassemble at export symbol\n");
	printf("  mem <addr> [n]        Hex dump n bytes (default 64)\n");
	printf("  read32 <addr>         Read 32-bit value\n");
	printf("  help                  This message\n");
	printf("  quit                  Exit\n");
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: pef_inspect <pef-file>\n");
		return 1;
	}

	// Initialize emulated memory
	Memory = (uint8_t *)aligned_alloc(4096, MemorySize);
	if (!Memory) { fprintf(stderr, "Failed to allocate memory\n"); return 1; }
	memset(Memory, 0, MemorySize);
	memorySetMemory(Memory, MemorySize);
	MM::Init(Memory, MemorySize, kGlobalSize, kStackSize);
	cpuStartup();
	cpuSetModel(3, 0);

	// Initialize Capstone
	if (cs_open(CS_ARCH_PPC, CS_MODE_BIG_ENDIAN, &cs) != CS_ERR_OK) {
		fprintf(stderr, "Failed to initialize Capstone\n");
		return 1;
	}

	// Load PEF with a resolver that assigns dummy addresses for imports
	uint32_t nextDummyAddr = 0xDEAD0000;
	auto resolver = [&](const std::string &lib, const std::string &sym,
	                     uint8_t cls) -> uint32_t {
		uint32_t addr = nextDummyAddr;
		nextDummyAddr += 0x10;
		return addr;
	};

	const char *path = argv[1];
	if (!PEFLoader::LoadPEFFile(path, resolver, result)) {
		fprintf(stderr, "Failed to load PEF: %s\n", path);
		return 1;
	}

	// Build symbol tables
	// Exports: address → name
	for (auto &exp : result.exports) {
		if (exp.sectionIndex < result.sections.size()) {
			uint32_t addr = result.sections[exp.sectionIndex].address + exp.offset;
			addrToSymbol[addr] = exp.name;
		}
	}
	// Imports: resolved address → "lib::symbol"
	for (auto &imp : result.imports) {
		if (imp.resolvedAddress)
			addrToSymbol[imp.resolvedAddress] = imp.library + "::" + imp.symbol;
	}

	printf("Loaded %s: %zu sections, %zu imports, %zu exports, TOC=0x%08X\n",
	       path, result.sections.size(), result.imports.size(),
	       result.exports.size(), result.tocBase);

	// REPL
	std::string line;
	printf("> ");
	while (std::getline(std::cin, line)) {
		std::istringstream iss(line);
		std::string cmd;
		iss >> cmd;

		if (cmd.empty()) {
			// nothing
		} else if (cmd == "sections") {
			cmdSections();
		} else if (cmd == "imports") {
			cmdImports();
		} else if (cmd == "exports") {
			std::string pattern;
			iss >> pattern;
			cmdExports(pattern);
		} else if (cmd == "entry") {
			cmdEntry();
		} else if (cmd == "toc") {
			cmdToc();
		} else if (cmd == "sym") {
			std::string name;
			iss >> name;
			if (name.empty()) printf("Usage: sym <name>\n");
			else cmdSym(name);
		} else if (cmd == "addr") {
			std::string s;
			iss >> s;
			if (s.empty()) printf("Usage: addr <address>\n");
			else cmdAddr(parseAddr(s));
		} else if (cmd == "tocoff") {
			std::string s;
			iss >> s;
			if (s.empty()) printf("Usage: tocoff <offset>\n");
			else {
				char *end;
				int32_t off = (int32_t)strtol(s.c_str(), &end, 0);
				cmdTocOff(off);
			}
		} else if (cmd == "tvec") {
			std::string s;
			iss >> s;
			if (s.empty()) printf("Usage: tvec <address>\n");
			else cmdTvec(parseAddr(s));
		} else if (cmd == "disasm") {
			std::string s;
			int count = 20;
			iss >> s;
			if (s.empty()) { printf("Usage: disasm <addr|symbol> [count]\n"); }
			else {
				iss >> count;
				if (count <= 0) count = 20;
				uint32_t addr = parseAddr(s);
				if (!addr) printf("Cannot resolve \"%s\"\n", s.c_str());
				else {
					// If it's a TVector (in data section), dereference to get code
					int sec = findSection(addr);
					if (sec >= 0 && result.sections[sec].kind != PEF::kCode) {
						uint32_t code = memoryReadLong(addr);
						if (findSection(code) >= 0) {
							printf("(TVector at 0x%08X → code 0x%08X)\n", addr, code);
							addr = code;
						}
					}
					cmdDisasm(addr, count);
				}
			}
		} else if (cmd == "mem") {
			std::string s;
			int count = 64;
			iss >> s;
			if (s.empty()) { printf("Usage: mem <addr> [count]\n"); }
			else {
				iss >> count;
				if (count <= 0) count = 64;
				cmdMem(parseAddr(s), count);
			}
		} else if (cmd == "read32") {
			std::string s;
			iss >> s;
			if (s.empty()) printf("Usage: read32 <addr>\n");
			else cmdRead32(parseAddr(s));
		} else if (cmd == "help" || cmd == "?") {
			cmdHelp();
		} else if (cmd == "quit" || cmd == "exit" || cmd == "q") {
			break;
		} else {
			printf("Unknown command: %s (type 'help')\n", cmd.c_str());
		}

		printf("> ");
	}

	cs_close(&cs);
	free(Memory);
	return 0;
}
