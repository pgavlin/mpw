/*
 * dispatch_test.cpp
 *
 * Tests for Phase 4 InterfaceLib/MathLib wrappers.
 * Calls each wrapper through PPC execution via CFM stubs.
 */

#include <toolbox/cfm_stubs.h>
#include <toolbox/ppc_dispatch.h>
#include <toolbox/pef_loader.h>
#include <toolbox/mm.h>
#include <toolbox/os.h>
#include <toolbox/toolbox.h>
#include <cpu/ppc/ppc.h>

#include <cpu/m68k/defs.h>
#include <cpu/m68k/CpuModule.h>
#include <cpu/m68k/fmem.h>

#include <macos/sysequ.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

static uint8_t *Memory = nullptr;
static const uint32_t MemorySize = 16 * 1024 * 1024;
static const uint32_t kGlobalSize = 0x10000;
static const uint32_t kStackSize = 32 * 1024;

// Helper: call a CFM stub through PPC with up to 4 args.
// Loads the TVector, sets r3-r6, does bctrl, returns r3.
static uint32_t callStub(const char *lib, const char *sym,
                          uint32_t r3 = 0, uint32_t r4 = 0,
                          uint32_t r5 = 0, uint32_t r6 = 0) {
	uint32_t tvec = CFMStubs::ResolveImport(lib, sym);
	assert(tvec != 0);

	uint32_t codeAddr = memoryReadLong(tvec);

	// Write a tiny caller at 0xE000 that calls the stub via bctrl.
	// The sc interrupt hook returns by setting PC=LR, which is the
	// address after bctrl. We need our outer LR (= 0 sentinel) to
	// survive. Save it in r0 on the stack frame, restore after bctrl.
	uint32_t base = 0xE000;
	memoryWriteLong(0x7C0802A6, base +  0); // mflr   r0
	memoryWriteLong(0x90010008, base +  4); // stw    r0, 8(r1)
	memoryWriteLong(0x7D8903A6, base +  8); // mtctr  r12
	memoryWriteLong(0x4E800421, base + 12); // bctrl
	memoryWriteLong(0x80010008, base + 16); // lwz    r0, 8(r1)
	memoryWriteLong(0x7C0803A6, base + 20); // mtlr   r0
	memoryWriteLong(0x4E800020, base + 24); // blr

	PPC::SetGPR(3, r3);
	PPC::SetGPR(4, r4);
	PPC::SetGPR(5, r5);
	PPC::SetGPR(6, r6);
	PPC::SetGPR(1, MemorySize - 256);
	PPC::SetGPR(2, memoryReadLong(tvec + 4));
	PPC::SetGPR(12, codeAddr);
	PPC::SetLR(0);
	PPC::Execute(base, PPC::GetGPR(2));

	return PPC::GetGPR(3);
}

// Helper: call stub, return FPR1 as double
static double callStubFPR(const char *lib, const char *sym,
                           uint32_t r3 = 0, uint32_t r4 = 0) {
	callStub(lib, sym, r3, r4);
	return PPC::GetFPR(1);
}

// ================================================================
// Memory Manager tests
// ================================================================

static void testMemoryManager() {
	printf("Test: Memory Manager wrappers...\n");

	// NewPtr / GetPtrSize / DisposePtr
	uint32_t ptr = callStub("InterfaceLib", "NewPtr", 256);
	assert(ptr != 0);
	printf("  NewPtr(256) = 0x%08X\n", ptr);

	uint32_t size = callStub("InterfaceLib", "GetPtrSize", ptr);
	assert(size == 256);
	printf("  GetPtrSize = %u OK\n", size);

	callStub("InterfaceLib", "DisposePtr", ptr);
	printf("  DisposePtr OK\n");

	// NewHandle / HLock / HUnlock / DisposeHandle
	uint32_t handle = callStub("InterfaceLib", "NewHandle", 128);
	assert(handle != 0);
	printf("  NewHandle(128) = 0x%08X\n", handle);

	uint32_t lockErr = callStub("InterfaceLib", "HLock", handle);
	assert(lockErr == 0);

	uint32_t unlockErr = callStub("InterfaceLib", "HUnlock", handle);
	assert(unlockErr == 0);

	callStub("InterfaceLib", "DisposeHandle", handle);
	printf("  Handle lifecycle OK\n");

	// BlockMove
	uint32_t src = 0xD000;
	uint32_t dst = 0xD100;
	memoryWriteLong(0xDEADBEEF, src);
	memoryWriteLong(0xCAFEBABE, src + 4);
	callStub("InterfaceLib", "BlockMove", src, dst, 8);
	assert(memoryReadLong(dst) == 0xDEADBEEF);
	assert(memoryReadLong(dst + 4) == 0xCAFEBABE);
	printf("  BlockMove OK\n");

	// FreeMem
	uint32_t free = callStub("InterfaceLib", "FreeMem");
	assert(free > 0);
	printf("  FreeMem = %u OK\n", free);

	// GetZone — returns ApplZone low-memory global
	uint32_t zone = callStub("InterfaceLib", "GetZone");
	printf("  GetZone = 0x%08X OK\n", zone);

	printf("  PASS\n");
}

