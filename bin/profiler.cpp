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

#include "profiler.h"

#include <cstdio>
#include <map>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include <cpu/m68k/defs.h>
#include <cpu/m68k/CpuModule.h>
#include <cpu/m68k/fmem.h>
#include <cpu/ppc/ppc.h>

#include <toolbox/loader.h>
#include <macos/traps.h>

struct CallFrame {
	uint32_t functionAddr;
	uint32_t callSitePC;
	uint32_t returnAddr;
	uint64_t selfCycles;
	uint64_t inclusiveCycles;
	bool isTrap;
};

struct CallArc {
	uint64_t count;
	uint64_t inclusiveCost;
};

struct FunctionInfo {
	std::string name;
	uint32_t startAddr;
	uint64_t selfCycles;
	uint64_t callCount;
};

struct Profiler::Impl {
	std::unordered_map<uint32_t, uint64_t> instrCycles;
	std::unordered_map<uint32_t, FunctionInfo> functions;
	std::map<std::pair<uint32_t,uint32_t>, CallArc> callArcs;
	std::vector<CallFrame> callStack;

	// addr -> name from debug symbols
	std::map<uint32_t, std::string> addrToName;
	// addr -> end addr (if known)
	std::map<uint32_t, uint32_t> addrToEnd;

	// loaded CODE segments (excluding segment 0)
	std::vector<Loader::CodeSegment> segments;

	bool pendingCall = false;
	bool pendingReturn = false;
	bool pendingTrap = false;
	uint32_t callSitePC = 0;
	uint16_t trapOpcode = 0;

	const std::string &functionName(uint32_t addr);
	uint32_t findFunction(uint32_t pc);
	void pushFrame(uint32_t funcAddr, uint32_t callSite, uint32_t returnAddr, bool isTrap);
	void popFramesTo(uint32_t returnAddr);
	void flushStack();
};

const std::string &Profiler::Impl::functionName(uint32_t addr) {
	auto &fi = functions[addr];
	if (fi.name.empty()) {
		auto it = addrToName.find(addr);
		if (it != addrToName.end()) {
			fi.name = it->second;
		} else {
			char buf[20];
			snprintf(buf, sizeof(buf), "sub_%08X", addr);
			fi.name = buf;
		}
		fi.startAddr = addr;
	}
	return fi.name;
}

uint32_t Profiler::Impl::findFunction(uint32_t pc) {
	// find the function containing pc by looking at addrToEnd or
	// finding the nearest symbol <= pc
	auto it = addrToName.upper_bound(pc);
	if (it != addrToName.begin()) {
		--it;
		uint32_t start = it->first;
		auto endIt = addrToEnd.find(start);
		if (endIt != addrToEnd.end() && endIt->second > 0 && pc < endIt->second) {
			return start;
		}
		// if no end info, assume it's close enough
		if (endIt == addrToEnd.end() || endIt->second == 0) {
			return start;
		}
	}
	return pc; // unknown - treat pc as its own function
}

void Profiler::Impl::pushFrame(uint32_t funcAddr, uint32_t callSite, uint32_t returnAddr, bool isTrap) {
	CallFrame frame;
	frame.functionAddr = funcAddr;
	frame.callSitePC = callSite;
	frame.returnAddr = returnAddr;
	frame.selfCycles = 0;
	frame.inclusiveCycles = 0;
	frame.isTrap = isTrap;

	functionName(funcAddr); // ensure entry exists
	functions[funcAddr].callCount++;
	callStack.push_back(frame);
}

void Profiler::Impl::popFramesTo(uint32_t returnAddr) {
	// pop frames until we find one whose returnAddr matches,
	// or the stack is empty (handles longjmp)
	while (!callStack.empty()) {
		CallFrame &top = callStack.back();
		uint64_t inclusive = top.inclusiveCycles + top.selfCycles;

		functions[top.functionAddr].selfCycles += top.selfCycles;

		// record call arc
		if (callStack.size() > 1) {
			uint32_t callerFunc = callStack[callStack.size() - 2].functionAddr;
			auto key = std::make_pair(callerFunc, top.functionAddr);
			auto &arc = callArcs[key];
			arc.count++;
			arc.inclusiveCost += inclusive;

			// propagate inclusive cost to caller
			callStack[callStack.size() - 2].inclusiveCycles += inclusive;
		}

		bool matched = (top.returnAddr == returnAddr);
		callStack.pop_back();
		if (matched) break;
	}
}

