/*
 * ppc_dispatch.cpp
 *
 * InterfaceLib/MathLib/PrivateInterfaceLib wrappers for PPC emulation.
 *
 * These implement the 66 functions that StdCLib imports (verified via
 * DumpPEF). InterfaceLib, MathLib, and PrivateInterfaceLib are stub
 * libraries with no code — our CFM stubs ARE the implementation.
 *
 * Each wrapper reads PPC registers (r3-r10 for args, f1-f13 for FP),
 * calls the existing Native:: API, and writes the result to r3 (or f1).
 */

#include "ppc_dispatch.h"
#include "cfm_stubs.h"
#include "mm.h"
#include "rm.h"
#include "os.h"
#include "os_internal.h"
#include "toolbox.h"

#include <cpu/m68k/defs.h>
#include <cpu/m68k/CpuModule.h>
#include <cpu/m68k/fmem.h>
#include <cpu/ppc/ppc.h>

#include <macos/sysequ.h>
#include <macos/errors.h>

#include <mpw/mpw.h>

#include <sane/sane.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <algorithm>
#include <vector>
#include <map>
#include <unistd.h>

namespace {

using PPC::GetGPR;
using PPC::SetGPR;
using PPC::GetFPR;
using PPC::SetFPR;

static void reg(const std::string &lib, const std::string &sym, CFMStubs::Handler h) {
	CFMStubs::RegisterStub(lib, sym, h);
}

// -- Helpers --

static std::string readPString(uint32_t addr) {
	if (!addr) return "";
	uint8_t len = memoryReadByte(addr);
	std::string s;
	for (uint8_t i = 0; i < len; i++)
		s.push_back(memoryReadByte(addr + 1 + i));
	return s;
}

static std::string readCString(uint32_t addr) {
	if (!addr) return "";
	std::string s;
	for (;;) {
		uint8_t c = memoryReadByte(addr++);
		if (!c) break;
		s.push_back(c);
	}
	return s;
}

// ================================================================
// Memory Manager
// ================================================================

static void wrap_NewPtr() {
	uint32_t ptr = 0;
	MM::Native::NewPtr(GetGPR(3), false, ptr);
	SetGPR(3, ptr);
}

static void wrap_DisposePtr() {
	MM::Native::DisposePtr(GetGPR(3));
}

static void wrap_GetPtrSize() {
	cpuSetAReg(0, GetGPR(3));
	uint32_t result = MM::GetPtrSize(0xA021);
	SetGPR(3, result);
}

static void wrap_SetPtrSize() {
	SetGPR(3, 0); // noErr
}

static void wrap_NewHandle() {
	uint32_t handle = 0;
	MM::Native::NewHandle(GetGPR(3), false, handle);
	SetGPR(3, handle);
}

static void wrap_DisposeHandle() {
	MM::Native::DisposeHandle(GetGPR(3));
}

static void wrap_HLock() {
	MM::Native::HLock(GetGPR(3));
	SetGPR(3, 0);
}

static void wrap_HUnlock() {
	MM::Native::HUnlock(GetGPR(3));
	SetGPR(3, 0);
}

static void wrap_BlockMove() {
	uint32_t src = GetGPR(3);
	uint32_t dst = GetGPR(4);
	uint32_t size = GetGPR(5);
	uint8_t *s = memoryPointer(src);
	uint8_t *d = memoryPointer(dst);
	if (s && d && size > 0) memmove(d, s, size);
}

static void wrap_FreeMem() {
	SetGPR(3, 8 * 1024 * 1024);
}

static void wrap_MemError() {
	SetGPR(3, memoryReadWord(MacOS::MemErr));
}

static void wrap_GetZone() {
	SetGPR(3, memoryReadLong(MacOS::ApplZone));
}

static void wrap_SetZone() {
	// ignore — single zone
}

// ================================================================
// File Manager — PB calls
// ================================================================

#define PPC_PB_WRAP(name, call) \
	static void wrap_##name() { \
		SetGPR(3, (uint32_t)(int16_t)(call)); \
	}

static void wrap_PBHOpenSync() {
	uint32_t parm = GetGPR(3);

	uint32_t namePtr = memoryReadLong(parm + 18);
	if (namePtr) {
		std::string name = readPString(namePtr);

		int hostFd = -1;
		if (name == "stdin" || name == "dev:stdin")
			hostFd = STDIN_FILENO;
		else if (name == "stdout" || name == "dev:stdout")
			hostFd = STDOUT_FILENO;
		else if (name == "stderr" || name == "dev:stderr")
			hostFd = STDERR_FILENO;

		if (hostFd >= 0) {
			memoryWriteWord(hostFd, parm + 24); // ioRefNum
			memoryWriteWord(0, parm + 16);       // ioResult
			SetGPR(3, 0);
			return;
		}
	}

	SetGPR(3, (uint32_t)(int16_t)OS::Native::Open(parm, 0xA200));
}

