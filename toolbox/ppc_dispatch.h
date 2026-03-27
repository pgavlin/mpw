#ifndef __mpw_ppc_dispatch_h__
#define __mpw_ppc_dispatch_h__

#include <cstdint>

namespace PPCDispatch {
	// Register the 66 stubs that StdCLib imports from
	// InterfaceLib, MathLib, and PrivateInterfaceLib.
	void RegisterStdCLibImports();

	// Patch the MPGM device table and IO table for PPC:
	// - Fill ECON device entry with PPC-callable sc stub TVectors
	// - Replace bare fd cookies with Handle-based cookies
	// - Point IO entries at the ECON device entry
	void PatchDeviceTable(uint32_t mpgmInfoAddr);
}

#endif