void Profiler::Impl::flushStack() {
	// flush remaining frames at shutdown
	while (!callStack.empty()) {
		CallFrame &top = callStack.back();
		uint64_t inclusive = top.inclusiveCycles + top.selfCycles;

		functions[top.functionAddr].selfCycles += top.selfCycles;

		if (callStack.size() > 1) {
			uint32_t callerFunc = callStack[callStack.size() - 2].functionAddr;
			auto key = std::make_pair(callerFunc, top.functionAddr);
			auto &arc = callArcs[key];
			arc.count++;
			arc.inclusiveCost += inclusive;
			callStack[callStack.size() - 2].inclusiveCycles += inclusive;
		}

		callStack.pop_back();
	}
}

Profiler::Profiler() : impl(new Impl()) {}

Profiler::~Profiler() {
	delete impl;
}

void Profiler::initialize() {
	Loader::DebugNameTable table;
	Loader::Native::LoadDebugNames(table);
	Loader::Native::LoadSegmentInfo(impl->segments);

	for (auto &entry : table) {
		uint32_t addr = entry.second.first;
		uint32_t end = entry.second.second;
		impl->addrToName[addr] = entry.first;
		if (end > 0) {
			impl->addrToEnd[addr] = addr + end;
		}
	}

	// push an initial frame for the program entry point
	uint32_t pc = cpuGetPC();
	impl->pushFrame(pc, 0, 0xFFFFFFFF, false);
	impl->functionName(pc); // ensure it exists
	if (impl->functions[pc].name.substr(0, 4) == "sub_") {
		impl->functions[pc].name = "_start";
		impl->addrToName[pc] = "_start";
	}
}

void Profiler::beforeInstruction(uint32_t pc, uint16_t opcode) {
	impl->pendingCall = false;
	impl->pendingReturn = false;
	impl->pendingTrap = false;

	// JSR ea: (opcode & 0xFFC0) == 0x4E80
	// BSR: (opcode & 0xFF00) == 0x6100
	if ((opcode & 0xFFC0) == 0x4E80 || (opcode & 0xFF00) == 0x6100) {
		impl->pendingCall = true;
		impl->callSitePC = pc;
	}
	// RTS: 0x4E75
	// RTD: 0x4E74
	else if (opcode == 0x4E75 || opcode == 0x4E74) {
		impl->pendingReturn = true;
	}
	// A-line trap: (opcode & 0xF000) == 0xA000
	else if ((opcode & 0xF000) == 0xA000) {
		impl->pendingTrap = true;
		impl->trapOpcode = opcode;
		impl->callSitePC = pc;
	}
}

void Profiler::afterInstruction(uint32_t pc, uint32_t cycles) {
	// attribute cycles to current PC and current frame
	impl->instrCycles[pc] += cycles;
	if (!impl->callStack.empty()) {
		impl->callStack.back().selfCycles += cycles;
	}

	if (impl->pendingCall) {
		uint32_t newPC = cpuGetPC();
		uint32_t sp = cpuGetAReg(7);
		uint32_t returnAddr = memoryReadLong(sp);
		impl->pushFrame(newPC, impl->callSitePC, returnAddr, false);
	}

	if (impl->pendingReturn) {
		uint32_t newPC = cpuGetPC();
		impl->popFramesTo(newPC);
	}

	if (impl->pendingTrap) {
		// traps execute within one cpuExecuteInstruction call,
		// so we create a synthetic frame, attribute the cycles, and pop it
		uint32_t trapAddr = 0xA0000000 | impl->trapOpcode;

		// ensure the trap has a name
		auto &fi = impl->functions[trapAddr];
		if (fi.name.empty()) {
			const char *name = TrapName(impl->trapOpcode);
			if (name) {
				fi.name = name;
			} else {
				char buf[20];
				snprintf(buf, sizeof(buf), "trap_%04X", impl->trapOpcode);
				fi.name = buf;
			}
			fi.startAddr = trapAddr;
		}
		fi.callCount++;
		fi.selfCycles += cycles;

		// subtract cycles from the current frame (they belong to the trap)
		if (!impl->callStack.empty()) {
			impl->callStack.back().selfCycles -= cycles;
		}

		// record call arc from current function to the trap
		if (!impl->callStack.empty()) {
			uint32_t callerFunc = impl->callStack.back().functionAddr;
			auto key = std::make_pair(callerFunc, trapAddr);
			auto &arc = impl->callArcs[key];
			arc.count++;
			arc.inclusiveCost += cycles;

			// add to caller's inclusive
			impl->callStack.back().inclusiveCycles += cycles;
		}

		// record the instruction cost under the trap's synthetic address
		impl->instrCycles[trapAddr] += cycles;
	}
}

// ================================================================
// PPC Profiling
// ================================================================