PPC_PB_WRAP(PBHOpenRFSync,    OS::Native::OpenRF(GetGPR(3), 0xA20A))
PPC_PB_WRAP(PBCloseSync,      OS::Native::Close(GetGPR(3)))
PPC_PB_WRAP(PBHCreateSync,    OS::Native::Create(GetGPR(3), 0xA208))
PPC_PB_WRAP(PBSetEOFSync,     OS::Native::SetEOF(GetGPR(3)))
PPC_PB_WRAP(PBGetCatInfoSync, OS::Native::HFSDispatch(GetGPR(3), 0x0009))
PPC_PB_WRAP(PBGetFCBInfoSync, OS::Native::HFSDispatch(GetGPR(3), 0x0008))

// ================================================================
// File Manager — high-level calls
// ================================================================

static void wrap_FSRead() {
	int16_t refNum = (int16_t)GetGPR(3);
	uint32_t countPtr = GetGPR(4);
	uint32_t bufPtr = GetGPR(5);
	int32_t count = memoryReadLong(countPtr);

	ssize_t n = OS::Internal::FDEntry::read(refNum, memoryPointer(bufPtr), count);
	if (n < 0) {
		SetGPR(3, (uint32_t)(int16_t)MacOS::ioErr);
	} else {
		memoryWriteLong((int32_t)n, countPtr);
		SetGPR(3, n < count ? (uint32_t)(int16_t)MacOS::eofErr : 0);
	}
}

static void wrap_FSWrite() {
	int16_t refNum = (int16_t)GetGPR(3);
	uint32_t countPtr = GetGPR(4);
	uint32_t bufPtr = GetGPR(5);
	int32_t count = memoryReadLong(countPtr);

	ssize_t n = OS::Internal::FDEntry::write(refNum, memoryPointer(bufPtr), count);
	if (n < 0) {
		SetGPR(3, (uint32_t)(int16_t)MacOS::ioErr);
	} else {
		memoryWriteLong((int32_t)n, countPtr);
		SetGPR(3, 0);
	}
}

PPC_PB_WRAP(FSClose,   OS::Native::Close(GetGPR(3)))
PPC_PB_WRAP(Create,    OS::Native::Create(GetGPR(3), 0xA008))
PPC_PB_WRAP(FSDelete,  OS::Native::Delete(GetGPR(3), 0xA009))
PPC_PB_WRAP(GetFInfo,  OS::Native::GetFileInfo(GetGPR(3), 0xA00C))
PPC_PB_WRAP(HGetFInfo, OS::Native::GetFileInfo(GetGPR(3), 0xA20C))
PPC_PB_WRAP(SetFInfo,  OS::Native::SetFileInfo(GetGPR(3), 0xA00D))
PPC_PB_WRAP(HSetFInfo, OS::Native::SetFileInfo(GetGPR(3), 0xA20D))
PPC_PB_WRAP(SetEOF,    OS::Native::SetEOF(GetGPR(3)))
PPC_PB_WRAP(GetFPos,   OS::Native::GetFPos(GetGPR(3)))
PPC_PB_WRAP(SetFPos,   OS::Native::SetFPos(GetGPR(3)))
PPC_PB_WRAP(HGetVol,   OS::Native::HGetVol(GetGPR(3)))
PPC_PB_WRAP(HDelete,   OS::Native::Delete(GetGPR(3), 0xA209))

#undef PPC_PB_WRAP

static void wrap_FSMakeFSSpec() {
	uint16_t vRefNum = GetGPR(3);
	uint32_t dirID = GetGPR(4);
	uint32_t fileNamePtr = GetGPR(5);
	uint32_t specPtr = GetGPR(6);

	if (specPtr) {
		memoryWriteWord(vRefNum, specPtr);
		memoryWriteLong(dirID, specPtr + 2);
		if (fileNamePtr) {
			uint8_t len = memoryReadByte(fileNamePtr);
			for (uint8_t i = 0; i <= len; i++)
				memoryWriteByte(memoryReadByte(fileNamePtr + i), specPtr + 6 + i);
		}
	}
	SetGPR(3, 0);
}

static void wrap_ResolveAliasFile() {
	uint32_t wasAliasedPtr = GetGPR(6);
	if (wasAliasedPtr) memoryWriteByte(0, wasAliasedPtr);
	SetGPR(3, 0);
}

