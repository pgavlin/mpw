#ifndef __mpw_cfm_stubs_h__
#define __mpw_cfm_stubs_h__

#include <cstdint>
#include <string>
#include <functional>

namespace CFMStubs {

	// Native C++ function called when a stub is invoked.
	// Should read parameters from PPC::GetGPR(3..10) and write
	// results to PPC::SetGPR(3) (or FPR for float returns).
	using Handler = std::function<void()>;

	// Initialize the stub system. Allocates emulated memory for
	// up to kMaxStubs stub entries.
	void Init();

	// Register a native handler for a library::symbol pair.
	// Returns the TVector address in emulated memory.
	// If already registered, returns the existing TVector address.
	uint32_t RegisterStub(const std::string &library,
	                      const std::string &symbol,
	                      Handler handler);

	// Look up a previously registered stub or TVector.
	// Returns the TVector address, or 0 if not found.
	uint32_t ResolveImport(const std::string &library,
	                       const std::string &symbol);

	// Called from the PPC sc handler. Reads r11 to determine which
	// stub was invoked and calls the corresponding handler.
	void Dispatch();

	// Enable/disable dispatch tracing to stderr.
	void SetTrace(bool trace);

	// Register an existing TVector address (e.g., an export from a
	// loaded PEF) as the resolution for a library::symbol import.
	// Used to register StdCLib exports for tool import resolution.
	void RegisterTVector(const std::string &library,
	                     const std::string &symbol,
	                     uint32_t tvecAddr);

	// Allocate a block of PPC code in the stub code region and create
	// a TVector for it. Returns the TVector address.
	// Used for the CallUniversalProc trampoline and similar native
	// PPC code that must exist in emulated memory.
	uint32_t AllocateCode(const uint32_t *instructions,
	                      uint32_t count);
}

#endif