uint32_t Profiler::ppcEstimateCycles(uint32_t instr) {
	uint32_t op = (instr >> 26) & 0x3F;
	switch (op) {
	case 31: { // extended ALU/misc
		uint32_t xo = (instr >> 1) & 0x3FF;
		switch (xo) {
		case 235: case 75:  return 3;  // mullw, mulhw
		case 491: case 459: return 37; // divw, divwu
		default: return 1;
		}
	}
	case 48: case 49: case 50: case 51: return 2; // lfs, lfsu, lfd, lfdu
	case 52: case 53: case 54: case 55: return 2; // stfs, stfsu, stfd, stfdu
	case 59: case 63: return 2; // FP single/double ops
	case 32: case 33: case 34: case 35:
	case 40: case 41: case 42: case 43: return 2; // loads
	case 36: case 37: case 38: case 39:
	case 44: case 45: case 46: case 47: return 1; // stores
	default: return 1; // ALU, cmp, branch, etc.
	}
}

void Profiler::initializePPC(uint32_t entryPC,
                              const std::map<std::string, uint32_t> &symbols) {
	// Populate symbol table from PEF exports and library imports
	for (auto &kv : symbols) {
		impl->addrToName[kv.second] = kv.first;
	}

	impl->pushFrame(entryPC, 0, 0xFFFFFFFF, false);
	impl->functionName(entryPC);
	if (impl->functions[entryPC].name.substr(0, 4) == "sub_") {
		impl->functions[entryPC].name = "_start";
		impl->addrToName[entryPC] = "_start";
	}
}

void Profiler::beforePPCInstruction(uint32_t pc, uint32_t instr) {
	impl->pendingCall = false;
	impl->pendingReturn = false;
	impl->pendingTrap = false;

	// bl (branch and link): primary opcode 18, LK bit set
	// bctrl (branch to CTR, link): 0x4E800421
	// blrl (branch to LR, link): 0x4E800021
	if ((instr & 0xFC000001) == 0x48000001 ||
	    instr == 0x4E800421 || instr == 0x4E800021) {
		impl->pendingCall = true;
		impl->callSitePC = pc;
	}
	// blr (return): 0x4E800020
	else if (instr == 0x4E800020) {
		impl->pendingReturn = true;
	}
	// sc (system call): primary opcode 17, bit 1 set
	else if ((instr & 0xFC000002) == 0x44000002) {
		impl->pendingTrap = true;
		impl->callSitePC = pc;
	}
}

void Profiler::afterPPCInstruction(uint32_t pc, uint32_t cycles) {
	impl->instrCycles[pc] += cycles;
	if (!impl->callStack.empty()) {
		impl->callStack.back().selfCycles += cycles;
	}

	if (impl->pendingCall) {
		uint32_t newPC = PPC::GetPC();
		uint32_t lr = PPC::GetLR();
		impl->pushFrame(newPC, impl->callSitePC, lr, false);
	}

	if (impl->pendingReturn) {
		uint32_t newPC = PPC::GetPC();
		impl->popFramesTo(newPC);
	}

	if (impl->pendingTrap) {
		// sc instructions dispatch to host-side handlers.
		// The sc handler sets PC = LR, so after the instruction
		// PC is back at the caller. We don't create a separate
		// frame for sc — the cost is attributed to the calling function.
	}
}