static void wrap_Rename() {
	SetGPR(3, 0);
}

// ================================================================
// Trap Manager
// ================================================================

static void wrap_NGetTrapAddress() {
	// Return a dummy non-zero address to indicate the trap exists.
	SetGPR(3, 0x9F000000 | (GetGPR(3) & 0xFFFF));
}

// ================================================================
// Gestalt Manager
// ================================================================

static void wrap_Gestalt() {
	uint32_t selector = GetGPR(3);
	uint32_t responsePtr = GetGPR(4);
	cpuSetDReg(0, selector);
	uint16_t err = OS::Gestalt(0xA1AD);
	if (responsePtr) memoryWriteLong(cpuGetAReg(0), responsePtr);
	SetGPR(3, (uint32_t)(int16_t)err);
}

// ================================================================
// Resource Manager
// ================================================================

static void wrap_ReleaseResource() {
	RM::Native::ReleaseResource(GetGPR(3));
}

static void wrap_ResError() {
	SetGPR(3, (uint32_t)(int16_t)RM::Native::ResError());
}

static void wrap_CurResFile() {
	SetGPR(3, (uint32_t)(int32_t)RM::Native::CurResFile());
}

// ================================================================
// Time
// ================================================================

static void wrap_GetDateTime() {
	uint32_t secsPtr = GetGPR(3);
	time_t now = time(nullptr);
	uint32_t macTime = (uint32_t)OS::UnixToMac(now);
	if (secsPtr) memoryWriteLong(macTime, secsPtr);
	SetGPR(3, 0);
}

static void wrap_SecondsToDate() {
	uint32_t secs = GetGPR(3);
	uint32_t dPtr = GetGPR(4);
	time_t unixTime = OS::MacToUnix((time_t)secs);
	struct tm *t = localtime(&unixTime);
	if (t && dPtr) {
		memoryWriteWord(t->tm_year + 1900, dPtr);
		memoryWriteWord(t->tm_mon + 1, dPtr + 2);
		memoryWriteWord(t->tm_mday, dPtr + 4);
		memoryWriteWord(t->tm_hour, dPtr + 6);
		memoryWriteWord(t->tm_min, dPtr + 8);
		memoryWriteWord(t->tm_sec, dPtr + 10);
		memoryWriteWord(t->tm_wday + 1, dPtr + 12);
	}
}

static void wrap_DateToSeconds() {
	uint32_t dPtr = GetGPR(3);
	uint32_t secsPtr = GetGPR(4);
	if (dPtr && secsPtr) {
		struct tm t = {};
		t.tm_year = (int16_t)memoryReadWord(dPtr) - 1900;
		t.tm_mon = (int16_t)memoryReadWord(dPtr + 2) - 1;
		t.tm_mday = (int16_t)memoryReadWord(dPtr + 4);
		t.tm_hour = (int16_t)memoryReadWord(dPtr + 6);
		t.tm_min = (int16_t)memoryReadWord(dPtr + 8);
		t.tm_sec = (int16_t)memoryReadWord(dPtr + 10);
		time_t unixTime = mktime(&t);
		memoryWriteLong((uint32_t)OS::UnixToMac(unixTime), secsPtr);
	}
}

static void wrap_TickCount() {
	static auto start = std::chrono::steady_clock::now();
	auto now = std::chrono::steady_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
	SetGPR(3, (uint32_t)(ms * 60 / 1000));
}

// ================================================================
// String Conversions
// ================================================================

static void wrap_c2pstr() {
	uint32_t strPtr = GetGPR(3);
	uint32_t len = 0;
	while (memoryReadByte(strPtr + len) != 0 && len < 255) len++;
	for (uint32_t i = len; i > 0; i--)
		memoryWriteByte(memoryReadByte(strPtr + i - 1), strPtr + i);
	memoryWriteByte(len, strPtr);
	SetGPR(3, strPtr);
}

static void wrap_p2cstr() {
	uint32_t strPtr = GetGPR(3);
	uint8_t len = memoryReadByte(strPtr);
	for (uint8_t i = 0; i < len; i++)
		memoryWriteByte(memoryReadByte(strPtr + 1 + i), strPtr + i);
	memoryWriteByte(0, strPtr + len);
	SetGPR(3, strPtr);
}

// ================================================================
// Process Manager
// ================================================================

static void wrap_GetCurrentProcess() {
	uint32_t psnPtr = GetGPR(3);
	if (psnPtr) {
		memoryWriteLong(0, psnPtr);     // highLongOfPSN
		memoryWriteLong(2, psnPtr + 4); // lowLongOfPSN = kCurrentProcess
	}
	SetGPR(3, 0);
}

