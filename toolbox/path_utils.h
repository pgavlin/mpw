#ifndef __toolbox_path_utils_h__
#define __toolbox_path_utils_h__

#include <string>

namespace OS {

	// Resolve a Unix path with case-insensitive matching.
	//
	// Tries the path as-is first (fast path). If that fails with ENOENT,
	// walks each path component and does a case-insensitive directory scan
	// to find the correct on-disk casing.
	//
	// Returns the resolved path on success. If the file does not exist
	// even case-insensitively, returns the original path unchanged (so
	// the caller gets the expected ENOENT from subsequent syscalls).
	//
	// If resolve_leaf is false, only resolves the directory portion of
	// the path. Use this for O_CREAT paths where the final component
	// may not exist yet.
	std::string resolve_path_ci(const std::string &path, bool resolve_leaf = true);

}

#endif