// ================================================================
// String Conversion tests
// ================================================================

static void testStringConversions() {
	printf("Test: String Conversion wrappers...\n");

	// c2pstr: write "hello\0" at 0xC000, convert in place
	uint32_t addr = 0xC000;
	const char *cstr = "hello";
	for (int i = 0; i <= 5; i++) memoryWriteByte(cstr[i], addr + i);

	uint32_t result = callStub("InterfaceLib", "c2pstr", addr);
	assert(result == addr);
	assert(memoryReadByte(addr) == 5);      // length
	assert(memoryReadByte(addr + 1) == 'h');
	assert(memoryReadByte(addr + 5) == 'o');
	printf("  c2pstr OK\n");

	// p2cstr: convert back
	result = callStub("InterfaceLib", "p2cstr", addr);
	assert(result == addr);
	assert(memoryReadByte(addr) == 'h');
	assert(memoryReadByte(addr + 4) == 'o');
	assert(memoryReadByte(addr + 5) == 0);
	printf("  p2cstr OK\n");

	printf("  PASS\n");
}

// ================================================================
// Time tests
// ================================================================

static void testTime() {
	printf("Test: Time wrappers...\n");

	// GetDateTime — writes Mac timestamp to ptr
	uint32_t secsAddr = 0xC100;
	memoryWriteLong(0, secsAddr);
	callStub("InterfaceLib", "GetDateTime", secsAddr);
	uint32_t macTime = memoryReadLong(secsAddr);
	// Mac epoch is 1904-01-01. Current time should be > 3 billion seconds.
	assert(macTime > 3000000000u);
	printf("  GetDateTime = %u OK\n", macTime);

	// SecondsToDate
	uint32_t dateAddr = 0xC110;
	callStub("InterfaceLib", "SecondsToDate", macTime, dateAddr);
	uint16_t year = memoryReadWord(dateAddr);
	uint16_t month = memoryReadWord(dateAddr + 2);
	assert(year >= 2024 && year <= 2030);
	assert(month >= 1 && month <= 12);
	printf("  SecondsToDate: year=%u month=%u OK\n", year, month);

	// TickCount
	uint32_t ticks = callStub("InterfaceLib", "TickCount");
	// Just verify it returns something (may be 0 if called immediately)
	printf("  TickCount = %u OK\n", ticks);

	printf("  PASS\n");
}

// Gestalt test deferred to Phase 6 integration (requires ToolBox::Init)

// ================================================================
// NGetTrapAddress test
// ================================================================

static void testNGetTrapAddress() {
	printf("Test: NGetTrapAddress wrapper...\n");

	// Should return non-zero (trap exists)
	uint32_t addr = callStub("InterfaceLib", "NGetTrapAddress", 0xA122, 1);
	assert(addr != 0);
	printf("  NGetTrapAddress(0xA122) = 0x%08X OK\n", addr);

	printf("  PASS\n");
}

// ================================================================
// Process Manager test
// ================================================================

static void testProcessManager() {
	printf("Test: Process Manager wrappers...\n");

	uint32_t psnAddr = 0xC300;
	memoryWriteLong(0, psnAddr);
	memoryWriteLong(0, psnAddr + 4);
	uint32_t err = callStub("InterfaceLib", "GetCurrentProcess", psnAddr);
	assert(err == 0);
	uint32_t psnLow = memoryReadLong(psnAddr + 4);
	assert(psnLow == 2); // kCurrentProcess
	printf("  GetCurrentProcess: PSN=%u OK\n", psnLow);

	printf("  PASS\n");
}

// ================================================================
// Mixed Mode Manager test
// ================================================================

static void testMixedMode() {
	printf("Test: Mixed Mode Manager wrappers...\n");

	// NewRoutineDescriptor with PPC ISA — should pass through
	uint32_t fakeTvec = 0xC400;
	memoryWriteLong(0x12345678, fakeTvec);     // code
	memoryWriteLong(0x9ABCDEF0, fakeTvec + 4); // toc

	uint32_t desc = callStub("InterfaceLib", "NewRoutineDescriptor",
	                          fakeTvec, 0, 1); // ISA=1 (PPC)
	assert(desc == fakeTvec); // should pass through unchanged
	printf("  NewRoutineDescriptor(PPC) = passthrough OK\n");

	// DisposeRoutineDescriptor on a PPC passthrough — should not crash
	callStub("InterfaceLib", "DisposeRoutineDescriptor", desc);
	printf("  DisposeRoutineDescriptor(PPC passthrough) OK\n");

	// NewRoutineDescriptor with 68K ISA — should allocate descriptor
	uint32_t desc68k = callStub("InterfaceLib", "NewRoutineDescriptor",
	                              0xC500, 0, 0); // ISA=0 (68K)
	assert(desc68k != 0);
	assert(memoryReadWord(desc68k) == 0xAAFE); // magic
	printf("  NewRoutineDescriptor(68K) = 0x%08X, magic=0xAAFE OK\n", desc68k);

	// DisposeRoutineDescriptor on 68K descriptor
	callStub("InterfaceLib", "DisposeRoutineDescriptor", desc68k);
	printf("  DisposeRoutineDescriptor(68K) OK\n");

	printf("  PASS\n");
}