static void wrap_GetProcessInformation() {
	uint32_t infoPtr = GetGPR(4);
	if (infoPtr) {
		for (int i = 0; i < 60; i++) memoryWriteByte(0, infoPtr + i);
	}
	SetGPR(3, 0);
}

static void wrap_ExitToShell() {
	PPC::Stop();
}

// ================================================================
// Mixed Mode Manager
// ================================================================

static const uint16_t kMixedModeMagic = 0xAAFE;
static const uint8_t kPowerPCISA = 1;

static void wrap_NewRoutineDescriptor() {
	uint32_t proc = GetGPR(3);
	uint32_t procInfo = GetGPR(4);
	uint32_t isa = GetGPR(5);

	if (isa == kPowerPCISA) {
		// PPC: the proc is already a TVector. Pass it through.
		SetGPR(3, proc);
		return;
	}

	// 68K: allocate a RoutineDescriptor
	uint32_t desc = 0;
	MM::Native::NewPtr(32, true, desc);
	if (!desc) { SetGPR(3, 0); return; }

	memoryWriteWord(kMixedModeMagic, desc);
	memoryWriteByte(7, desc + 2);            // version
	memoryWriteLong(procInfo, desc + 12);
	memoryWriteByte((uint8_t)isa, desc + 17);
	memoryWriteWord(0x0004, desc + 18);      // routineFlags
	memoryWriteLong(proc, desc + 20);        // procDescriptor

	SetGPR(3, desc);
}

static void wrap_DisposeRoutineDescriptor() {
	uint32_t desc = GetGPR(3);
	if (desc && memoryReadWord(desc) == kMixedModeMagic) {
		MM::Native::DisposePtr(desc);
	}
}

// 68K handler for CallUniversalProc — called when trampoline detects 0xAAFE
static void wrap_CallUniversalProc_68K() {
	uint32_t desc = GetGPR(3);
	uint32_t target = memoryReadLong(desc + 20);
	uint16_t opcode = memoryReadWord(target);
	MPW::dispatch(opcode);
	SetGPR(3, cpuGetDReg(0));
}

// ================================================================
// Internationalization
// ================================================================

static void wrap_GetIntlResource() {
	uint32_t handle = 0;
	uint16_t id = GetGPR(3);
	// GetIntlResource(id) returns GetResource('itl0'/'itl1'/etc., id)
	uint32_t type = (id == 0) ? 0x69746C30 : 0x69746C31; // 'itl0' or 'itl1'
	RM::Native::GetResource(type, id, handle);
	SetGPR(3, handle);
}

// ================================================================
// Misc
// ================================================================

static void wrap_DebugStr() {
	uint32_t strPtr = GetGPR(3);
	std::string s = readPString(strPtr);
	fprintf(stderr, "DebugStr: %s\n", s.c_str());
}

static void wrap_LMGetCurApName() {
	SetGPR(3, MacOS::CurApName);
}

static void wrap_GetNodeAddress() {
	SetGPR(3, (uint32_t)(int16_t)-1);
}

// ================================================================
// MathLib — SANE decimal conversion
// ================================================================

/*
 * PPC decimal struct layout (42 bytes):
 *   +0: int8  sgn (0=+, 1=-)
 *   +1: int8  unused
 *   +2: int16 exp
 *   +4: uint8 sig.length
 *   +5: char  sig.text[36]
 *  +41: uint8 sig.unused
 *
 * PPC decform struct layout (4 bytes):
 *   +0: int8  style (0=FLOAT, 1=FIXED)
 *   +1: int8  unused
 *   +2: int16 digits
 */

static SANE::decimal readDecimal(uint32_t addr) {
	SANE::decimal d;
	d.sgn = memoryReadByte(addr) ? 1 : 0;
	d.exp = (int16_t)memoryReadWord(addr + 2);
	uint8_t sigLen = memoryReadByte(addr + 4);
	d.sig.clear();
	for (uint8_t i = 0; i < sigLen && i < 36; i++)
		d.sig.push_back(memoryReadByte(addr + 5 + i));
	return d;
}

static void writeDecimal(uint32_t addr, const SANE::decimal &d) {
	memoryWriteByte(d.sgn ? 1 : 0, addr);
	memoryWriteByte(0, addr + 1);
	memoryWriteWord((uint16_t)(int16_t)d.exp, addr + 2);
	uint8_t sigLen = std::min((int)d.sig.size(), 36);
	memoryWriteByte(sigLen, addr + 4);
	for (uint8_t i = 0; i < sigLen; i++)
		memoryWriteByte(d.sig[i], addr + 5 + i);
	for (uint8_t i = sigLen; i < 36; i++)
		memoryWriteByte(0, addr + 5 + i);
}

