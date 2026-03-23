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

#include <cstdio>
#include <cstdint>
#include <cassert>
#include <cctype>
#include <cstring>

#include <string>
#include <vector>
#include <map>
#include <fstream>

#include <rsrc/rsrc.h>

#include <cpu/m68k/defs.h>
#include <cpu/m68k/fmem.h>
#include <cpu/m68k/CpuModule.h>

#include <macos/traps.h>
#include <macos/sysequ.h>

char strings[4][256];

const uint8_t *Memory = NULL;
uint32_t MemorySize = 0;


void ToolBox(uint32_t pc, uint16_t trap)
{
	const char *name;

	name = TrapName(trap);

	if (name)
	{
		printf("$%08X   %-51s ; %04X\n", pc, name, trap);
	}
	else
	{
		printf("$%08X   Tool       #$%04X                                   ; %04X\n", pc, trap, trap);
	}
}

void help()
{
	fprintf(stderr, "Usage: disasm [options] <file>\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  --raw-drvr    Treat file as a raw DRVR resource\n");
	fprintf(stderr, "  --raw         Treat file as raw 68K code\n");
	fprintf(stderr, "  --raw-type <type> <id>\n");
	fprintf(stderr, "                Treat file as a raw resource of given type/id\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Without options, reads CODE resources from the file's resource fork.\n");
}


void code0(uint32_t data_size)
{
	// dump the code 0 jump table.

	uint32_t offset = 0;
	uint32_t pc;
	uint32_t size;
	printf("Above A5:          $%08X\n", memoryReadLong(0));
	printf("Below A5:          $%08X\n", memoryReadLong(4));
	printf("Jump Table Size:   $%08X\n", size = memoryReadLong(8));
	printf("Jump Table Offset: $%08X\n", pc = memoryReadLong(12)); // aka CurJTOffset.  should be 32.

	offset = 16;

	bool longA5 = false;
	while (offset < data_size)
	{

		if (longA5)
		{
			uint16_t segment = memoryReadWord(offset);
			uint32_t segOffset = memoryReadLong(offset + 4);

			if (memoryReadWord(offset + 2) == 0xA9F0)
			{
				printf("$%08X   %04X : %08X\n", pc + 2, segment, segOffset);
			}
			else
			{
				printf("$%08X   ???\n", pc + 2);
			}
		}
		else
		{
			uint16_t data[4];
			for (unsigned i = 0; i < 4; ++i)
				data[i] = memoryReadWord(offset + 2 * i);

			if (data[1] == 0xffff)
			{
				longA5 = true;
				printf("--------\n");
			}
			else if (data[1] == 0x3F3C && data[3] == 0xA9F0)
			{
				uint16_t segOffset = data[0];
				uint16_t segment = data[2];

				// pc +2 since the first byte is the offset, not code.
				printf("$%08X   %04X : %04X\n", pc + 2, segment, segOffset);
			}
			else
			{
				printf("$%08X   ???\n", pc + 2);
			}
		}
		offset += 8;
		pc += 8;
	}

	printf("\n\n");
}

inline char *cc2(uint16_t value, char out[3])
{
	uint8_t tmp;

	tmp = (value >> 8) & 0xff;
	if (tmp & 0x80 || !isprint(tmp)) out[0] = '.';
	else out[0] = tmp;

	tmp = (value >> 0) & 0xff;
	if (tmp & 0x80 || !isprint(tmp)) out[1] = '.';
	else out[1] = tmp;

	out[2] = 0;
	return out;
}

