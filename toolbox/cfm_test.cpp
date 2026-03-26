/*
 * cfm_test.cpp
 *
 * Test for the CFM stub system — validates stub registration,
 * dispatch through PPC execution, tracing, and PEF loader integration.
 */

#include <toolbox/cfm_stubs.h>
#include <toolbox/ppc_dispatch.h>
#include <toolbox/pef_loader.h>
#include <toolbox/mm.h>
#include <cpu/ppc/ppc.h>

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

static void writeBE32(uint32_t addr, uint32_t val) {
	memoryWriteLong(val, addr);
}

static void testRegisterAndResolve() {
	printf("Test: register and resolve stubs... ");

	uint32_t tvec = CFMStubs::RegisterStub("TestLib", "add", []() {
		PPC::SetGPR(3, PPC::GetGPR(3) + PPC::GetGPR(4));
	});
	assert(tvec != 0);

	uint32_t resolved = CFMStubs::ResolveImport("TestLib", "add");
	assert(resolved == tvec);

	// Unregistered symbol returns 0
	assert(CFMStubs::ResolveImport("TestLib", "nonexistent") == 0);

	// Re-registering returns same TVector
	uint32_t tvec2 = CFMStubs::RegisterStub("TestLib", "add", nullptr);
	assert(tvec2 == tvec);

	printf("PASS\n");
}

static void testDispatchThroughPPC() {
	printf("Test: dispatch stub through PPC execution... ");

	// Register a stub that adds r3 + r4
	uint32_t addTvec = CFMStubs::ResolveImport("TestLib", "add");
	assert(addTvec != 0);


	// Write PPC code that calls through the TVector:
	//   li    r3, 10
	//   li    r4, 20
	//   lis   r12, hi(addTvec)
	//   ori   r12, r12, lo(addTvec)
	//   lwz   r0, 0(r12)       ; load code addr from TVector
	//   lwz   r2, 4(r12)       ; load TOC from TVector
	//   mtctr r0
	//   mflr  r0               ; save LR (bctrl will overwrite)
	//   stw   r0, 8(r1)
	//   bctrl                   ; call stub
	//   lwz   r0, 8(r1)        ; restore LR
	//   mtlr  r0
	//   blr
	uint32_t base = 0x8000;
	uint32_t hi = (addTvec >> 16) & 0xFFFF;
	uint32_t lo = addTvec & 0xFFFF;

	writeBE32(base +  0, 0x3860000A);              // li r3, 10
	writeBE32(base +  4, 0x38800014);              // li r4, 20
	writeBE32(base +  8, 0x3D800000 | hi);         // lis r12, hi
	writeBE32(base + 12, 0x618C0000 | lo);         // ori r12, r12, lo
	writeBE32(base + 16, 0x800C0000);              // lwz r0, 0(r12)
	writeBE32(base + 20, 0x804C0004);              // lwz r2, 4(r12)
	writeBE32(base + 24, 0x7C0903A6);              // mtctr r0
	writeBE32(base + 28, 0x7C0802A6);              // mflr r0
	writeBE32(base + 32, 0x90010008);              // stw r0, 8(r1)
	writeBE32(base + 36, 0x4E800421);              // bctrl
	writeBE32(base + 40, 0x80010008);              // lwz r0, 8(r1)
	writeBE32(base + 44, 0x7C0803A6);              // mtlr r0
	writeBE32(base + 48, 0x4E800020);              // blr

	// Set up stack pointer
	PPC::SetGPR(1, MemorySize - 256);
	PPC::SetLR(0); // sentinel
	PPC::Execute(base, 0);

	assert(PPC::GetGPR(3) == 30);
	printf("PASS\n");
}

static void testTrace() {
	printf("Test: dispatch tracing... ");

	CFMStubs::SetTrace(true);

	CFMStubs::RegisterStub("TraceLib", "traced_fn", []() {
		PPC::SetGPR(3, 0x42);
	});

	uint32_t tvec = CFMStubs::ResolveImport("TraceLib", "traced_fn");

	// Write code that calls the stub directly (bl to stub code)
	uint32_t base = 0x9000;
	uint32_t hi = (tvec >> 16) & 0xFFFF;
	uint32_t lo = tvec & 0xFFFF;
	writeBE32(base +  0, 0x3D800000 | hi);         // lis r12, hi
	writeBE32(base +  4, 0x618C0000 | lo);         // ori r12, r12, lo
	writeBE32(base +  8, 0x800C0000);              // lwz r0, 0(r12)
	writeBE32(base + 12, 0x804C0004);              // lwz r2, 4(r12)
	writeBE32(base + 16, 0x7C0903A6);              // mtctr r0
	writeBE32(base + 20, 0x7C0802A6);              // mflr r0
	writeBE32(base + 24, 0x90010008);              // stw r0, 8(r1)
	writeBE32(base + 28, 0x4E800421);              // bctrl
	writeBE32(base + 32, 0x80010008);              // lwz r0, 8(r1)
	writeBE32(base + 36, 0x7C0803A6);              // mtlr r0
	writeBE32(base + 40, 0x4E800020);              // blr

	PPC::SetGPR(1, MemorySize - 256);
	PPC::SetLR(0);
	fprintf(stderr, "  (expect trace output below)\n");
	PPC::Execute(base, 0);

	assert(PPC::GetGPR(3) == 0x42);
	CFMStubs::SetTrace(false);
	printf("PASS (check stderr for trace)\n");
}