static void wrap_str2dec() {
	uint32_t sPtr = GetGPR(3);
	uint32_t ixPtr = GetGPR(4);
	uint32_t dPtr = GetGPR(5);
	uint32_t vpPtr = GetGPR(6);

	std::string s = readCString(sPtr);
	uint16_t ix = memoryReadWord(ixPtr);
	SANE::decimal d;
	uint16_t vp = 0;
	SANE::str2dec(s, ix, d, vp);

	memoryWriteWord(ix, ixPtr);
	writeDecimal(dPtr, d);
	memoryWriteWord(vp, vpPtr);
}

static void wrap_dec2num() {
	SANE::decimal d = readDecimal(GetGPR(3));
	long double result = SANE::dec2x(d);
	SetFPR(1, (double)result);
}

static void wrap_dec2numl() {
	// long double == double on PPC 603e
	SANE::decimal d = readDecimal(GetGPR(3));
	long double result = SANE::dec2x(d);
	SetFPR(1, (double)result);
}

static void wrap_num2decl() {
	uint32_t fPtr = GetGPR(3);
	double x = GetFPR(1);
	uint32_t dPtr = GetGPR(4);

	SANE::decform f;
	f.style = memoryReadByte(fPtr);
	f.digits = (int16_t)memoryReadWord(fPtr + 2);

	SANE::decimal d = SANE::x2dec((long double)x, f);
	writeDecimal(dPtr, d);
}

// ================================================================
// PrivateInterfaceLib
// ================================================================

static void wrap_GetEmulatorRegister() {
	SetGPR(3, 0);
}

static void wrap_SetEmulatorRegister() {
	// no-op
}

} // anonymous namespace

// ================================================================
// Registration
// ================================================================

