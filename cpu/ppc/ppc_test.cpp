/*
 * ppc_test.cpp
 *
 * Standalone test for the PPC/Unicorn integration.
 * Build: c++ -std=c++11 -I../.. ppc_test.cpp ppc.cpp -lunicorn -L/opt/homebrew/lib -o ppc_test
 * Or just link via cmake.
 */

#include <cpu/ppc/ppc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

static uint8_t *Memory = nullptr;
static const uint32_t MemorySize = 4 * 1024 * 1024; // 4 MB

// Write a big-endian 32-bit value at the given address.
static void writeBE32(uint32_t addr, uint32_t val) {
	Memory[addr + 0] = (val >> 24) & 0xFF;
	Memory[addr + 1] = (val >> 16) & 0xFF;
	Memory[addr + 2] = (val >>  8) & 0xFF;
	Memory[addr + 3] = (val >>  0) & 0xFF;
}

static uint32_t readBE32(uint32_t addr) {
	return ((uint32_t)Memory[addr + 0] << 24)
	     | ((uint32_t)Memory[addr + 1] << 16)
	     | ((uint32_t)Memory[addr + 2] <<  8)
	     | ((uint32_t)Memory[addr + 3] <<  0);
}

static bool scFired = false;

static void testBasicExecution() {
	printf("Test: basic PPC execution... ");

	// Write a small program at address 0x1000:
	//   li    r3, 42        # r3 = 42
	//   li    r4, 100       # r4 = 100
	//   add   r5, r3, r4    # r5 = r3 + r4 = 142
	//   blr                 # return (PC = LR = 0 → stop)
	uint32_t base = 0x1000;
	writeBE32(base +  0, 0x3860002A); // li r3, 42
	writeBE32(base +  4, 0x38800064); // li r4, 100
	writeBE32(base +  8, 0x7CA32214); // add r5, r3, r4
	writeBE32(base + 12, 0x4E800020); // blr

	PPC::SetLR(0); // sentinel: blr to 0 stops execution
	PPC::Execute(base, 0);

	assert(PPC::GetGPR(3) == 42);
	assert(PPC::GetGPR(4) == 100);
	assert(PPC::GetGPR(5) == 142);
	printf("PASS\n");
}

static void testSCHandler() {
	printf("Test: sc handler dispatch... ");

	scFired = false;
	PPC::SetSCHandler([]() {
		scFired = true;
		uint32_t r3 = PPC::GetGPR(3);
		uint32_t r4 = PPC::GetGPR(4);
		uint32_t r5 = PPC::GetGPR(5);
		printf("(sc: r3=%d r4=%d r5=%d) ", r3, r4, r5);
		assert(r3 == 42);
		assert(r4 == 100);
		assert(r5 == 142);
		// Set a return value
		PPC::SetGPR(3, 999);
	});

	// Program: compute, sc, then return
	//   li    r3, 42
	//   li    r4, 100
	//   add   r5, r3, r4
	//   li    r11, 7        # stub index
	//   sc
	//   blr
	uint32_t base = 0x2000;
	writeBE32(base +  0, 0x3860002A); // li r3, 42
	writeBE32(base +  4, 0x38800064); // li r4, 100
	writeBE32(base +  8, 0x7CA32214); // add r5, r3, r4
	writeBE32(base + 12, 0x39600007); // li r11, 7
	writeBE32(base + 16, 0x44000002); // sc
	writeBE32(base + 20, 0x4E800020); // blr

	PPC::SetLR(0);
	PPC::Execute(base, 0);

	assert(scFired);
	// sc handler set r3 = 999, and our interrupt hook returns via LR
	// so r3 should be 999
	assert(PPC::GetGPR(3) == 999);
	printf("PASS\n");
}

static void testStopFromHandler() {
	printf("Test: Stop() from sc handler... ");

	PPC::SetSCHandler([]() {
		PPC::Stop();
	});

	// Infinite loop that only exits via sc → Stop():
	//   sc
	//   b  -4   (branch back to sc -- should never be reached)
	uint32_t base = 0x3000;
	writeBE32(base + 0, 0x44000002); // sc
	writeBE32(base + 4, 0x4BFFFFFC); // b -4

	PPC::SetLR(0);
	PPC::Execute(base, 0);

	// If we get here, Stop() worked
	printf("PASS\n");
}

static void testMemorySharing() {
	printf("Test: memory sharing (PPC writes, host reads)... ");

	// PPC stores a value into memory; verify host can read it.
	//   lis   r3, 0x0010      # r3 = 0x00100000
	//   li    r4, 0x1234      # r4 = 0x1234
	//   stw   r4, 0(r3)       # store r4 at address 0x00100000
	//   blr
	uint32_t base = 0x4000;
	writeBE32(base +  0, 0x3C600010); // lis r3, 0x0010
	writeBE32(base +  4, 0x38801234); // li r4, 0x1234
	writeBE32(base +  8, 0x90830000); // stw r4, 0(r3)
	writeBE32(base + 12, 0x4E800020); // blr

	PPC::SetSCHandler(nullptr);
	PPC::SetLR(0);
	PPC::Execute(base, 0);

	// Read from host memory — should see big-endian 0x00001234
	uint32_t val = readBE32(0x00100000);
	assert(val == 0x1234);
	printf("PASS\n");
}

static void testMultipleExecutions() {
	printf("Test: multiple Execute() calls... ");

	PPC::SetSCHandler(nullptr);

	// First call: set r3 = 10
	uint32_t base = 0x5000;
	writeBE32(base + 0, 0x3860000A); // li r3, 10
	writeBE32(base + 4, 0x4E800020); // blr
	PPC::SetLR(0);
	PPC::Execute(base, 0);
	assert(PPC::GetGPR(3) == 10);

	// Second call: set r3 = 20
	base = 0x5100;
	writeBE32(base + 0, 0x38600014); // li r3, 20
	writeBE32(base + 4, 0x4E800020); // blr
	PPC::SetLR(0);
	PPC::Execute(base, 0);
	assert(PPC::GetGPR(3) == 20);

	printf("PASS\n");
}

static void testTraceCode() {
	printf("Test: instruction tracing... ");

	PPC::SetTraceCode(true);
	PPC::SetSCHandler(nullptr);

	uint32_t base = 0x6000;
	writeBE32(base + 0, 0x38600001); // li r3, 1
	writeBE32(base + 4, 0x4E800020); // blr

	PPC::SetLR(0);
	fprintf(stderr, "\n");
	PPC::Execute(base, 0);

	PPC::SetTraceCode(false);
	assert(PPC::GetGPR(3) == 1);
	printf("PASS (check stderr for trace output)\n");
}

int main() {
	Memory = (uint8_t *)aligned_alloc(4096, MemorySize);
	if (!Memory) {
		fprintf(stderr, "Failed to allocate memory\n");
		return 1;
	}
	memset(Memory, 0, MemorySize);

	PPC::Init(Memory, MemorySize);

	testBasicExecution();
	testSCHandler();
	testStopFromHandler();
	testMemorySharing();
	testMultipleExecutions();
	testTraceCode();

	PPC::Shutdown();
	free(Memory);

	printf("\nAll PPC tests passed.\n");
	return 0;
}
