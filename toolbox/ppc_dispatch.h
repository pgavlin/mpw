#ifndef __mpw_ppc_dispatch_h__
#define __mpw_ppc_dispatch_h__

namespace PPCDispatch {
	// Register the 66 stubs that StdCLib imports from
	// InterfaceLib, MathLib, and PrivateInterfaceLib.
	void RegisterStdCLibImports();
}

#endif