void disasm(const char *name, int segment, uint32_t data_size,
            const std::map<uint32_t, const char *> *labels = nullptr)
{

	if (name && *name) printf("segment %d - %s\n", segment, name);
	else printf("segment %d\n", segment);

	uint32_t pc = 0;


	uint16_t prevOP = 0;

	while (pc < data_size)
	{
		// Print label if one exists at this address
		if (labels)
		{
			auto it = labels->find(pc);
			if (it != labels->end())
			{
				printf("\n; --- %s ---\n", it->second);
			}
		}

		for (unsigned j = 0; j < 4; ++j) strings[j][0] = 0;

		uint16_t op = memoryReadWord(pc);

		if (prevOP == 0x4E75  || prevOP == 0x4ED0 || prevOP == 0x4E74)
		{
			if (op > 0x8000)
			{
				// RTS followed by debug symbol.
				unsigned len = (op >> 8) - 0x80;

				std::string s;
				s.reserve(len);
				pc += 1; // skip the length byte.
				for (unsigned i = 0; i < len; ++ i)
				{
					s.push_back(memoryReadByte(pc++));
				}

				printf("%s\n", s.c_str());
				pc = (pc + 1) & ~0x01;

#if 0
				if (memoryReadWord(pc) == 0x0000) pc += 2;
#else
				// treat the remainder as data until 4E56. [Link A6,#]
				while (pc < data_size)
				{
					char tmp[3];
					uint16_t data = memoryReadWord(pc);
					if (data == 0x4e56) break;
					printf("$%08X   $%04X                                               ; '%s'\n", pc, data, cc2(data, tmp));
					pc += 2;
				}
#endif
				printf("\n");

				prevOP = 0;
				continue;
			}
		}

		if ((op & 0xf000) == 0xa000)
		{
			// tool call!

			ToolBox(pc, op);
			pc += 2;
			prevOP = op;
			continue;
		}

		pc = cpuDisOpcode(pc, strings[0], strings[1], strings[2], strings[3]);

		// address, data, instruction, operand
		printf("%s   %-10s %-40s ; %s\n", strings[0], strings[2], strings[3], strings[1]);

		prevOP = op;
	}

	printf("\n\n");
}

std::vector<uint8_t> readFile(const char *path)
{
	std::vector<uint8_t> data;
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) return data;
	auto size = f.tellg();
	f.seekg(0);
	data.resize(size);
	f.read(reinterpret_cast<char *>(data.data()), size);
	return data;
}

uint16_t readBE16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

void disasmDRVR(const uint8_t *data, uint32_t size)
{
	if (size < 18)
	{
		fprintf(stderr, "DRVR resource too small\n");
		return;
	}

	uint16_t drvrFlags  = readBE16(data + 0);
	uint16_t drvrDelay  = readBE16(data + 2);
	uint16_t drvrEMask  = readBE16(data + 4);
	uint16_t drvrMenu   = readBE16(data + 6);
	uint16_t drvrOpen   = readBE16(data + 8);
	uint16_t drvrPrime  = readBE16(data + 10);
	uint16_t drvrCtl    = readBE16(data + 12);
	uint16_t drvrStatus = readBE16(data + 14);
	uint16_t drvrClose  = readBE16(data + 16);

	// Parse driver name (Pascal string at offset 18)
	uint8_t nameLen = data[18];
	std::string drvrName;
	if (nameLen > 0 && 18 + 1 + nameLen <= size)
	{
		drvrName.assign(reinterpret_cast<const char *>(data + 19), nameLen);
	}

	printf("; ============================================================\n");
	printf("; DRVR resource: \"%s\"\n", drvrName.c_str());
	printf("; ============================================================\n");
	printf(";\n");
	printf("; drvrFlags:  $%04X\n", drvrFlags);
	printf(";   dReadEnable:  %s\n", (drvrFlags & 0x4000) ? "yes" : "no");
	printf(";   dWritEnable:  %s\n", (drvrFlags & 0x2000) ? "yes" : "no");
	printf(";   dCtlEnable:   %s\n", (drvrFlags & 0x1000) ? "yes" : "no");
	printf(";   dStatEnable:  %s\n", (drvrFlags & 0x0800) ? "yes" : "no");
	printf(";   dNeedGoodBye: %s\n", (drvrFlags & 0x0400) ? "yes" : "no");
	printf(";   dNeedTime:    %s\n", (drvrFlags & 0x0200) ? "yes" : "no");
	printf(";   dNeedLock:    %s\n", (drvrFlags & 0x0100) ? "yes" : "no");
	printf("; drvrDelay:  %u ticks\n", drvrDelay);
	printf("; drvrEMask:  $%04X\n", drvrEMask);
	printf("; drvrMenu:   %u\n", drvrMenu);
	printf("; drvrOpen:   $%04X\n", drvrOpen);
	printf("; drvrPrime:  $%04X\n", drvrPrime);
	printf("; drvrCtl:    $%04X\n", drvrCtl);
	printf("; drvrStatus: $%04X\n", drvrStatus);
	printf("; drvrClose:  $%04X\n", drvrClose);
	printf("; drvrName:   \"%s\"\n", drvrName.c_str());
	printf(";\n");

	// Build label map for entry points
	std::map<uint32_t, const char *> labels;
	if (drvrOpen)   labels[drvrOpen]   = "drvrOpen";
	if (drvrPrime)  labels[drvrPrime]  = "drvrPrime";
	if (drvrCtl)    labels[drvrCtl]    = "drvrCtl";
	if (drvrStatus) labels[drvrStatus] = "drvrStatus";
	if (drvrClose)  labels[drvrClose]  = "drvrClose";

	// Disassemble the entire DRVR resource as code
	memorySetMemory(const_cast<uint8_t *>(data), size);
	disasm(drvrName.c_str(), 0, size, &labels);
	memorySetMemory(nullptr, 0);
}