static void testRegisterTVector() {
	printf("Test: RegisterTVector for real library exports... ");

	// Simulate a real library TVector at a known address
	uint32_t fakeTvec = 0xA000;
	writeBE32(fakeTvec, 0xDEAD0000);     // code addr
	writeBE32(fakeTvec + 4, 0xBEEF0000); // TOC

	CFMStubs::RegisterTVector("StdCLib", "fprintf", fakeTvec);

	uint32_t resolved = CFMStubs::ResolveImport("StdCLib", "fprintf");
	assert(resolved == fakeTvec);

	printf("PASS\n");
}

static void testAllocateCode() {
	printf("Test: AllocateCode for custom PPC trampolines... ");

	// Allocate a small PPC routine: li r3, 99; blr
	uint32_t code[] = {
		0x38600063, // li r3, 99
		0x4E800020, // blr
	};

	uint32_t tvec = CFMStubs::AllocateCode(code, 2);
	assert(tvec != 0);

	// Verify the TVector points to valid code
	uint32_t codeAddr = memoryReadLong(tvec);
	assert(memoryReadLong(codeAddr) == 0x38600063);
	assert(memoryReadLong(codeAddr + 4) == 0x4E800020);

	// Execute through the TVector
	uint32_t base = 0xB000;
	uint32_t hi = (tvec >> 16) & 0xFFFF;
	uint32_t lo = tvec & 0xFFFF;
	writeBE32(base +  0, 0x3D800000 | hi);
	writeBE32(base +  4, 0x618C0000 | lo);
	writeBE32(base +  8, 0x800C0000);              // lwz r0, 0(r12)
	writeBE32(base + 12, 0x804C0004);              // lwz r2, 4(r12)
	writeBE32(base + 16, 0x7C0903A6);              // mtctr r0
	writeBE32(base + 20, 0x7C0802A6);              // mflr r0
	writeBE32(base + 24, 0x90010008);              // stw r0, 8(r1)
	writeBE32(base + 28, 0x4E800421);              // bctrl
	writeBE32(base + 32, 0x80010008);              // lwz r0, 8(r1)
	writeBE32(base + 36, 0x7C0803A6);              // mtlr r0
	writeBE32(base + 40, 0x4E800020);              // blr

	PPC::SetGPR(1, MemorySize - 256);
	PPC::SetLR(0);
	PPC::Execute(base, 0);

	assert(PPC::GetGPR(3) == 99);
	printf("PASS\n");
}

static void testPEFIntegration(const char *stdclibPath) {
	printf("Test: PEF loader integration with CFM resolver...\n");

	// Register all InterfaceLib/MathLib/PrivateInterfaceLib stubs
	PPCDispatch::RegisterStdCLibImports();
	uint32_t resolved = 0;
	uint32_t catchAll = 0;

	auto resolver = [&](const std::string &lib, const std::string &sym,
	                     uint8_t cls) -> uint32_t {
		uint32_t addr = CFMStubs::ResolveImport(lib, sym);
		if (addr) {
			resolved++;
			return addr;
		}
		// Register catch-all
		catchAll++;
		return CFMStubs::RegisterStub(lib, sym, [lib, sym]() {
			fprintf(stderr, "  UNIMPLEMENTED: %s::%s\n", lib.c_str(), sym.c_str());
			PPC::SetGPR(3, 0);
		});
	};

	PEFLoader::LoadResult result;
	bool ok = PEFLoader::LoadPEFFile(stdclibPath, resolver, result);
	assert(ok);

	printf("  StdCLib loaded: %zu sections, %zu exports\n",
	       result.sections.size(), result.exports.size());
	printf("  Imports: %u pre-registered, %u catch-all\n", resolved, catchAll);

	// All 66 imports should resolve against pre-registered stubs — 0 catch-alls
	assert(resolved + catchAll == 66);
	printf("  Resolved: %u, Catch-all: %u\n", resolved, catchAll);
	assert(catchAll == 0);

	// Register StdCLib exports so tools can import from them
	uint32_t exportCount = 0;
	for (const auto &exp : result.exports) {
		if (exp.sectionIndex < result.sections.size()) {
			uint32_t addr = result.sections[exp.sectionIndex].address + exp.offset;
			CFMStubs::RegisterTVector("StdCLib", exp.name, addr);
			exportCount++;
		}
	}
	printf("  Registered %u StdCLib exports\n", exportCount);

	// Verify we can resolve StdCLib exports
	assert(CFMStubs::ResolveImport("StdCLib", "fprintf") != 0);
	assert(CFMStubs::ResolveImport("StdCLib", "exit") != 0);
	assert(CFMStubs::ResolveImport("StdCLib", "__target_for_exit") != 0);

	printf("  PASS\n");
}

int main(int argc, char *argv[]) {
	Memory = (uint8_t *)aligned_alloc(4096, MemorySize);
	assert(Memory);
	memset(Memory, 0, MemorySize);
	memorySetMemory(Memory, MemorySize);
	MM::Init(Memory, MemorySize, kGlobalSize, kStackSize);
	PPC::Init(Memory, MemorySize);
	PPC::SetSCHandler(CFMStubs::Dispatch);
	CFMStubs::Init();

	testRegisterAndResolve();
	testDispatchThroughPPC();
	testTrace();
	testRegisterTVector();
	testAllocateCode();

	const char *stdclibPath = nullptr;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--stdclib=", 10) == 0) stdclibPath = argv[i] + 10;
	}

	if (stdclibPath) testPEFIntegration(stdclibPath);
	else printf("Skipping PEF integration test (pass --stdclib=<path>)\n");

	PPC::Shutdown();
	free(Memory);
	printf("\nAll CFM stub tests passed.\n");
	return 0;
}
