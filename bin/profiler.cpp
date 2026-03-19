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

#include <cpu/defs.h>
#include <cpu/CpuModule.h>
#include <cpu/fmem.h>

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
	fprintf(f, "fl=CODE\n");
	fprintf(f, "\n");

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

	// emit each function
	for (uint32_t funcAddr : funcAddrs) {
		auto &fi = impl->functions[funcAddr];
		fprintf(f, "fn=%s\n", fi.name.c_str());

		// sort instructions by address
		auto &instrs = funcInstr[funcAddr];
		std::sort(instrs.begin(), instrs.end());

		for (auto &ip : instrs) {
			fprintf(f, "0x%08X %llu\n", ip.first, (unsigned long long)ip.second);
		}

		// emit call arcs from this function
		for (auto &arcEntry : impl->callArcs) {
			if (arcEntry.first.first != funcAddr) continue;

			uint32_t calleeAddr = arcEntry.first.second;
			auto &arc = arcEntry.second;
			auto &calleeFi = impl->functions[calleeAddr];

			fprintf(f, "cfn=%s\n", calleeFi.name.c_str());
			fprintf(f, "calls=%llu 0x%08X\n", (unsigned long long)arc.count, calleeAddr);

			// find the call site PC - use the most common instruction in this function
			// that precedes a call to the callee. For simplicity, use funcAddr as fallback.
			uint32_t callSitePC = funcAddr;
			// search instrCycles for the best match
			for (auto &ip : instrs) {
				// just use the first instruction as call site if we can't do better
				callSitePC = ip.first;
				break;
			}
			fprintf(f, "0x%08X %llu\n", callSitePC, (unsigned long long)arc.inclusiveCost);
		}

		fprintf(f, "\n");
	}

	fclose(f);
	fprintf(stderr, "profiler: wrote %s\n", filename.c_str());
}