void disasmRaw(const uint8_t *data, uint32_t size)
{
	memorySetMemory(const_cast<uint8_t *>(data), size);
	disasm("raw", 0, size);
	memorySetMemory(nullptr, 0);
}

int main(int argc, char **argv)
{
	const uint32_t kCODE = 0x434f4445;
	const uint32_t kDRVR = 0x44525652;

	enum Mode { MODE_RSRC, MODE_RAW_DRVR, MODE_RAW };
	Mode mode = MODE_RSRC;
	const char *filePath = nullptr;
	uint32_t rsrcType = 0;

	for (int i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--raw-drvr") == 0)
		{
			mode = MODE_RAW_DRVR;
		}
		else if (strcmp(argv[i], "--raw") == 0)
		{
			mode = MODE_RAW;
		}
		else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
		{
			help();
			return 0;
		}
		else if (argv[i][0] == '-')
		{
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			help();
			return -1;
		}
		else
		{
			filePath = argv[i];
		}
	}

	if (!filePath)
	{
		help();
		return -1;
	}

	cpuSetModel(3, 0); // 68030

	if (mode == MODE_RAW_DRVR)
	{
		auto data = readFile(filePath);
		if (data.empty())
		{
			fprintf(stderr, "Unable to read file %s\n", filePath);
			return -1;
		}
		disasmDRVR(data.data(), data.size());
		return 0;
	}

	if (mode == MODE_RAW)
	{
		auto data = readFile(filePath);
		if (data.empty())
		{
			fprintf(stderr, "Unable to read file %s\n", filePath);
			return -1;
		}
		disasmRaw(data.data(), data.size());
		return 0;
	}

	// Default: read CODE resources from resource fork
	auto forkData = rsrc::readResourceFork(filePath);
	if (forkData.empty())
	{
		fprintf(stderr, "Unable to open resource fork for %s\n", filePath);
		return -1;
	}

	auto rf = rsrc::ResourceFile::open(forkData);
	if (!rf)
	{
		fprintf(stderr, "Unable to parse resource fork for %s\n", filePath);
		return -1;
	}

	// Check for DRVR resources first, then CODE
	int drvrCount = rf->countResources(kDRVR);
	if (drvrCount > 0)
	{
		for (int i = 0; i < drvrCount; ++i)
		{
			const rsrc::ResourceEntry *entry = rf->getIndResource(kDRVR, i + 1);
			if (!entry) continue;

			auto resData = rf->loadResource(*entry);
			if (resData.empty()) continue;

			printf("; DRVR ID=%d\n", entry->id);
			disasmDRVR(resData.data(), resData.size());
		}
		return 0;
	}

	int l = rf->countResources(kCODE);

	for (int i = 0; i < l; ++i)
	{
		const rsrc::ResourceEntry *entry = rf->getIndResource(kCODE, i + 1);
		if (!entry) continue;

		auto resData = rf->loadResource(*entry);
		if (resData.empty()) continue;

		int16_t resID = entry->id;
		std::string cname = entry->name;
		uint32_t size = resData.size();
		const uint8_t *data = resData.data();

		if (resID == 0)
		{
			memorySetMemory(const_cast<uint8_t*>(data), size);
			code0(size);
		}
		else
		{
			const uint8_t *codeData = data;
			uint32_t codeSize = size;

			// near model uses a $4-byte header.
			// far model uses a $28-byte header.
			if (codeData[0] == 0xff && codeData[1] == 0xff)
			{
				codeData += 0x28;
				codeSize -= 0x28;
			}
			else
			{
				codeData += 0x04;
				codeSize -= 0x04;
			}
			memorySetMemory(const_cast<uint8_t*>(codeData), codeSize);
			disasm(cname.c_str(), resID, codeSize);
		}

		memorySetMemory(nullptr, 0);
	}

    return 0;
}
