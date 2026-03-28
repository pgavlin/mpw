// clang++ -c -std=c++11 -stdlib=libc++ -Wno-deprecated-declarations loader.cpp
/*
 * Copyright (c) 2013, Kelvin W Sherlock
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <cstdint>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <chrono>

#include <sysexits.h>
#include <getopt.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cpu/m68k/defs.h>
#include <cpu/m68k/CpuModule.h>
#include <cpu/m68k/fmem.h>

extern "C" void cpuSetRaiseInterrupt(BOOLE raise_irq);

#include <toolbox/toolbox.h>
#include <toolbox/mm.h>
#include <toolbox/os.h>
#include <toolbox/rm.h>
#include <toolbox/path_utils.h>
#include <toolbox/loader.h>
#include <toolbox/pef_loader.h>
#include <toolbox/cfm_stubs.h>
#include <toolbox/ppc_dispatch.h>

#include <cpu/ppc/ppc.h>

#include <mpw/mpw.h>

#include <mplite/mplite.h>

#include <macos/sysequ.h>
#include <macos/traps.h>

#include "loader.h"
#include "debugger.h"
#include "profiler.h"

#include <cxx/string_splitter.h>


Settings Flags;
static Profiler *profiler = nullptr;

const uint32_t kGlobalSize = 0x10000;
// retained to make debugging easier.
uint8_t *Memory = nullptr;
uint32_t MemorySize = 0;


uint8_t ReadByte(const void *data, uint32_t offset)
{
	offset &= 0xffffff;
	return ((uint8_t *)data)[offset];
}

uint16_t ReadWord(const void *data, uint32_t offset)
{
	offset &= 0xffffff;
	return (ReadByte(data, offset) << 8) | ReadByte(data, offset+1);
}

uint32_t ReadLong(const void *data, uint32_t offset)
{
	offset &= 0xffffff;
	return (ReadWord(data, offset) << 16) | ReadWord(data, offset+2);
}

void WriteByte(void *data, uint32_t offset, uint8_t value)
{
	offset &= 0xffffff;
	((uint8_t *)data)[offset] = value;
}

void WriteWord(void *data, uint32_t offset, uint16_t value)
{
	offset &= 0xffffff;

	((uint8_t *)data)[offset++] = value >> 8;
	((uint8_t *)data)[offset++] = value;
}

void WriteLong(void *data, uint32_t offset, uint32_t value)
{
	offset &= 0xffffff;

	((uint8_t *)data)[offset++] = value >> 24;
	((uint8_t *)data)[offset++] = value >> 16;
	((uint8_t *)data)[offset++] = value >> 8;
	((uint8_t *)data)[offset++] = value;
}



void GlobalInit()
{
	// todo -- move this somewhere better.


	// 0x031a - Lo3Bytes
	memoryWriteLong(0x00ffffff, MacOS::Lo3Bytes);

	// 0x0a02 - OneOne
	memoryWriteLong(0x00010001, MacOS::OneOne);

	// 0x0a06 - MinusOne
	memoryWriteLong(0xffffffff, MacOS::MinusOne);


	// 0x0130 -- ApplLimit
	memoryWriteLong(Flags.memorySize - Flags.stackSize - 1, MacOS::ApplLimit);
	memoryWriteLong(kGlobalSize, MacOS::ApplZone);
	memoryWriteLong(Flags.memorySize - 1, MacOS::BufPtr);

	//
	memoryWriteLong(Flags.stackRange.first, MacOS::CurStackBase);

}


void CreateStack()
{
	// allocate stack, set A7...

	uint32_t address;
	//uint16_t error;

#if 0
	Flags.stackSize = (Flags.stackSize + 3) & ~0x03;

	error = MM::Native::NewPtr(Flags.stackSize, true, address);
	if (error)
	{
		fprintf(stderr, "Unable to allocate stack (%08x bytes)\n", Flags.stackSize);
		exit(EX_CONFIG);
	}
#else

	address = Flags.memorySize - Flags.stackSize;

#endif

	Flags.stackRange.first = address;
	Flags.stackRange.second = address + Flags.stackSize;

	// TODO -- is there a global for the max (min) stack pointer?

	// address grows down
	// -4 is for the return address.
	cpuSetAReg(7, Flags.stackRange.second - 4);
	// return address.
	memoryWriteLong(MacOS::MinusOne, Flags.stackRange.second - 4); // MinusOne Global -- 0xffff ffff
}



void LogToolBox(uint32_t pc, uint16_t trap)
{
	const char *name;

	name = TrapName(trap);

	if (name)
	{
		fprintf(stderr, "$%08X   %-51s ; %04X\n", pc, name, trap);
	}
	else
	{
		fprintf(stderr, "$%08X   Tool       #$%04X                                   ; %04X\n", pc, trap, trap);
	}
}

void InstructionLogger()
{

	static char strings[4][256];
	for (unsigned j = 0; j < 4; ++j) strings[j][0] = 0;

	uint32_t pc = cpuGetPC();
	uint16_t opcode = ReadWord(Memory, pc);

	if ((opcode & 0xf000) == 0xa000)
	{
		LogToolBox(pc, opcode);
		return;
	}


	if (Flags.traceCPU)
	{
		cpuDisOpcode(pc, strings[0], strings[1], strings[2], strings[3]);

		// address, data, instruction, operand
		fprintf(stderr, "%s   %-10s %-40s ; %s\n", strings[0], strings[2], strings[3], strings[1]);

		// todo -- trace registers (only print changed registers?)

		#if 0
		if (pc >= 0x00010E94 && pc <= 0x00010FC0)
		{
			fprintf(stderr, "d7 = %08x\n", cpuGetDReg(7));
		}
		#endif
	}

	int mboffset = 0;
	switch (opcode)
	{
		case 0x4E75: // rts
		case 0x4ED0: // jmp (a0)
			mboffset = 2;
			break;
		case 0x4E74: // rtd #
			mboffset = 4;
			break;
	}

	if (mboffset) // RTS or JMP (A0)
	{
		pc += mboffset;
		// check for MacsBug name after rts.
		std::string s;
		unsigned b = memoryReadByte(pc);
		if (b >= 0x80 && b <= 0x9f)
		{
			b -= 0x80;
			pc++;

			if (!b) b = memoryReadByte(pc++);

			s.reserve(b);
			for (unsigned i = 0; i < b; ++i)
			{
				s.push_back(memoryReadByte(pc++));
			}
			fprintf(stderr, "%s\n\n", s.c_str());
		}
	}

}

void MemoryLogger(uint32_t address, int size, int readWrite, uint32_t value)
{
	if (address < kGlobalSize)
	{
		const char *name = GlobalName(address);
		if (!name) name = "unknown";

		fprintf(stderr, "%-20s %08x - ", name, address);

		if (!readWrite)
		{
			switch(size)
			{
			case 1:
				value = ReadByte(Memory, address);
				break;
			case 2:
				value = ReadWord(Memory, address);
				break;
			case 4:
				value = ReadLong(Memory, address);
				break;
			}
		}


		// todo -- for write, display previous value?
		fprintf(stderr, " %s %d bytes", readWrite ? "write" : "read ", size);
		switch(size)
		{
			case 1:
				fprintf(stderr, " [%02x]\n", value);
				break;
			case 2:
				fprintf(stderr, " [%04x]\n", value);
				break;
			case 3:
				fprintf(stderr, " [%06x]\n", value);
				break;
			case 4:
				fprintf(stderr, " [%08x]\n", value);
				break;
			default:
				fprintf(stderr, "\n");
				break;
		}

	}
}

void MidInstructionExceptionFunc()
{
	// todo - cpu exception?
	fprintf(stderr, "Mid Instruction Exception!\n");
	//throw std::runtime_error::runtime_error("mid instruction exception");
}


#define MPW_VERSION "0.8.3"
void help()
{
	printf("MPW " MPW_VERSION "\n");
	printf("Usage: mpw [options] utility ...\n");
	printf("\n");
	printf(" --help              display usage information\n");
	printf(" --trace-cpu         print cpu information\n");
	printf(" --trace-macsbug     print macsbug names\n");
	printf(" --trace-toolbox     print toolbox calls\n");
	printf(" --trace-mpw         print mpw calls\n");
	printf(" --ppc               force PPC execution (PEF data fork)\n");
	printf(" --68k               force 68K execution (CODE resources)\n");
	printf(" --memory-stats      print memory usage information\n");
	printf(" --profile           generate callgrind profiling output\n");
	printf(" --profile-cycles    use estimated PPC cycle counts (default: instruction count)\n");
	printf(" --profile-output=F  set profile output filename\n");
	printf(" --ram=<number>      set the ram size.  Default=16M\n");
	printf(" --stack=<number>    set the stack size.  Default=8K\n");
	printf("\n");
}

bool parse_number(const char *input, uint32_t *dest)
{
	char *end;
	long value;
	int base = 0;

	// octal is dumb so don't allow it.

	while (isspace(*input)) ++input;
	if (*input == '0' && isdigit(input[1])) base = 10;

	errno = 0;
	value = strtol(input, &end, base);

	if (errno || value < 0 || input == end)
	{
		fprintf(stderr, "%s - invalid input\n", input);
		return false;
	}

	// M/K
	if (*end)
	{
		int old = value;
		if (strcasecmp(end, "M") == 0 || strcasecmp(end, "MB") == 0)
			value *= 1024 * 1024;
		else if (strcasecmp(end, "K") == 0 || strcasecmp(end, "KB") == 0)
			value *= 1024;
		else
		{
			fprintf(stderr, "%s - invalid input\n", input);
			return false;
		}
		if (value < old)
		{
			// overflow
			fprintf(stderr, "%s - invalid input\n", input);
			return false;
		}
	}

	if (dest) *dest = value;
	return true;
}

bool file_exists(const std::string & name)
{
	struct stat st;
	std::string resolved = OS::resolve_path_ci(name);
	return ::stat(resolved.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string find_file(const std::string &name)
{
	struct stat st;
	std::string resolved = OS::resolve_path_ci(name);
	if (::stat(resolved.c_str(), &st) == 0 && S_ISREG(st.st_mode))
		return resolved;
	return std::string();
}

std::string old_find_exe(const std::string &name)
{
	std::string found = find_file(name);
	if (!found.empty()) return found;

	// if name is a path, then it doesn't exist.
	if (name.find('/') != name.npos) return std::string();

	std::string path = MPW::RootDir();
	if (path.empty()) return path;


	if (path.back() != '/') path.push_back('/');
	path.append("Tools/");
	path.append(name);

	return find_file(path);

}


// this needs to run *after* the MPW environment variables are loaded.
std::string find_exe(const std::string &name)
{

	// if this is an absolute or relative name, return as-is.

	if (name.find(':') != name.npos) {
		std::string path = ToolBox::MacToUnix(name);
		return find_file(path);
	}

	if (name.find('/') != name.npos) {
		return find_file(name);
	}

	// otherwise, check the Commands variable for locations.
	std::string commands = MPW::GetEnv("Commands");
	if (commands.empty()) return old_find_exe(name);


	// string is , separated, possibly in MacOS format.

	for (auto iter = string_splitter(commands, ','); iter; ++iter)
	{
		if (iter->empty()) continue;
		std::string path = *iter;

		// convert to unix.
		path = ToolBox::MacToUnix(path);
		// should always have a length...
		if (path.length() && path.back() != '/') path.push_back('/');
		path.append(name);
		std::string found = find_file(path);
		if (!found.empty()) return found;
	}

	return "";
}


void MainLoop()
{
	bool useBatch = !profiler && !Flags.traceCPU && !Flags.traceMacsbug;

	if (useBatch) {
		// Batch execution: run at full JIT speed, stopping only at
		// A-line/F-line traps via Unicorn's exits mechanism.
		cpuEnableBatchMode();
		for (;;) {
			if (cpuGetStop()) break;

			uint32_t pc = cpuGetPC();
			if (pc == 0x00000000) {
				fprintf(stderr, "Exiting - PC = 0\n");
				exit(EX_SOFTWARE);
			}

			if (!cpuRunBatch()) break;

			// Check stack bounds at trap boundaries
			uint32_t sp = cpuGetAReg(7);
			if (sp < Flags.stackRange.first) {
				fprintf(stderr, "Stack overflow error - please increase the stack size (--stack=size)\n");
				fprintf(stderr, "Current stack size is 0x%06x\n", Flags.stackSize);
				exit(EX_SOFTWARE);
			}
			if (sp > Flags.stackRange.second) {
				fprintf(stderr, "Stack underflow error\n");
				exit(EX_SOFTWARE);
			}
		}
		cpuDisableBatchMode();
	} else {
		// Single-step execution: used when profiling or tracing is active.
		uint64_t cycles = 0;
		for (;;)
		{
			uint32_t pc = cpuGetPC();
			uint32_t sp = cpuGetAReg(7);

			if (pc == 0x00000000)
			{
				fprintf(stderr, "Exiting - PC = 0\n");
				exit(EX_SOFTWARE);
			}

			if (sp < Flags.stackRange.first)
			{
				fprintf(stderr, "Stack overflow error - please increase the stack size (--stack=size)\n");
				fprintf(stderr, "Current stack size is 0x%06x\n", Flags.stackSize);
				exit(EX_SOFTWARE);
			}

			if (sp > Flags.stackRange.second)
			{
				fprintf(stderr, "Stack underflow error\n");
				exit(EX_SOFTWARE);
			}

			if (cpuGetStop()) break;

			uint16_t opcode = 0;
			if (profiler) {
				opcode = ReadWord(Memory, pc);
				profiler->beforeInstruction(pc, opcode);
			}

			#ifndef CPU_INSTRUCTION_LOGGING
			if (Flags.traceCPU || Flags.traceMacsbug)
			{
				InstructionLogger();
			}
			#endif

			uint32_t icycles = cpuExecuteInstruction();
			cycles += icycles;

			if (profiler)
				profiler->afterInstruction(pc, icycles);
		}
	}
}

// -- PPC support --

static bool IsPEFFile(const std::string &path) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	uint8_t header[8];
	size_t n = fread(header, 1, 8, f);
	fclose(f);
	return n == 8 && PEFLoader::IsPEF(header, 8);
}

static std::string findSharedLibrary(const std::string &name) {
	std::string mpwRoot = MPW::RootDir();

	std::vector<std::string> searchPaths = {
		mpwRoot + "/Libraries/SharedLibraries/" + name,
		mpwRoot + "/SharedLibraries/" + name,
		mpwRoot + "/Libraries/" + name,
	};

	for (const auto &path : searchPaths) {
		struct stat st;
		if (stat(path.c_str(), &st) == 0) return path;
	}

	return "";
}

static void PPCCallFunction(uint32_t tvecAddr) {
	uint32_t codeAddr = memoryReadLong(tvecAddr);
	uint32_t toc = memoryReadLong(tvecAddr + 4);

	// Set up PPC stack pointer at top of stack area.
	// The stack area is the last stackSize bytes of memory.
	// mplite's aCtrl array sits just below the stack area, so we must
	// NOT place SP below (memorySize - stackSize) or we'll corrupt it.
	uint32_t ppcStackTop = Flags.memorySize - 64;
	ppcStackTop &= ~0xF; // 16-byte aligned
	PPC::SetGPR(1, ppcStackTop);
	PPC::SetGPR(2, toc);
	PPC::SetLR(0); // sentinel: blr to 0 stops execution

	PPC::Execute(codeAddr, toc);
}

static void RunPPC(int argc, char **argv, const std::string &command) {
	// Initialize PPC subsystem
	PPC::Init(Memory, MemorySize);
	CFMStubs::Init();
	PPC::SetSCHandler(CFMStubs::Dispatch);
	PPCDispatch::RegisterStdCLibImports();

	// Wire trace flags
	if (Flags.traceCPU) PPC::SetTraceCode(true);
	if (Flags.traceToolBox) {
		CFMStubs::SetTrace(true);
		PEFLoader::SetTrace(true);
	}
	MPW::Trace = Flags.traceMPW;

	// Track loaded libraries
	std::map<std::string, PEFLoader::LoadResult> loadedLibs;

	// Resolver that loads real libraries on demand
	std::function<uint32_t(const std::string &, const std::string &, uint8_t)> resolver;
	resolver = [&](const std::string &lib, const std::string &sym,
	               uint8_t cls) -> uint32_t {
		// Check if already resolved (CFM stubs or previously loaded library)
		uint32_t addr = CFMStubs::ResolveImport(lib, sym);
		if (addr) return addr;

		// Try to load the library PEF if not yet loaded
		if (loadedLibs.find(lib) == loadedLibs.end()) {
			std::string libPath = findSharedLibrary(lib);
			if (!libPath.empty()) {
				if (Flags.traceToolBox)
					fprintf(stderr, "PPC: loading shared library %s from %s\n",
					        lib.c_str(), libPath.c_str());

				PEFLoader::LoadResult libResult;
				bool ok = PEFLoader::LoadPEFFile(libPath, resolver, libResult);
				if (ok) {
					for (const auto &exp : libResult.exports) {
						if (exp.sectionIndex < libResult.sections.size()) {
							uint32_t a = libResult.sections[exp.sectionIndex].address
							             + exp.offset;
							CFMStubs::RegisterTVector(lib, exp.name, a);
						}
					}
					loadedLibs[lib] = std::move(libResult);

					addr = CFMStubs::ResolveImport(lib, sym);
					if (addr) return addr;
				}
			} else {
				if (Flags.traceToolBox)
					fprintf(stderr, "PPC: shared library %s not found\n", lib.c_str());
			}
		}

		// Register catch-all for truly unresolved imports
		return CFMStubs::RegisterStub(lib, sym, [lib, sym]() {
			fprintf(stderr, "PPC FATAL: unimplemented stub %s::%s called\n",
			        lib.c_str(), sym.c_str());
			fprintf(stderr, "  r3=0x%08X r4=0x%08X r5=0x%08X LR=0x%08X\n",
			        PPC::GetGPR(3), PPC::GetGPR(4), PPC::GetGPR(5), PPC::GetLR());
			PPC::Stop();
		});
	};

	// Load tool PEF
	PEFLoader::LoadResult toolResult;
	bool ok = PEFLoader::LoadPEFFile(command, resolver, toolResult);
	if (!ok) {
		fprintf(stderr, "PPC: failed to load %s\n", command.c_str());
		exit(EX_SOFTWARE);
	}

	// Patch MPGM device table and IO table for PPC:
	// Must happen after PEF loading (which allocates sections) but before
	// library init (which reads the device/IO tables).
	{
		uint32_t mpgmHdr = memoryReadLong(0x0316);
		uint32_t mpgmInfo = memoryReadLong(mpgmHdr + 4);
		PPCDispatch::PatchDeviceTable(mpgmInfo);
	}

	// Run library init routines
	for (auto &kv : loadedLibs) {
		if (kv.second.initPoint) {
			if (Flags.traceToolBox)
				fprintf(stderr, "PPC: running %s __initialize\n", kv.first.c_str());
			PPCCallFunction(kv.second.initPoint);
		}
	}

	// Set up profiler if requested
	if (Flags.profile) {
		// Collect symbol names from all loaded libraries and tool
		std::map<std::string, uint32_t> symbols;
		auto exportAddr = [](const PEFLoader::LoadResult &lr,
		                     const PEFLoader::ExportedSymbolInfo &exp) -> uint32_t {
			if (exp.sectionIndex < lr.sections.size())
				return lr.sections[exp.sectionIndex].address + exp.offset;
			return 0;
		};
		for (auto &kv : loadedLibs) {
			for (auto &exp : kv.second.exports) {
				uint32_t addr = exportAddr(kv.second, exp);
				if (addr) symbols[kv.first + "::" + exp.name] = addr;
			}
		}
		for (auto &exp : toolResult.exports) {
			uint32_t addr = exportAddr(toolResult, exp);
			if (addr) symbols[exp.name] = addr;
		}
		// Add CFM stub names
		auto stubNames = CFMStubs::GetStubNames();
		for (auto &kv : stubNames) {
			symbols[kv.second] = kv.first;
		}

		uint32_t entryCode = memoryReadLong(toolResult.entryPoint);

		profiler = new Profiler();
		profiler->initializePPC(entryCode, symbols);

		bool useCycles = Flags.profileCycles;
		uint32_t prevPC = 0;
		uint32_t prevInstr = 0;
		bool first = true;

		PPC::SetCodeHook([&](uint32_t pc, uint32_t instr) {
			if (!first) {
				uint32_t cost = useCycles ? Profiler::ppcEstimateCycles(prevInstr) : 1;
				profiler->afterPPCInstruction(prevPC, cost);
			}
			profiler->beforePPCInstruction(pc, instr);
			prevPC = pc;
			prevInstr = instr;
			first = false;
		});
	}

	// Run tool entry point
	if (toolResult.entryPoint) {
		if (Flags.traceToolBox)
			fprintf(stderr, "PPC: running tool entry point\n");

		if (Flags.debugger) {
			// Set up PPC state but don't execute — let the debugger control it
			uint32_t codeAddr = memoryReadLong(toolResult.entryPoint);
			uint32_t toc = memoryReadLong(toolResult.entryPoint + 4);
			uint32_t ppcStackTop = Flags.memorySize - 64;
			ppcStackTop &= ~0xF;
			PPC::SetGPR(1, ppcStackTop);
			PPC::SetGPR(2, toc);
			PPC::SetLR(0);
			PPC::SetPC(codeAddr);

			Debug::SetPPCMode(true);
			Debug::Shell();
		} else {
			PPCCallFunction(toolResult.entryPoint);
		}
	} else {
		fprintf(stderr, "PPC: no entry point in %s\n", command.c_str());
		exit(EX_SOFTWARE);
	}

	// Write profiler output
	if (profiler) {
		std::string profileFile = Flags.profileOutput.empty()
		                          ? "mpw.callgrind" : Flags.profileOutput;
		profiler->writeOutput(profileFile);
		delete profiler;
		profiler = nullptr;
	}

	if (Flags.memoryStats) {
		MM::Native::PrintMemoryStats();
	}

	// Capture exit code from MPGM info+0x0E
	uint32_t rv = MPW::ExitStatus();
	if (rv > 0xff) rv = 0xff;
	exit(rv);
}

int main(int argc, char **argv)
{
	// getopt...

	enum
	{
		kTraceCPU = 1,
		kTraceMacsBug,
		kTraceGlobals,
		kTraceToolBox,
		kTraceMPW,
		kDebugger,
		kMemoryStats,
		kProfile,
		kProfileOutput,
		kProfileCycles,
		kShell,
		kForcePPC,
		kForce68K,
	};
	static struct option LongOpts[] =
	{
		{ "ram",required_argument, NULL, 'r' },
		{ "stack", required_argument, NULL, 's' },
		{ "machine", required_argument, NULL, 'm' },
		{ "trace-cpu", no_argument, NULL, kTraceCPU },
		{ "trace-macsbug", no_argument, NULL, kTraceMacsBug },
		{ "trace-globals", no_argument, NULL, kTraceGlobals },
		{ "trace-toolbox", no_argument, NULL, kTraceToolBox },
		{ "trace-tools", no_argument, NULL, kTraceToolBox },
		{ "trace-mpw", no_argument, NULL, kTraceMPW },

		{ "debug", no_argument, NULL, kDebugger },
		{ "debugger", no_argument, NULL, kDebugger },

		{ "memory-stats", no_argument, NULL, kMemoryStats },
		{ "profile", no_argument, NULL, kProfile },
		{ "profile-output", required_argument, NULL, kProfileOutput },
		{ "profile-cycles", no_argument, NULL, kProfileCycles },

		{ "ppc", no_argument, NULL, kForcePPC },
		{ "68k", no_argument, NULL, kForce68K },

		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "shell", no_argument, NULL, kShell },
		{ NULL, 0, NULL, 0 }
	};

	//auto start_time = std::chrono::high_resolution_clock::now();

	std::vector<std::string> defines;

	int c;
	while ((c = getopt_long(argc, argv, "+hVm:r:s:D:", LongOpts, NULL)) != -1)
	{
		switch(c)
		{
			case kTraceCPU:
				Flags.traceCPU = true;
				break;

			case kTraceMacsBug:
				Flags.traceMacsbug = true;
				break;

			case kTraceGlobals:
				Flags.traceGlobals = true;
				break;

			case kTraceToolBox:
				Flags.traceToolBox = true;
				break;

			case kTraceMPW:
				Flags.traceMPW = true;
				break;

			case kMemoryStats:
				Flags.memoryStats = true;
				break;

			case kProfile:
				Flags.profile = true;
				break;

			case kProfileOutput:
				Flags.profileOutput = optarg;
				break;

			case kProfileCycles:
				Flags.profileCycles = true;
				break;

			case kDebugger:
				Flags.debugger = true;
				break;

			case kForcePPC:
				Flags.forcePPC = true;
				break;

			case kForce68K:
				Flags.force68K = true;
				break;

			case kShell:
				break;

			case 'm':
				if (!parse_number(optarg, &Flags.machine))
					exit(EX_CONFIG);
				break;

			case 'r':
				if (!parse_number(optarg, &Flags.memorySize))
					exit(EX_CONFIG);
				break;

			case 's':
				if (!parse_number(optarg, &Flags.stackSize))
					exit(EX_CONFIG);
				break;

			case 'D':
				defines.push_back(optarg);
				break;

			case ':':
			case '?':
				help();
				exit(EX_USAGE);
				break;

			case 'h':
				help();
				exit(EX_OK);
				break;

			case 'V':
				printf("mpw version " MPW_VERSION "\n");
				exit(EX_OK);
				break;
		}

	}

	argc -= optind;
	argv += optind;

	if (!argc)
	{
		help();
		exit(EX_USAGE);
	}

	Flags.stackSize = (Flags.stackSize + 0xff) & ~0xff;
	Flags.memorySize = (Flags.memorySize + 0xfff) & ~0xfff; // page-align for Unicorn

	if (Flags.stackSize < 0x100)
	{
		fprintf(stderr, "Invalid stack size\n");
		exit(EX_CONFIG);
	}

	if (Flags.memorySize < 0x200)
	{
		fprintf(stderr, "Invalid ram size\n");
		exit(EX_CONFIG);
	}

	if (Flags.stackSize >= Flags.memorySize)
	{
		fprintf(stderr, "Invalid stack/ram size\n");
		exit(EX_CONFIG);
	}



	MPW::InitEnvironment(defines);

	std::string command(argv[0]); // InitMPW updates argv...
	command = find_exe(command);
	if (command.empty())
	{
		std::string mpw = MPW::RootDir();
		fprintf(stderr, "Unable to find command %s\n", argv[0]);
		fprintf(stderr, "$MPW = %s\n", mpw.c_str());
		fprintf(stderr, "$Commands = %s\n", MPW::GetEnv("Commands").c_str());
		exit(EX_USAGE);
	}
	argv[0] = ::strdup(command.c_str()); // hmm.. could setenv(mpw_command) instead.


	// move to CreateRam()
	// Use aligned_alloc for page alignment (required by Unicorn's uc_mem_map_ptr).
	Memory = Flags.memory = (uint8_t *)aligned_alloc(4096, Flags.memorySize);
	memset(Memory, 0, Flags.memorySize);
	MemorySize = Flags.memorySize;


	/// ahhh... need to set PC after memory.
	// for pre-fetch.
	memorySetMemory(Memory, MemorySize);


	MM::Init(Memory, MemorySize, kGlobalSize, Flags.stackSize);
	OS::Init();
	ToolBox::Init();
	MPW::Init(argc, argv);

	// Detect PPC vs 68K.
	// --ppc/--68k force the mode. Otherwise, try to load CODE resources
	// (68K). If that fails, check for a PEF data fork and use PPC.
	bool usePPC;
	if (Flags.forcePPC) usePPC = true;
	else if (Flags.force68K) usePPC = false;
	else {
		// Auto-detect: check if the file has CODE resources (68K).
		// If not, check for PEF magic in the data fork.
		int16_t refNum = -1;
		uint16_t err = RM::Native::OpenResourceFile(command, 1, refNum);
		bool hasCode = false;
		if (!err) {
			uint32_t codeHandle = 0;
			hasCode = (RM::Native::GetResource(0x434F4445 /*'CODE'*/, 0, codeHandle) == 0
			           && codeHandle != 0);
			RM::Native::CloseResFile(refNum);
		}
		if (hasCode) {
			usePPC = false;
		} else {
			// No CODE resources — check for PEF data fork.
			std::string resolved = OS::resolve_path_ci(command);
			FILE *f = fopen(resolved.c_str(), "rb");
			usePPC = false;
			if (f) {
				uint8_t magic[8];
				if (fread(magic, 1, 8, f) == 8) {
					// PEF magic: 'Joy!' 'peff'
					usePPC = (magic[0] == 'J' && magic[1] == 'o' &&
					          magic[2] == 'y' && magic[3] == '!' &&
					          magic[4] == 'p' && magic[5] == 'e' &&
					          magic[6] == 'f' && magic[7] == 'f');
				}
				fclose(f);
			}
			if (!usePPC) {
				fprintf(stderr, "Unable to load command %s: no CODE resources or PEF data fork\n",
				        command.c_str());
				exit(EX_SOFTWARE);
			}
		}
	}

	if (usePPC) {
		// 68K CPU still needs basic init for Gestalt bridge etc.
		cpuStartup();
		cpuSetModel(3, 0);

		RunPPC(argc, argv, command);

		uint32_t rv = MPW::ExitStatus();
		if (rv > 0xff) rv = 0xff;
		exit(rv);
	}

	// -- 68K path (unchanged) --

	cpuStartup();
	cpuSetRaiseInterrupt(FALSE);
	cpuSetModel(3,0);

	CreateStack();

	uint16_t err = Loader::Native::LoadFile(command);
	if (err) {
		const char *cp = ErrorName(err);
		fprintf(stderr, "Unable to load command %s: ", command.c_str());
		if (cp) printf("%s\n", cp);
		else printf("%hd\n", err);
		exit(EX_SOFTWARE);
	}
	GlobalInit();


	cpuSetALineExceptionFunc(ToolBox::dispatch);
	cpuSetFLineExceptionFunc(MPW::dispatch);

	cpuSetMidInstructionExceptionFunc(MidInstructionExceptionFunc);


	if (Flags.traceGlobals) //memorySetGlobalLog(kGlobalSize);
		memorySetLoggingFunc(MemoryLogger);

	MPW::Trace = Flags.traceMPW;
	ToolBox::Trace = Flags.traceToolBox;


	if (Flags.traceCPU || Flags.traceMacsbug)
	{
		#ifdef CPU_INSTRUCTION_LOGGING
		cpuSetInstructionLoggingFunc(InstructionLogger);
		#endif
		// else do it manually below.
	}

	if (Flags.profile) {
		profiler = new Profiler();
		profiler->initialize();
	}

	if (Flags.debugger) Debug::Shell();
	else MainLoop();



	if (Flags.memoryStats)
	{
		MM::Native::PrintMemoryStats();
	}

	if (profiler) {
		std::string profileFile = Flags.profileOutput;
		if (profileFile.empty()) {
			char buf[64];
			snprintf(buf, sizeof(buf), "callgrind.out.%d", getpid());
			profileFile = buf;
		}
		profiler->writeOutput(profileFile);
		delete profiler;
		profiler = nullptr;
	}

	uint32_t rv = MPW::ExitStatus();
	if (rv > 0xff) rv = 0xff;

	exit(rv);
}
