#ifndef __mpw_pef_h__
#define __mpw_pef_h__

#include <cstdint>

namespace PEF {

// Container header magic values
static const uint32_t kPEFTag1 = 0x4A6F7921; // 'Joy!'
static const uint32_t kPEFTag2 = 0x70656666; // 'peff'

// Architecture tags
static const uint32_t kPEFArchPPC = 0x70777063; // 'pwpc'
static const uint32_t kPEFArch68K = 0x6D36386B; // 'm68k'

// Section kinds
enum SectionKind : uint8_t {
	kCode               = 0,
	kUnpackedData       = 1,
	kPatternInitData    = 2,
	kConstant           = 3,
	kLoader             = 4,
	kDebug              = 5,
	kExecutableData     = 6,
	kException          = 7,
	kTraceback          = 8,
};

// Symbol classes (bits 27-24 of classAndName)
enum SymbolClass : uint8_t {
	kCodeSymbol         = 0,
	kDataSymbol         = 1,
	kTVectorSymbol      = 2,
	kTOCSymbol          = 3,
	kGlueSymbol         = 4,
};

// Imported library option flags
enum LibraryOptions : uint8_t {
	kInitBefore         = 0x80, // must init before client
	kWeakImport         = 0x40, // library is optional
};

// Imported symbol flags (bits 31-28 of classAndName)
enum SymbolFlags : uint8_t {
	kWeakSymbol         = 0x08,
};

// Export hash helpers
static const uint32_t kPEFHashLengthShift = 16;
static const uint32_t kPEFHashValueMask   = 0x0000FFFF;

} // namespace PEF

#endif