// ================================================================
// MathLib test
// ================================================================

static void testMathLib() {
	printf("Test: MathLib wrappers...\n");

	// str2dec: parse "3.14"
	uint32_t strAddr = 0xC600;
	const char *numStr = "3.14";
	for (int i = 0; i <= 4; i++) memoryWriteByte(numStr[i], strAddr + i);

	uint32_t ixAddr = 0xC610;
	memoryWriteWord(0, ixAddr); // index starts at 0 (C-style, 0-based)

	uint32_t decAddr = 0xC620; // 42 bytes for decimal struct
	memset(memoryPointer(decAddr), 0, 42);

	uint32_t vpAddr = 0xC660;
	memoryWriteWord(0, vpAddr);

	callStub("MathLib", "str2dec", strAddr, ixAddr, decAddr, vpAddr);
	uint16_t vp = memoryReadWord(vpAddr);
	uint8_t sigLen = memoryReadByte(decAddr + 4);
	printf("  str2dec(\"3.14\"): vp=%u, sigLen=%u, sgn=%u, exp=%d\n",
	       vp, sigLen, memoryReadByte(decAddr),
	       (int16_t)memoryReadWord(decAddr + 2));
	assert(vp != 0); // should be valid
	assert(sigLen > 0);

	// dec2num: convert back to double
	double val = callStubFPR("MathLib", "dec2num", decAddr);
	printf("  dec2num -> %f\n", val);
	assert(val > 3.13 && val < 3.15);

	printf("  PASS\n");
}

// ================================================================
// PBHOpenSync stdin/stdout/stderr test
// ================================================================

static void testPBHOpenSync() {
	printf("Test: PBHOpenSync stdin/stdout/stderr mapping...\n");

	// Build a parameter block with ioNamePtr pointing to "stdout"
	uint32_t pb = 0xC700;
	memset(memoryPointer(pb), 0, 80);

	uint32_t nameAddr = 0xC780;
	// Write Pascal string "stdout"
	memoryWriteByte(6, nameAddr);
	memoryWriteByte('s', nameAddr + 1);
	memoryWriteByte('t', nameAddr + 2);
	memoryWriteByte('d', nameAddr + 3);
	memoryWriteByte('o', nameAddr + 4);
	memoryWriteByte('u', nameAddr + 5);
	memoryWriteByte('t', nameAddr + 6);

	memoryWriteLong(nameAddr, pb + 18); // ioNamePtr

	uint32_t err = callStub("InterfaceLib", "PBHOpenSync", pb);
	assert(err == 0);
	uint16_t refNum = memoryReadWord(pb + 24);
	assert(refNum == 1); // stdout = fd 1
	printf("  PBHOpenSync(\"stdout\") -> refNum=%d OK\n", refNum);

	printf("  PASS\n");
}

// ================================================================
// Full StdCLib load test (all 66 resolve)
// ================================================================

static void testStdCLibLoad(const char *path) {
	printf("Test: Load StdCLib — all 66 imports resolve...\n");

	uint32_t resolved = 0, catchAll = 0;
	auto resolver = [&](const std::string &lib, const std::string &sym,
	                     uint8_t cls) -> uint32_t {
		uint32_t addr = CFMStubs::ResolveImport(lib, sym);
		if (addr) { resolved++; return addr; }
		catchAll++;
		return CFMStubs::RegisterStub(lib, sym, [lib, sym]() {
			fprintf(stderr, "  UNIMPLEMENTED: %s::%s\n", lib.c_str(), sym.c_str());
			PPC::SetGPR(3, 0);
		});
	};

	PEFLoader::LoadResult result;
	bool ok = PEFLoader::LoadPEFFile(path, resolver, result);
	assert(ok);
	assert(resolved == 66);
	assert(catchAll == 0);
	printf("  66 resolved, 0 catch-all: OK\n");
	printf("  PASS\n");
}

int main(int argc, char *argv[]) {
	Memory = (uint8_t *)aligned_alloc(4096, MemorySize);
	assert(Memory);
	memset(Memory, 0, MemorySize);
	memorySetMemory(Memory, MemorySize);

	MM::Init(Memory, MemorySize, kGlobalSize, kStackSize);
	cpuStartup();
	cpuSetModel(3, 0);
	PPC::Init(Memory, MemorySize);
	PPC::SetSCHandler(CFMStubs::Dispatch);
	CFMStubs::Init();
	PPCDispatch::RegisterStdCLibImports();
	testMemoryManager();
	testStringConversions();
	testTime();
	testNGetTrapAddress();
	testProcessManager();
	testMixedMode();
	testMathLib();
	testPBHOpenSync();

	const char *stdclibPath = nullptr;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--stdclib=", 10) == 0) stdclibPath = argv[i] + 10;
	}
	if (stdclibPath) testStdCLibLoad(stdclibPath);
	else printf("Skipping StdCLib load test (pass --stdclib=<path>)\n");

	PPC::Shutdown();
	free(Memory);
	printf("\nAll Phase 4 dispatch tests passed.\n");
	return 0;
}