namespace PPCDispatch {

void RegisterStdCLibImports() {
	// -- Memory Manager (8) --
	reg("InterfaceLib", "NewPtr", wrap_NewPtr);
	reg("InterfaceLib", "DisposePtr", wrap_DisposePtr);
	reg("InterfaceLib", "GetPtrSize", wrap_GetPtrSize);
	reg("InterfaceLib", "SetPtrSize", wrap_SetPtrSize);
	reg("InterfaceLib", "NewHandle", wrap_NewHandle);
	reg("InterfaceLib", "DisposeHandle", wrap_DisposeHandle);
	reg("InterfaceLib", "HLock", wrap_HLock);
	reg("InterfaceLib", "HUnlock", wrap_HUnlock);

	// -- Memory Manager misc (4) --
	reg("InterfaceLib", "BlockMove", wrap_BlockMove);
	reg("InterfaceLib", "FreeMem", wrap_FreeMem);
	reg("InterfaceLib", "MemError", wrap_MemError);
	reg("InterfaceLib", "GetZone", wrap_GetZone);
	reg("InterfaceLib", "SetZone", wrap_SetZone);

	// -- File Manager PB (7) --
	reg("InterfaceLib", "PBHOpenSync", wrap_PBHOpenSync);
	reg("InterfaceLib", "PBHOpenRFSync", wrap_PBHOpenRFSync);
	reg("InterfaceLib", "PBCloseSync", wrap_PBCloseSync);
	reg("InterfaceLib", "PBHCreateSync", wrap_PBHCreateSync);
	reg("InterfaceLib", "PBSetEOFSync", wrap_PBSetEOFSync);
	reg("InterfaceLib", "PBGetCatInfoSync", wrap_PBGetCatInfoSync);
	reg("InterfaceLib", "PBGetFCBInfoSync", wrap_PBGetFCBInfoSync);

	// -- File Manager high-level (13) --
	reg("InterfaceLib", "FSRead", wrap_FSRead);
	reg("InterfaceLib", "FSWrite", wrap_FSWrite);
	reg("InterfaceLib", "FSClose", wrap_FSClose);
	reg("InterfaceLib", "Create", wrap_Create);
	reg("InterfaceLib", "FSDelete", wrap_FSDelete);
	reg("InterfaceLib", "GetFInfo", wrap_GetFInfo);
	reg("InterfaceLib", "HGetFInfo", wrap_HGetFInfo);
	reg("InterfaceLib", "SetFInfo", wrap_SetFInfo);
	reg("InterfaceLib", "HSetFInfo", wrap_HSetFInfo);
	reg("InterfaceLib", "SetEOF", wrap_SetEOF);
	reg("InterfaceLib", "GetFPos", wrap_GetFPos);
	reg("InterfaceLib", "SetFPos", wrap_SetFPos);
	reg("InterfaceLib", "HGetVol", wrap_HGetVol);
	reg("InterfaceLib", "HDelete", wrap_HDelete);
	reg("InterfaceLib", "FSMakeFSSpec", wrap_FSMakeFSSpec);
	reg("InterfaceLib", "ResolveAliasFile", wrap_ResolveAliasFile);
	reg("InterfaceLib", "Rename", wrap_Rename);

	// -- Trap Manager (1) --
	reg("InterfaceLib", "NGetTrapAddress", wrap_NGetTrapAddress);

	// -- Gestalt (1) --
	reg("InterfaceLib", "Gestalt", wrap_Gestalt);

	// -- Resource Manager (3) --
	reg("InterfaceLib", "ReleaseResource", wrap_ReleaseResource);
	reg("InterfaceLib", "ResError", wrap_ResError);
	reg("InterfaceLib", "CurResFile", wrap_CurResFile);

	// -- Time (4) --
	reg("InterfaceLib", "GetDateTime", wrap_GetDateTime);
	reg("InterfaceLib", "SecondsToDate", wrap_SecondsToDate);
	reg("InterfaceLib", "DateToSeconds", wrap_DateToSeconds);
	reg("InterfaceLib", "TickCount", wrap_TickCount);

	// -- String Conversions (4) --
	reg("InterfaceLib", "c2pstr", wrap_c2pstr);
	reg("InterfaceLib", "C2PStr", wrap_c2pstr);
	reg("InterfaceLib", "p2cstr", wrap_p2cstr);
	reg("InterfaceLib", "P2CStr", wrap_p2cstr);

	// -- Process Manager (3) --
	reg("InterfaceLib", "GetCurrentProcess", wrap_GetCurrentProcess);
	reg("InterfaceLib", "GetProcessInformation", wrap_GetProcessInformation);
	reg("InterfaceLib", "ExitToShell", wrap_ExitToShell);

	// -- Mixed Mode Manager (3) --
	reg("InterfaceLib", "NewRoutineDescriptor", wrap_NewRoutineDescriptor);
	reg("InterfaceLib", "DisposeRoutineDescriptor", wrap_DisposeRoutineDescriptor);

	// CallUniversalProc: PPC trampoline in emulated memory.
	// Checks for 0xAAFE (68K descriptor) vs PPC TVector.
	{
		uint32_t sc68K = CFMStubs::RegisterStub(
			"_MixedMode", "_CallUPP_68K", wrap_CallUniversalProc_68K);

		uint32_t sc68KCode = memoryReadLong(sc68K);
		uint32_t hi = (sc68KCode >> 16) & 0xFFFF;
		uint32_t lo = sc68KCode & 0xFFFF;

		uint32_t code[] = {
			0x7C0802A6, // mflr    r0
			0x90010008, // stw     r0, 8(r1)
			0x9421FFC0, // stwu    r1, -64(r1)
			0x90410038, // stw     r2, 56(r1)
			0xA0030000, // lhz     r0, 0(r3)
			0x2C00AAFE, // cmpwi   cr0, r0, -21762
			0x41820024, // beq     +36 (to 68K path at index 18)
			// PPC TVector path
			0x7C6C1B78, // mr      r12, r3
			0x7CA32B78, // mr      r3, r5
			0x7CC43378, // mr      r4, r6
			0x7CE53B78, // mr      r5, r7
			0x7D064378, // mr      r6, r8
			0x7D274B78, // mr      r7, r9
			0x7D485378, // mr      r8, r10
			0x800C0000, // lwz     r0, 0(r12)
			0x804C0004, // lwz     r2, 4(r12)
			0x7C0903A6, // mtctr   r0
			0x4E800421, // bctrl
			0x48000014, // b       +20 (to epilogue at index 23)
			// 68K RoutineDescriptor path
			0x3D800000 | hi,     // lis   r12, hi(sc68KCode)
			0x618C0000 | lo,     // ori   r12, r12, lo(sc68KCode)
			0x7D8903A6,          // mtctr r12
			0x4E800420,          // bctr
			// Epilogue
			0x80410038, // lwz     r2, 56(r1)
			0x38210040, // addi    r1, r1, 64
			0x80010008, // lwz     r0, 8(r1)
			0x7C0803A6, // mtlr    r0
			0x4E800020, // blr
		};

		uint32_t tvec = CFMStubs::AllocateCode(code, sizeof(code) / sizeof(code[0]));
		if (tvec) {
			CFMStubs::RegisterTVector("InterfaceLib", "CallUniversalProc", tvec);
		}
	}

	// -- Internationalization (1) --
	reg("InterfaceLib", "GetIntlResource", wrap_GetIntlResource);

	// -- Misc (3) --
	reg("InterfaceLib", "DebugStr", wrap_DebugStr);
	reg("InterfaceLib", "LMGetCurApName", wrap_LMGetCurApName);
	reg("InterfaceLib", "GetNodeAddress", wrap_GetNodeAddress);

	// -- MathLib (4) --
	reg("MathLib", "str2dec", wrap_str2dec);
	reg("MathLib", "dec2num", wrap_dec2num);
	reg("MathLib", "dec2numl", wrap_dec2numl);
	reg("MathLib", "num2decl", wrap_num2decl);

	// -- PrivateInterfaceLib (2) --
	reg("PrivateInterfaceLib", "GetEmulatorRegister", wrap_GetEmulatorRegister);
	reg("PrivateInterfaceLib", "SetEmulatorRegister", wrap_SetEmulatorRegister);
}

// ================================================================
// ECON Device Handlers (Phase 6)
// ================================================================

// Cookie address → host fd mapping
static std::map<uint32_t, int> cookieFdMap;

static int cookieToFd(uint32_t cookieHandle) {
	// Check host-side map first (most reliable)
	auto it = cookieFdMap.find(cookieHandle);
	if (it != cookieFdMap.end()) return it->second;

	// Dereference Handle → master ptr → cookie data → fd index at +0x00
	// Safety: validate addresses are in emulated memory range
	// (low addresses < 0x1000 are likely bare fd numbers, not Handles)
	if (cookieHandle < 0x10000) return -1;
	uint32_t masterPtr = memoryReadLong(cookieHandle);
	if (masterPtr < 0x10000) return -1;
	int32_t fdIndex = (int32_t)memoryReadLong(masterPtr);
	if (fdIndex >= 0 && fdIndex <= 2) return fdIndex;
	return -1;
}

static void econ_read() {
	uint32_t ioEntry = GetGPR(3);
	uint32_t cookie = memoryReadLong(ioEntry + 8);
	int hostFd = cookieToFd(cookie);
	uint32_t count = memoryReadLong(ioEntry + 12);
	uint32_t buffer = memoryReadLong(ioEntry + 16);

	if (MPW::Trace) {
		fprintf(stderr, "  ECON read(fd=%d, count=%u)\n", hostFd, count);
	}

	ssize_t n = ::read(hostFd, memoryPointer(buffer), count);
	if (n < 0) {
		memoryWriteWord(0xFFFF, ioEntry + 2);
		memoryWriteLong(0, ioEntry + 12);
	} else {
		memoryWriteWord(0, ioEntry + 2);
		memoryWriteLong((uint32_t)n, ioEntry + 12);
	}
	SetGPR(3, n < 0 ? (uint32_t)-1 : 0);
}

static void econ_write() {
	uint32_t ioEntry = GetGPR(3);
	uint32_t cookie = memoryReadLong(ioEntry + 8);
	int hostFd = cookieToFd(cookie);
	uint32_t count = memoryReadLong(ioEntry + 12);
	uint32_t buffer = memoryReadLong(ioEntry + 16);

	if (MPW::Trace) {
		fprintf(stderr, "  ECON write(fd=%d, count=%u, buf=\"%.*s\")\n",
		        hostFd, count, std::min(count, 40u),
		        (const char *)memoryPointer(buffer));
	}

	// CR→LF conversion
	uint8_t *ptr = memoryPointer(buffer);
	std::vector<uint8_t> converted;
	for (uint32_t i = 0; i < count; i++) {
		uint8_t c = ptr[i];
		converted.push_back(c == '\r' ? '\n' : c);
	}

	ssize_t n = ::write(hostFd, converted.data(), converted.size());
	memoryWriteWord(n < 0 ? 0xFFFF : 0, ioEntry + 2);
	memoryWriteLong(n < 0 ? 0 : (uint32_t)n, ioEntry + 12);
	SetGPR(3, n < 0 ? (uint32_t)-1 : 0);
}

static void econ_close() {
	if (MPW::Trace) fprintf(stderr, "  ECON close()\n");
	SetGPR(3, 0);
}

static void econ_ioctl() {
	uint32_t ioEntry = GetGPR(3);
	uint32_t cmd = GetGPR(4);
	uint32_t arg = GetGPR(5);

	if (MPW::Trace) {
		static const char *cmdNames[] = {"?","FIODUPFD","FIOINTERACTIVE",
		                                  "FIOBUFSIZE","?","FIOREFNUM"};
		const char *cmdName = (cmd >= 0x6601 && cmd <= 0x6605)
		                      ? cmdNames[cmd - 0x6600] : "unknown";
		fprintf(stderr, "  ECON ioctl(cmd=%s/0x%04X, arg=0x%08X)\n",
		        cmdName, cmd, arg);
	}

	switch (cmd) {
	case 0x6601: { // FIODUPFD
		uint32_t cookie = memoryReadLong(ioEntry + 8);
		int fd = cookieToFd(cookie);
		if (MPW::Trace)
			fprintf(stderr, "  ECON FIODUPFD: cookie=0x%08X fd=%d\n", cookie, fd);
		if (fd >= 0) cookieFdMap[cookie] = fd;
		SetGPR(3, 0);
		break;
	}
	case 0x6602: // FIOINTERACTIVE
		SetGPR(3, 0); // 0 = interactive
		break;
	case 0x6603: // FIOBUFSIZE
		if (arg) memoryWriteLong(2048, arg);
		SetGPR(3, 0);
		break;
	case 0x6605: // FIOREFNUM
		SetGPR(3, (uint32_t)(int32_t)-1);
		break;
	default:
		if (MPW::Trace)
			fprintf(stderr, "    unknown ECON ioctl 0x%04X\n", cmd);
		SetGPR(3, (uint32_t)(int32_t)-1);
		break;
	}

	if (MPW::Trace)
		fprintf(stderr, "    -> %d\n", (int32_t)GetGPR(3));
}

static void econ_faccess() {
	if (MPW::Trace) fprintf(stderr, "  ECON faccess()\n");
	SetGPR(3, 0);
}

// Allocate a cookie Handle for a stdio fd.
static uint32_t allocateCookieHandle(int fdIndex) {
	// Allocate cookie data
	uint32_t cookieData = 0;
	uint32_t cookieHandle = 0;
	uint16_t error = MM::Native::NewHandle(0x10, true, cookieHandle, cookieData);
	if (error) return 0;

	memoryWriteLong(fdIndex, cookieData + 0x00);  // fd index for FIODUPFD
	memoryWriteLong(fdIndex, cookieData + 0x04);  // fd index for _coWrite
	memoryWriteByte(0, cookieData + 0x0C);         // not connected

	return cookieHandle;
}

void PatchDeviceTable(uint32_t mpgmInfoAddr) {
	// Read device table and IO table pointers from MPGM info block
	uint32_t devTablePtr = memoryReadLong(mpgmInfoAddr + 0x20);
	uint32_t ioTablePtr = memoryReadLong(mpgmInfoAddr + 0x1C);

	if (!devTablePtr || !ioTablePtr) return;

	// Register ECON handler sc stubs
	uint32_t econFaccess = CFMStubs::RegisterStub("_ECON", "faccess", econ_faccess);
	uint32_t econClose   = CFMStubs::RegisterStub("_ECON", "close", econ_close);
	uint32_t econRead    = CFMStubs::RegisterStub("_ECON", "read", econ_read);
	uint32_t econWrite   = CFMStubs::RegisterStub("_ECON", "write", econ_write);
	uint32_t econIoctl   = CFMStubs::RegisterStub("_ECON", "ioctl", econ_ioctl);

	// Write TVectors into the ECON device table entry (devTable + 24)
	uint32_t econEntry = devTablePtr + 24;
	// 'ECON' name is already written by MPW::Init()
	memoryWriteLong(econFaccess, econEntry + 4);
	memoryWriteLong(econClose, econEntry + 8);
	memoryWriteLong(econRead, econEntry + 12);
	memoryWriteLong(econWrite, econEntry + 16);
	memoryWriteLong(econIoctl, econEntry + 20);

	// Allocate Handle-based cookies and patch IO table
	struct { int fd; uint16_t flags; } fds[] = {
		{STDIN_FILENO,  0x0001},  // read
		{STDOUT_FILENO, 0x0002},  // write
		{STDERR_FILENO, 0x0002},  // write
	};

	// Patch IO table entries: allocate Handle cookies, point at ECON device
	for (int i = 0; i < 3; i++) {
		uint32_t ioEntry = ioTablePtr + i * 20;
		uint32_t cookieHandle = allocateCookieHandle(fds[i].fd);

		if (cookieHandle) {
			memoryWriteLong(cookieHandle, ioEntry + 8);
		}

		memoryWriteLong(econEntry, ioEntry + 4);
	}
}

} // namespace PPCDispatch
