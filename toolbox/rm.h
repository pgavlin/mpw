#ifndef __mpw_rm_h__
#define __mpw_rm_h__

#include <cstdint>
#include <string>

namespace RM
{

	namespace Native
	{
		uint16_t SetResLoad(bool tf);
		uint16_t GetResource(uint32_t type, uint16_t id, uint32_t &theHandle);
		uint16_t OpenResourceFile(const std::string &path, uint16_t permission, int16_t &outRefNum);

		int16_t CurResFile();
		uint16_t UseResFile(int16_t refNum);
		uint16_t CloseResFile(int16_t refNum);
		int16_t OpenResFile(const std::string &name);
		uint16_t ReleaseResource(uint32_t theResource);
		int16_t HomeResFile(uint32_t theResource);
		uint16_t GetResAttrs(uint32_t theResource);
		uint16_t GetResInfo(uint32_t theResource, int16_t *theID, uint32_t *theType, std::string *name);
		int32_t GetResourceSizeOnDisk(uint32_t theResource);
		uint16_t UpdateResFile(int16_t refNum);
		uint16_t AddResource(uint32_t theData, uint32_t theType, int16_t theID, const std::string &name);
		uint16_t ChangedResource(uint32_t theResource);
		uint16_t RemoveResource(uint32_t theResource);
		uint16_t ResError();
	}

	uint16_t CloseResFile(uint16_t trap);
	uint16_t GetNamedResource(uint16_t trap);
	uint16_t Get1NamedResource(uint16_t trap);

	uint16_t GetResource(uint16_t trap);
	uint16_t Get1Resource(uint16_t trap);
	uint16_t Get1IndResource(uint16_t trap);

	uint16_t ReleaseResource(uint16_t trap);
	uint16_t ResError(uint16_t trap);

	uint16_t OpenResFile(uint16_t trap);
	uint16_t OpenRFPerm(uint16_t trap);
	uint16_t HOpenResFile(uint16_t trap);

	uint16_t SetResLoad(uint16_t trap);

	uint16_t CurResFile(uint16_t trap);
	uint16_t UseResFile(uint16_t trap);

	uint16_t CreateResFile(uint16_t trap);
	uint16_t HCreateResFile(uint16_t trap);

	uint16_t Count1Resources(uint16_t trap);
	uint16_t UpdateResFile(uint16_t trap);
	uint16_t GetResFileAttrs(uint16_t trap);
	uint16_t SetResFileAttrs(uint16_t trap);

	uint16_t AddResource(uint16_t trap);
	uint16_t GetResAttrs(uint16_t trap);
	uint16_t SetResAttrs(uint16_t trap);
	uint16_t WriteResource(uint16_t trap);
	uint16_t DetachResource(uint16_t trap);
	uint16_t ChangedResource(uint16_t trap);
	uint16_t RemoveResource(uint16_t trap);

	uint16_t GetResourceSizeOnDisk(uint16_t trap);

	uint16_t GetResInfo(uint16_t trap);
	uint16_t LoadResource(uint16_t trap);

	uint16_t HomeResFile(uint16_t trap);
	uint16_t Count1Types(uint16_t trap);
	uint16_t Get1IndType(uint16_t trap);


	uint16_t FSpOpenResFile(void);
	uint16_t FSpCreateResFile(void);

	/* Query if a refNum belongs to the Resource Manager.
	 * Returns true and fills path/permission if it does. */
	bool IsRMRefNum(int16_t refNum, std::string *outPath = nullptr);

}


#endif
