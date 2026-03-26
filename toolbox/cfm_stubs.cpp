/*
 * cfm_stubs.cpp
 *
 * CFM stub generator and sc dispatcher.
 *
 * Each stub consists of:
 * - Code: 12 bytes at codeAddr = { li r11,<index>; sc; blr }
 * - A TVector: 8 bytes at tvecAddr = { codeAddr, 0 }
 *
 * When PPC executes the stub code:
 *   li r11, <index>   =>  0x39600000 | index
 *   sc                =>  0x44000002
 *
 * The sc callback reads R11 to get the function index and calls the handler.
 * (The blr is present for completeness but never reached — the Phase 1
 * sc hook returns via LR directly.)
 */

#include "cfm_stubs.h"
#include "mm.h"

#include <cpu/m68k/defs.h>
#include <cpu/m68k/fmem.h>
#include <cpu/ppc/ppc.h>

#include <cstdio>
#include <vector>
#include <unordered_map>

namespace CFMStubs {

static const uint32_t kMaxStubs = 4096;
static const uint32_t kStubCodeSize = 12; // li r11,N + sc + blr
static const uint32_t kTVecSize = 8;      // code ptr + toc ptr

struct StubEntry {
	std::string library;
	std::string symbol;
	std::string fullName; // "library::symbol"
	Handler handler;
	uint32_t tvecAddr;
	uint32_t codeAddr;
};

static std::vector<StubEntry> stubs;
static std::unordered_map<std::string, uint32_t> stubMap; // fullName -> index
static uint32_t stubMemBase = 0;
static uint32_t stubMemSize = 0;
static uint32_t nextCodeOffset = 0;
static uint32_t nextTVecOffset = 0;
static bool initialized = false;
static bool traceEnabled = false;

void Init() {
	if (initialized) return;

	// Allocate memory for stubs: code region + tvec region
	stubMemSize = kMaxStubs * (kStubCodeSize + kTVecSize);
	uint16_t err = MM::Native::NewPtr(stubMemSize, true, stubMemBase);
	if (err) {
		fprintf(stderr, "CFMStubs: failed to allocate stub memory\n");
		return;
	}

	nextCodeOffset = 0;
	nextTVecOffset = kMaxStubs * kStubCodeSize; // TVecs after code region
	stubs.clear();
	stubMap.clear();
	initialized = true;
}

uint32_t RegisterStub(const std::string &library, const std::string &symbol,
                      Handler handler) {
	if (!initialized) Init();

	std::string fullName = library + "::" + symbol;

	auto it = stubMap.find(fullName);
	if (it != stubMap.end()) {
		// Update handler if re-registering with a new one
		if (handler) stubs[it->second].handler = handler;
		return stubs[it->second].tvecAddr;
	}

	uint32_t index = stubs.size();
	if (index >= kMaxStubs) {
		fprintf(stderr, "CFMStubs: too many stubs (max %u)\n", kMaxStubs);
		return 0;
	}

	// Write stub code: li r11, <index>; sc; blr
	uint32_t codeAddr = stubMemBase + nextCodeOffset;
	memoryWriteLong(0x39600000 | (index & 0xFFFF), codeAddr);     // li r11, index
	memoryWriteLong(0x44000002, codeAddr + 4);                     // sc
	memoryWriteLong(0x4E800020, codeAddr + 8);                     // blr
	nextCodeOffset += kStubCodeSize;

	// Write TVector: {codeAddr, 0}
	uint32_t tvecAddr = stubMemBase + nextTVecOffset;
	memoryWriteLong(codeAddr, tvecAddr);
	memoryWriteLong(0, tvecAddr + 4); // TOC = 0 for native stubs
	nextTVecOffset += kTVecSize;

	StubEntry entry;
	entry.library = library;
	entry.symbol = symbol;
	entry.fullName = fullName;
	entry.handler = handler;
	entry.tvecAddr = tvecAddr;
	entry.codeAddr = codeAddr;

	stubs.push_back(std::move(entry));
	stubMap[fullName] = index;

	return tvecAddr;
}

uint32_t ResolveImport(const std::string &library, const std::string &symbol) {
	std::string fullName = library + "::" + symbol;
	auto it = stubMap.find(fullName);
	if (it != stubMap.end())
		return stubs[it->second].tvecAddr;
	return 0;
}

void Dispatch() {
	uint32_t index = PPC::GetGPR(11);
	if (index >= stubs.size()) {
		fprintf(stderr, "CFMStubs: invalid stub index %u (max %zu)\n",
		        index, stubs.size());
		PPC::Stop();
		return;
	}

	StubEntry &entry = stubs[index];

	if (traceEnabled) {
		fprintf(stderr, "  sc> %s (r3=0x%08X r4=0x%08X r5=0x%08X)\n",
		        entry.fullName.c_str(),
		        PPC::GetGPR(3), PPC::GetGPR(4), PPC::GetGPR(5));
	}

	if (entry.handler) {
		entry.handler();
	} else {
		fprintf(stderr, "CFMStubs: unimplemented stub %s (index %u)\n",
		        entry.fullName.c_str(), index);
		PPC::Stop();
		return;
	}

	if (traceEnabled) {
		fprintf(stderr, "       -> r3=0x%08X\n", PPC::GetGPR(3));
	}
}

void SetTrace(bool trace) {
	traceEnabled = trace;
}

void RegisterTVector(const std::string &library, const std::string &symbol,
                     uint32_t tvecAddr) {
	std::string fullName = library + "::" + symbol;

	auto it = stubMap.find(fullName);
	if (it != stubMap.end()) {
		stubs[it->second].tvecAddr = tvecAddr;
		return;
	}

	uint32_t index = stubs.size();
	StubEntry entry;
	entry.library = library;
	entry.symbol = symbol;
	entry.fullName = fullName;
	entry.handler = nullptr; // no sc handler — native PPC code
	entry.tvecAddr = tvecAddr;
	entry.codeAddr = memoryReadLong(tvecAddr); // read code addr from TVector
	stubs.push_back(std::move(entry));
	stubMap[fullName] = index;
}

uint32_t AllocateCode(const uint32_t *instructions, uint32_t count) {
	if (!initialized) Init();

	uint32_t codeSize = count * 4;
	if (nextCodeOffset + codeSize > kMaxStubs * kStubCodeSize) {
		fprintf(stderr, "CFMStubs: out of code space for custom code\n");
		return 0;
	}

	uint32_t codeAddr = stubMemBase + nextCodeOffset;
	for (uint32_t i = 0; i < count; i++)
		memoryWriteLong(instructions[i], codeAddr + i * 4);
	nextCodeOffset += codeSize;

	// Create a TVector for the code
	if (nextTVecOffset + kTVecSize > stubMemSize) {
		fprintf(stderr, "CFMStubs: out of tvec space for custom code\n");
		return 0;
	}
	uint32_t tvecAddr = stubMemBase + nextTVecOffset;
	memoryWriteLong(codeAddr, tvecAddr);
	memoryWriteLong(0, tvecAddr + 4);
	nextTVecOffset += kTVecSize;

	return tvecAddr;
}

} // namespace CFMStubs
