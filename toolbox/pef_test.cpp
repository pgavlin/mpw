/*
 * pef_test.cpp
 *
 * Standalone test for the PEF loader.
 * Requires a working Memory Manager (MM::Init) and emulated memory.
 */

#include <toolbox/pef_loader.h>
#include <toolbox/mm.h>

#include <cpu/m68k/defs.h>
#include <cpu/m68k/fmem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

static uint8_t *Memory = nullptr;
static const uint32_t MemorySize = 16 * 1024 * 1024;
static const uint32_t kGlobalSize = 0x10000;
static const uint32_t kStackSize = 32 * 1024;

static void testIsPEF() {
	printf("Test: IsPEF detection... ");

	uint8_t pefHeader[] = { 'J','o','y','!', 'p','e','f','f' };
	assert(PEFLoader::IsPEF(pefHeader, 8));

	uint8_t notPef[] = { 0,0,0,0, 0,0,0,0 };
	assert(!PEFLoader::IsPEF(notPef, 8));

	assert(!PEFLoader::IsPEF(pefHeader, 4));

	printf("PASS\n");
}

static void testLoadHello(const char *path) {
	printf("Test: load Hello PEF from %s...\n", path);

	PEFLoader::SetTrace(true);

	uint32_t importCount = 0;
	auto resolver = [&](const std::string &lib, const std::string &sym,
	                     uint8_t cls) -> uint32_t {
		importCount++;
		// Return dummy addresses so relocations can proceed
		return 0xDEAD0000 + importCount * 0x10;
	};

	PEFLoader::LoadResult result;
	bool ok = PEFLoader::LoadPEFFile(path, resolver, result);
	assert(ok);

	printf("  Sections: %zu\n", result.sections.size());
	for (size_t i = 0; i < result.sections.size(); i++) {
		printf("    [%zu] kind=%d addr=0x%08X size=0x%X\n",
		       i, result.sections[i].kind,
		       result.sections[i].address, result.sections[i].size);
	}

	printf("  Exports: %zu\n", result.exports.size());
	for (auto &exp : result.exports) {
		printf("    %s (class=%d, section=%d, offset=0x%X)\n",
		       exp.name.c_str(), exp.symbolClass,
		       exp.sectionIndex, exp.offset);
	}

	printf("  entryPoint=0x%08X initPoint=0x%08X tocBase=0x%08X\n",
	       result.entryPoint, result.initPoint, result.tocBase);

	// Verify against DumpPEF reference output
	assert(result.sections.size() == 3);
	assert(result.sections[0].kind == 0); // code
	assert(result.sections[0].size == 0x11C);
	assert(result.sections[1].kind == 2); // pidata
	assert(result.sections[1].size == 0x50);
	assert(result.sections[2].kind == 4); // loader
	assert(result.entryPoint != 0);
	assert(result.tocBase != 0);
	assert(importCount == 10); // DumpPEF shows 10 StdCLib imports

	// Verify relocated import addresses (DumpPEF: SYMR cnt=10 at offset 0)
	uint32_t dataAddr = result.sections[1].address;
	for (int i = 0; i < 10; i++) {
		uint32_t val = memoryReadLong(dataAddr + i * 4);
		assert(val == 0xDEAD0000 + (i + 1) * 0x10);
	}
	printf("  Import relocations: OK\n");

	// Verify sectionD relocations (DumpPEF: DATA cnt=2 at offsets 0x28, 0x2C)
	uint32_t secD = result.sections[1].address;
	assert(memoryReadLong(dataAddr + 0x28) == 0x38 + secD);
	assert(memoryReadLong(dataAddr + 0x2C) == 0x48 + secD);
	printf("  SectionD relocations: OK\n");

	// Verify TVector8 (DumpPEF: DSC2 cnt=1 at offset 0x30)
	uint32_t secC = result.sections[0].address;
	assert(memoryReadLong(dataAddr + 0x30) == 0x34 + secC);
	assert(memoryReadLong(dataAddr + 0x34) == 0x30 + secD);
	printf("  TVector8 relocation: OK\n");

	// Verify "Hello, world!\r" string at offset 0x38
	const char *expected = "Hello, world!\r";
	for (int i = 0; expected[i]; i++)
		assert(memoryReadByte(dataAddr + 0x38 + i) == (uint8_t)expected[i]);
	printf("  String data: OK\n");

	// Entry TVector code should point into code section
	uint32_t entryCode = memoryReadLong(result.entryPoint);
	assert(entryCode >= secC && entryCode < secC + result.sections[0].size);

	PEFLoader::SetTrace(false);
	printf("  PASS\n");
}

static void testLoadStdCLib(const char *path) {
	printf("Test: load StdCLib PEF from %s...\n", path);

	PEFLoader::SetTrace(true);

	uint32_t importCount = 0;
	auto resolver = [&](const std::string &lib, const std::string &sym,
	                     uint8_t cls) -> uint32_t {
		importCount++;
		return 0xBEEF0000 + importCount * 0x10;
	};

	PEFLoader::LoadResult result;
	bool ok = PEFLoader::LoadPEFFile(path, resolver, result);
	assert(ok);

	printf("  Sections: %zu\n", result.sections.size());
	printf("  Exports: %zu\n", result.exports.size());
	printf("  entryPoint=0x%08X initPoint=0x%08X tocBase=0x%08X\n",
	       result.entryPoint, result.initPoint, result.tocBase);

	// StdCLib is a shared library: no main entry, but has init
	assert(result.entryPoint == 0);
	assert(result.initPoint != 0);
	assert(result.exports.size() > 50);
	assert(result.tocBase != 0);
	assert(importCount == 66); // 60 InterfaceLib + 4 MathLib + 2 PrivateInterfaceLib

	// Critical: TOC must come from init TVector, not data section start
	uint32_t dataSectAddr = result.sections[1].address;
	assert(result.tocBase != dataSectAddr);
	printf("  TOC from init TVector (not data sect start): 0x%08X OK\n", result.tocBase);

	// Verify known exports
	assert(PEFLoader::FindExport(result, "fprintf") != 0);
	assert(PEFLoader::FindExport(result, "exit") != 0);
	assert(PEFLoader::FindExport(result, "malloc") != 0);
	assert(PEFLoader::FindExport(result, "__target_for_exit") != 0);
	printf("  Key exports: OK\n");

	PEFLoader::SetTrace(false);
	printf("  PASS\n");
}

int main(int argc, char *argv[]) {
	// Set up emulated memory (same as loader.cpp does)
	Memory = (uint8_t *)aligned_alloc(4096, MemorySize);
	if (!Memory) {
		fprintf(stderr, "Failed to allocate memory\n");
		return 1;
	}
	memset(Memory, 0, MemorySize);
	memorySetMemory(Memory, MemorySize);
	MM::Init(Memory, MemorySize, kGlobalSize, kStackSize);

	testIsPEF();

	// Test with Hello PEF if provided
	const char *helloPath = nullptr;
	const char *stdclibPath = nullptr;

	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--hello=", 8) == 0) helloPath = argv[i] + 8;
		else if (strncmp(argv[i], "--stdclib=", 10) == 0) stdclibPath = argv[i] + 10;
	}

	if (helloPath) testLoadHello(helloPath);
	else printf("Skipping Hello test (pass --hello=<path>)\n");

	if (stdclibPath) testLoadStdCLib(stdclibPath);
	else printf("Skipping StdCLib test (pass --stdclib=<path>)\n");

	free(Memory);
	printf("\nAll PEF loader tests passed.\n");
	return 0;
}