void Profiler::writeOutput(const std::string &filename) {
	impl->flushStack();

	FILE *f = fopen(filename.c_str(), "w");
	if (!f) {
		fprintf(stderr, "profiler: unable to open %s for writing\n", filename.c_str());
		return;
	}

	fprintf(f, "# callgrind format\n");
	fprintf(f, "version: 1\n");
	fprintf(f, "creator: mpw-profiler\n");
	fprintf(f, "positions: instr\n");
	fprintf(f, "events: Cycles\n");
	fprintf(f, "\n");

	// build a segment lookup: sorted by address for binary search
	// each entry maps an absolute address to a segment number and base offset
	struct SegLookup {
		uint32_t address;
		uint32_t size;
		uint16_t segmentNumber;
	};
	std::vector<SegLookup> segLookup;
	for (auto &cs : impl->segments) {
		segLookup.push_back({cs.address, cs.size, cs.segmentNumber});
	}
	std::sort(segLookup.begin(), segLookup.end(),
		[](const SegLookup &a, const SegLookup &b) { return a.address < b.address; });

	// convert an absolute PC to a segment-relative offset.
	// returns the segment number (0 = unknown/trap) and sets offset.
	auto toSegOffset = [&](uint32_t pc, uint32_t &offset) -> uint16_t {
		// synthetic trap addresses pass through
		if ((pc & 0xA0000000) == 0xA0000000) {
			offset = pc;
			return 0;
		}
		// binary search: find last segment with address <= pc
		SegLookup key = {pc, 0, 0};
		auto it = std::upper_bound(segLookup.begin(), segLookup.end(), key,
			[](const SegLookup &a, const SegLookup &b) { return a.address < b.address; });
		if (it != segLookup.begin()) {
			--it;
			if (pc >= it->address && pc < it->address + it->size) {
				offset = pc - it->address;
				return it->segmentNumber;
			}
		}
		// not in any known segment - use absolute address
		offset = pc;
		return 0;
	};

	// helper to format the fl= name for a segment
	auto segFileName = [](uint16_t seg) -> std::string {
		if (seg == 0) return "TRAPS";
		char buf[16];
		snprintf(buf, sizeof(buf), "SEG_%u", seg);
		return buf;
	};

	// collect all instruction PCs by function
	// first, build a sorted list of function start addresses
	std::vector<uint32_t> funcAddrs;
	funcAddrs.reserve(impl->functions.size());
	for (auto &p : impl->functions) {
		funcAddrs.push_back(p.first);
	}
	std::sort(funcAddrs.begin(), funcAddrs.end());

	// build a complete sorted map of all known function start addresses
	// (from both debug symbols and JSR targets discovered during execution)
	std::map<uint32_t, uint32_t> allFuncAddrs;
	for (auto &p : impl->functions) {
		if ((p.first & 0xA0000000) != 0xA0000000) { // skip synthetic trap addrs
			allFuncAddrs[p.first] = p.first;
		}
	}

	// map each instruction PC to its function
	std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint64_t>>> funcInstr;
	for (auto &p : impl->instrCycles) {
		uint32_t pc = p.first;
		uint64_t cy = p.second;

		// synthetic trap addresses
		if ((pc & 0xA0000000) == 0xA0000000) {
			funcInstr[pc].push_back({pc, cy});
			continue;
		}

		// find the function containing this PC using all known function addresses
		uint32_t func = pc;
		auto it = allFuncAddrs.upper_bound(pc);
		if (it != allFuncAddrs.begin()) {
			--it;
			func = it->second;
		}
		// ensure the function has a name entry
		impl->functionName(func);
		funcInstr[func].push_back({pc, cy});
	}

	// determine the segment for each function
	std::map<uint16_t, std::vector<uint32_t>> segToFuncs;
	for (uint32_t funcAddr : funcAddrs) {
		uint32_t offset;
		uint16_t seg = toSegOffset(funcAddr, offset);
		segToFuncs[seg].push_back(funcAddr);
	}

	// emit functions grouped by segment
	for (auto &sf : segToFuncs) {
		uint16_t seg = sf.first;
		fprintf(f, "fl=%s\n", segFileName(seg).c_str());

		for (uint32_t funcAddr : sf.second) {
			auto &fi = impl->functions[funcAddr];
			fprintf(f, "fn=%s\n", fi.name.c_str());

			// sort instructions by address
			auto &instrs = funcInstr[funcAddr];
			std::sort(instrs.begin(), instrs.end());

			for (auto &ip : instrs) {
				uint32_t offset;
				toSegOffset(ip.first, offset);
				fprintf(f, "0x%08X %llu\n", offset, (unsigned long long)ip.second);
			}

			// emit call arcs from this function
			for (auto &arcEntry : impl->callArcs) {
				if (arcEntry.first.first != funcAddr) continue;

				uint32_t calleeAddr = arcEntry.first.second;
				auto &arc = arcEntry.second;
				auto &calleeFi = impl->functions[calleeAddr];

				// emit callee's file if it's in a different segment
				uint32_t calleeOffset;
				uint16_t calleeSeg = toSegOffset(calleeAddr, calleeOffset);
				if (calleeSeg != seg) {
					fprintf(f, "cfl=%s\n", segFileName(calleeSeg).c_str());
				}

				fprintf(f, "cfn=%s\n", calleeFi.name.c_str());
				fprintf(f, "calls=%llu 0x%08X\n", (unsigned long long)arc.count, calleeOffset);

				// use the first instruction in this function as call site
				uint32_t callSitePC = funcAddr;
				for (auto &ip : instrs) {
					callSitePC = ip.first;
					break;
				}
				uint32_t callSiteOffset;
				toSegOffset(callSitePC, callSiteOffset);
				fprintf(f, "0x%08X %llu\n", callSiteOffset, (unsigned long long)arc.inclusiveCost);
			}

			fprintf(f, "\n");
		}
	}

	fclose(f);
	fprintf(stderr, "profiler: wrote %s\n", filename.c_str());
}
