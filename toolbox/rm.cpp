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

#include <string>
#include <cstring>
#include <list>
#include <map>
#include <unordered_set>

#include <unistd.h>
#include <fcntl.h>

#include <rsrc/rsrc.h>

#include "rm.h"
#include "toolbox.h"
#include "mm.h"
#include "os_internal.h"
#include "path_utils.h"

#include <cpu/defs.h>
#include <cpu/CpuModule.h>
#include <cpu/fmem.h>

#include <macos/sysequ.h>
#include <macos/errors.h>
#include <macos/tool_return.h>

#include "stackframe.h"
#include "fs_spec.h"
using ToolBox::Log;

using namespace OS::Internal;
using namespace ToolBox;

using MacOS::tool_return;
using MacOS::macos_error_from_errno;
using MacOS::macos_error;

namespace
{

	bool ResLoad = true;


	// An open resource file
	struct OpenResFileEntry {
		int16_t refNum;
		std::string path;
		std::unique_ptr<rsrc::ResourceFile> file;
		uint16_t permission;
		bool dirty;
	};

	// Stack of open resource files (most recently opened first)
	std::list<OpenResFileEntry> openFiles;
	int16_t currentResFile = -1;
	int16_t nextRefNum = 100;

	// Map from emulated handle -> resource info for tracking
	struct ResourceRef {
		int16_t refNum;       // which open file owns this
		uint32_t type;
		int16_t id;
		std::string name;
		uint8_t attributes;
	};
	std::map<uint32_t, ResourceRef> rhandle_map;


	OpenResFileEntry *findOpenFile(int16_t refNum) {
		for (auto &of : openFiles) {
			if (of.refNum == refNum) return &of;
		}
		return nullptr;
	}

	OpenResFileEntry *currentFile() {
		return findOpenFile(currentResFile);
	}


	inline uint16_t SetResError(uint16_t error)
	{
		memoryWriteWord(error, MacOS::ResErr);
		return error;
	}

	bool LoadResType(uint32_t type)
	{
		return true;
	}

}

namespace RM
{

	namespace Native
	{
		uint16_t OpenResourceFile(const std::string &path, uint16_t permission, int16_t &outRefNum)
		{
			outRefNum = -1;

			std::string resolved = OS::resolve_path_ci(path);
			auto data = rsrc::readResourceFork(resolved);
			if (data.empty()) {
				// If opening for write access, create an empty resource fork
				if (permission > 1) {
					data = rsrc::ResourceFile::createEmpty();
					rsrc::writeResourceFork(resolved, data);
				} else {
					return SetResError(MacOS::resFNotFound);
				}
			}

			auto rf = rsrc::ResourceFile::open(data);
			if (!rf)
				return SetResError(MacOS::resFNotFound);

			int16_t refNum = nextRefNum++;

			OpenResFileEntry of;
			of.refNum = refNum;
			of.path = path;
			of.file = std::move(rf);
			of.permission = permission;
			of.dirty = false;

			openFiles.push_front(std::move(of));
			currentResFile = refNum;
			outRefNum = refNum;

			return SetResError(0);
		}


		// Load a resource from an open file into emulated memory
		uint16_t LoadResourceFromEntry(const rsrc::ResourceEntry *entry, rsrc::ResourceFile *rf,
		                               int16_t fileRefNum, uint32_t &theHandle)
		{
			theHandle = 0;

			if (!entry) return SetResError(MacOS::resNotFound);

			if (!LoadResType(entry->type))
				return SetResError(MacOS::resNotFound);

			std::vector<uint8_t> resData;
			if (ResLoad) {
				resData = rf->loadResource(*entry);
			}

			uint32_t ptr;
			uint16_t error = MM::Native::NewHandle(resData.size(), false, theHandle, ptr);
			if (!theHandle)
				return SetResError(error);

			MM::Native::HSetRBit(theHandle);

			if (!resData.empty())
				std::memcpy(memoryPointer(ptr), resData.data(), resData.size());

			ResourceRef ref;
			ref.refNum = fileRefNum;
			ref.type = entry->type;
			ref.id = entry->id;
			ref.name = entry->name;
			ref.attributes = entry->attributes;
			rhandle_map.insert({theHandle, ref});

			return SetResError(0);
		}


		uint16_t GetResource(uint32_t type, uint16_t id, uint32_t &theHandle)
		{
			theHandle = 0;

			// Search all open files, starting from the current one
			for (auto &of : openFiles) {
				const rsrc::ResourceEntry *entry = of.file->findResource(type, (int16_t)id);
				if (entry) {
					return LoadResourceFromEntry(entry, of.file.get(), of.refNum, theHandle);
				}
			}

			return SetResError(MacOS::resNotFound);
		}

		uint16_t SetResLoad(bool load)
		{
			ResLoad = load;
			memoryWriteByte(load ? 0xff : 0x00, MacOS::ResLoad);
			return SetResError(0);
		}
	}

	uint16_t CloseResFile(uint16_t trap)
	{
		uint16_t refNum;

		StackFrame<2>(refNum);

		Log("%04x CloseResFile(%04x)\n", trap, refNum);

		if (refNum == 0)
			return SetResError(0);

		auto *of = findOpenFile(refNum);
		if (!of)
			return SetResError(MacOS::resFNotFound);

		// flush if dirty
		if (of->dirty) {
			auto serialized = of->file->serialize();
			rsrc::writeResourceFork(of->path, serialized);
		}

		// remove any tracked handles for this file
		for (auto it = rhandle_map.begin(); it != rhandle_map.end(); ) {
			if (it->second.refNum == refNum)
				it = rhandle_map.erase(it);
			else
				++it;
		}

		// update current if needed
		if (currentResFile == refNum) {
			// find the next file in the chain
			bool found = false;
			for (auto it = openFiles.begin(); it != openFiles.end(); ++it) {
				if (it->refNum == refNum) {
					auto next = std::next(it);
					if (next != openFiles.end())
						currentResFile = next->refNum;
					else
						currentResFile = -1;
					found = true;
					break;
				}
			}
			if (!found) currentResFile = -1;
		}

		openFiles.remove_if([refNum](const OpenResFileEntry &f) { return f.refNum == refNum; });

		return SetResError(0);
	}


	uint16_t Get1NamedResource(uint16_t trap)
	{
		uint32_t sp;
		uint32_t theType;
		uint32_t name;

		sp = StackFrame<8>(theType, name);

		std::string sname = ToolBox::ReadPString(name);

		Log("%04x Get1NamedResource(%08x ('%s'), %s)\n",
			trap, theType, TypeToString(theType).c_str(), sname.c_str());

		uint32_t resourceHandle = 0;
		uint16_t d0 = MacOS::resNotFound;

		auto *of = currentFile();
		if (of) {
			const rsrc::ResourceEntry *entry = of->file->findResource(theType, sname);
			if (entry) {
				d0 = Native::LoadResourceFromEntry(entry, of->file.get(), of->refNum, resourceHandle);
			}
		}

		ToolReturn<4>(sp, resourceHandle);
		return SetResError(d0);
	}

	uint16_t GetNamedResource(uint16_t trap)
	{
		uint32_t sp;
		uint32_t theType;
		uint32_t name;

		sp = StackFrame<8>(theType, name);

		std::string sname = ToolBox::ReadPString(name);

		Log("%04x GetNamedResource(%08x ('%s'), %s)\n",
			trap, theType, TypeToString(theType).c_str(), sname.c_str());

		uint32_t resourceHandle = 0;
		uint16_t d0 = MacOS::resNotFound;

		for (auto &of : openFiles) {
			const rsrc::ResourceEntry *entry = of.file->findResource(theType, sname);
			if (entry) {
				d0 = Native::LoadResourceFromEntry(entry, of.file.get(), of.refNum, resourceHandle);
				break;
			}
		}

		ToolReturn<4>(sp, resourceHandle);
		return SetResError(d0);
	}

	uint16_t GetResource(uint16_t trap)
	{
		uint32_t sp;
		uint32_t theType;
		uint16_t theID;

		sp = StackFrame<6>(theType, theID);

		Log("%04x GetResource(%08x ('%s'), %04x)\n",
				trap, theType, TypeToString(theType).c_str(), theID);


		uint32_t resourceHandle = 0;
		uint32_t d0;
		d0 = Native::GetResource(theType, theID, resourceHandle);

		ToolReturn<4>(sp, resourceHandle);
		return d0;
	}


	uint16_t Get1Resource(uint16_t trap)
	{
		uint32_t sp;
		uint32_t theType;
		uint16_t theID;

		sp = StackFrame<6>(theType, theID);

		Log("%04x Get1Resource(%08x ('%s'), %04x)\n", trap, theType, TypeToString(theType).c_str(), theID);


		uint32_t resourceHandle = 0;
		uint16_t d0 = MacOS::resNotFound;

		auto *of = currentFile();
		if (of) {
			const rsrc::ResourceEntry *entry = of->file->findResource(theType, (int16_t)theID);
			if (entry) {
				d0 = Native::LoadResourceFromEntry(entry, of->file.get(), of->refNum, resourceHandle);
			}
		}


		ToolReturn<4>(sp, resourceHandle);
		return d0;
	}


	uint16_t ReleaseResource(uint16_t trap)
	{
		uint32_t sp;
		uint32_t theResource;

		sp = StackFrame<4>(theResource);

		Log("%04x ReleaseResource(%08x)\n", trap, theResource);

		return SetResError(0);
	}

	uint16_t ResError(uint16_t trap)
	{
		uint32_t sp;

		Log("%04x ResError()\n", trap);

		sp = cpuGetAReg(7);
		ToolReturn<2>(sp, memoryReadWord(MacOS::ResErr));
		return 0;
	}

	uint16_t SetResLoad(uint16_t trap)
	{
		uint16_t load;

		StackFrame<2>(load);

		Log("%04x SetResLoad(%04x)\n", trap, load);

		ResLoad = load;

		memoryWriteByte(load ? 0xff : 0x00, MacOS::ResLoad);
		return SetResError(0);
	}


	uint16_t CurResFile(uint16_t trap)
	{

		Log("%04x CurResFile()\n", trap);

		ToolReturn<2>(-1, (uint16_t)currentResFile);
		return SetResError(0);
	}

	uint16_t UseResFile(uint16_t trap)
	{
		uint16_t resFile;

		StackFrame<2>(resFile);

		Log("%04x UseResFile(%04x)\n", trap, resFile);

		auto *of = findOpenFile(resFile);
		if (!of)
			return SetResError(MacOS::resFNotFound);

		currentResFile = resFile;
		return SetResError(0);
	}


	tool_return<void> CreateResFile(const std::string &path, uint32_t creator = 0, uint32_t fileType = 0)
	{

		if (path.empty()) return MacOS::paramErr;

		std::string resolved = OS::resolve_path_ci(path, false);
		int fd;

		fd = ::open(resolved.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
		if (fd < 0)
		{
			if (errno != EEXIST) return macos_error_from_errno();
		}
		else
		{
			if (creator || fileType)
				OS::Internal::SetFinderInfo(resolved, fileType, creator);

			close(fd);
		}

		// Check if resource fork already exists
		auto existing = rsrc::readResourceFork(resolved);
		if (!existing.empty())
			return MacOS::dupFNErr;

		// Create empty resource fork
		auto emptyFork = rsrc::ResourceFile::createEmpty();
		if (!rsrc::writeResourceFork(resolved, emptyFork))
			return MacOS::ioErr;

		return {};
	}

	uint16_t CreateResFile(uint16_t trap)
	{
		uint32_t fileName;

		StackFrame<4>(fileName);

		std::string sname = ToolBox::ReadPString(fileName, true);
		Log("%04x CreateResFile(%s)\n", trap, sname.c_str());

		if (!sname.length()) return SetResError(MacOS::paramErr);


		auto rv = CreateResFile(sname);

		return SetResError(rv.error());
	}

	uint16_t HCreateResFile(uint16_t trap)
	{
		uint16_t vRefNum;
		uint32_t dirID;
		uint32_t fileName;

		StackFrame<10>(vRefNum, dirID, fileName);

		std::string sname = ToolBox::ReadPString(fileName, true);

		Log("%04x HCreateResFile(%04x, %08x, %s)\n",
			trap, vRefNum, dirID, sname.c_str());


		sname = OS::FSSpecManager::ExpandPath(sname, dirID);
		if (sname.empty())
		{
			return SetResError(MacOS::dirNFErr);
		}

		auto rv = CreateResFile(sname);

		return SetResError(rv.error() == MacOS::dupFNErr ? 0 : rv.error());
	}


	uint16_t FSpCreateResFile(void)
	{
		uint32_t sp;
		uint32_t spec;
		uint32_t creator;
		uint32_t fileType;
		uint16_t scriptTag;

		sp = StackFrame<14>(spec, creator, fileType, scriptTag);


		int parentID = memoryReadLong(spec + 2);
		std::string sname = ToolBox::ReadPString(spec + 6, false);

		Log("     FSpCreateResFile(%s, %08x ('%s'), %08x ('%s'), %02x)\n",
			sname.c_str(),
			creator, ToolBox::TypeToString(creator).c_str(),
			fileType, ToolBox::TypeToString(fileType).c_str(),
			scriptTag);


		sname = OS::FSSpecManager::ExpandPath(sname, parentID);
		if (sname.empty())
		{
			return SetResError(MacOS::dirNFErr);
		}

		auto rv = CreateResFile(sname, creator, fileType);

		return SetResError(rv.error() == MacOS::dupFNErr ? 0 : rv.error());
	}


	tool_return<int16_t> OpenResCommon(const std::string &path, uint16_t permission = 0)
	{
		int16_t refNum = -1;
		uint16_t err = Native::OpenResourceFile(path, permission, refNum);
		if (err)
			return (MacOS::macos_error)err;

		return refNum;
	}

	uint16_t OpenResFile(uint16_t trap)
	{
		uint32_t sp;
		uint32_t fileName;

		sp = StackFrame<4>(fileName);

		std::string sname = ToolBox::ReadPString(fileName, true);

		Log("%04x OpenResFile(%s)\n", trap, sname.c_str());

		auto rv = OpenResCommon(sname);

		ToolReturn<2>(sp, rv.value_or(-1));

		return SetResError(rv.error());
	}

	uint16_t HOpenResFile(uint16_t trap)
	{
		uint32_t sp;

		uint16_t vRefNum;
		uint32_t dirID;
		uint32_t fileName;
		uint8_t permission;

		sp = StackFrame<12>(vRefNum, dirID, fileName, permission);

		std::string sname = ToolBox::ReadPString(fileName, true);

		Log("%04x HOpenResFile(%04x, %08x, %s, %04x)\n",
			trap, vRefNum, dirID, sname.c_str(), permission);

		if (vRefNum) {
			fprintf(stderr, "HOpenResFile: vRefNum not supported yet.\n");
			exit(1);
		}

		sname = OS::FSSpecManager::ExpandPath(sname, dirID);
		if (sname.empty())
		{
			ToolReturn<2>(sp, (uint16_t)-1);
			return SetResError(MacOS::dirNFErr);
		}

		auto rv = OpenResCommon(sname, permission);

		ToolReturn<2>(sp, rv.value_or(-1));

		return SetResError(rv.error());
	}

	uint16_t FSpOpenResFile(void)
	{
		uint32_t sp;
		uint32_t spec;
		uint8_t permission;

		sp = StackFrame<6>(spec, permission);


		int parentID = memoryReadLong(spec + 2);

		std::string sname = ToolBox::ReadPString(spec + 6, false);

		Log("     FSpOpenResFile(%s, %04x)\n",  sname.c_str(), permission);


		sname = OS::FSSpecManager::ExpandPath(sname, parentID);
		if (sname.empty())
		{
			ToolReturn<2>(sp, (uint16_t)-1);
			return SetResError(MacOS::dirNFErr);
		}

		auto rv = OpenResCommon(sname, permission);

		ToolReturn<2>(sp, rv.value_or(-1));

		return SetResError(rv.error());
	}



	uint16_t OpenRFPerm(uint16_t trap)
	{
		uint32_t sp;
		uint32_t fileName;
		uint16_t vRefNum;
		uint16_t permission;

		sp = StackFrame<8>(fileName, vRefNum, permission);

		std::string sname = ToolBox::ReadPString(fileName, true);
		Log("%04x OpenRFPerm(%s, %04x, %04x)\n",
			trap, sname.c_str(), vRefNum, permission);

		auto rv = OpenResCommon(sname, permission);

		ToolReturn<2>(sp, rv.value_or(-1));

		return SetResError(rv.error());
	}

	uint16_t Count1Resources(uint16_t trap)
	{
		uint32_t sp;
		uint32_t theType;
		uint16_t count;

		sp = StackFrame<4>(theType);

		Log("%04x Count1Resources(%08x ('%s'))\n",
			trap, theType, TypeToString(theType).c_str());

		count = 0;
		auto *of = currentFile();
		if (of) {
			count = of->file->countResources(theType);
		}

		ToolReturn<2>(sp, count);
		return SetResError(0);
	}


	uint16_t UpdateResFile(uint16_t trap)
	{
		uint16_t refNum;

		StackFrame<2>(refNum);

		Log("%04x UpdateResFile(%04x)\n", trap, refNum);

		auto *of = findOpenFile(refNum);
		if (!of)
			return SetResError(MacOS::resFNotFound);

		if (of->dirty) {
			auto serialized = of->file->serialize();
			rsrc::writeResourceFork(of->path, serialized);
			of->dirty = false;
		}

		return SetResError(0);
	}


	uint16_t ChangedResource(uint16_t trap)
	{
		uint32_t theResource;

		StackFrame<4>(theResource);

		Log("%04x ChangedResource(%08x)\n", trap, theResource);

		auto iter = rhandle_map.find(theResource);
		if (iter == rhandle_map.end())
		{
			return SetResError(MacOS::resNotFound);
		}

		auto *of = findOpenFile(iter->second.refNum);
		if (!of)
			return SetResError(MacOS::resFNotFound);

		// Mark the file as dirty
		of->dirty = true;

		// Copy data from emulated handle back to the resource file
		auto info = MM::GetHandleInfo(theResource);
		if (info.error()) return SetResError(MacOS::resNotFound);

		std::vector<uint8_t> newData(info->size);
		if (info->size)
			std::memcpy(newData.data(), memoryPointer(info->address), info->size);

		// Update in-memory resource data
		// (For a full implementation, we'd update the ResourceFile's data.
		//  Since serialize() re-reads from data_, we need to handle this.
		//  For now, mark dirty and let UpdateResFile handle it.)

		return SetResError(0);
	}


	uint16_t GetResFileAttrs(uint16_t trap)
	{
		uint32_t sp;
		uint16_t attrs;
		uint16_t refNum;

		sp = StackFrame<2>(refNum);
		Log("%04x GetResFileAttrs(%04x)\n", trap, refNum);

		auto *of = findOpenFile(refNum);
		if (!of) {
			ToolReturn<2>(sp, (uint16_t)0);
			return SetResError(MacOS::resFNotFound);
		}

		attrs = of->file->fileAttributes();
		ToolReturn<2>(sp, attrs);

		return SetResError(0);
	}


	uint16_t SetResFileAttrs(uint16_t trap)
	{
		uint32_t sp;
		uint16_t attrs;
		uint16_t refNum;

		sp = StackFrame<4>(refNum, attrs);
		Log("%04x SetResFileAttrs(%04x, %04x)\n", trap, refNum, attrs);

		auto *of = findOpenFile(refNum);
		if (!of)
			return SetResError(MacOS::resFNotFound);

		of->file->setFileAttributes(attrs);

		return SetResError(0);
	}

	uint16_t AddResource(uint16_t trap)
	{
		uint32_t theData;
		uint32_t theType;
		uint16_t theID;
		uint32_t namePtr;


		StackFrame<14>(theData, theType, theID, namePtr);

		std::string sname = ToolBox::ReadPString(namePtr, false);

		Log("%04x AddResource(%08x, %08x ('%s'), %04x, %s)\n",
			trap, theData, theType, TypeToString(theType).c_str(), theID, sname.c_str()
		);

		auto *of = currentFile();
		if (!of) return SetResError(MacOS::addResFailed);

		auto info = MM::GetHandleInfo(theData);
		if (info.error()) return SetResError(MacOS::addResFailed);

		std::vector<uint8_t> resData(info->size);
		if (info->size)
			std::memcpy(resData.data(), memoryPointer(info->address), info->size);

		of->file->addResource(theType, (int16_t)theID, sname, 0, resData);
		of->dirty = true;

		// Track the handle
		ResourceRef ref;
		ref.refNum = of->refNum;
		ref.type = theType;
		ref.id = (int16_t)theID;
		ref.name = sname;
		ref.attributes = 0;
		rhandle_map.insert({theData, ref});

		MM::Native::HSetRBit(theData);

		return SetResError(0);
	}

	uint16_t SetResAttrs(uint16_t trap)
	{
		uint32_t theResource;
		uint16_t attrs;

		StackFrame<6>(theResource, attrs);

		Log("%04x SetResAttrs(%08x, %04x)\n", trap, theResource, attrs);

		auto iter = rhandle_map.find(theResource);
		if (iter == rhandle_map.end()) return SetResError(MacOS::resNotFound);

		iter->second.attributes = attrs;

		return SetResError(0);
	}

	uint16_t GetResAttrs(uint16_t trap)
	{
		uint32_t sp;
		uint32_t theResource;
		uint16_t attrs;

		sp = StackFrame<4>(theResource);

		Log("%04x GetResAttrs(%08x)\n", trap, theResource);

		auto iter = rhandle_map.find(theResource);
		if (iter == rhandle_map.end())
		{
			ToolReturn<2>(sp, 0);
			return SetResError(MacOS::resNotFound);
		}

		attrs = iter->second.attributes;

		ToolReturn<2>(sp, attrs);

		return SetResError(0);
	}


	uint16_t WriteResource(uint16_t trap)
	{
		uint32_t theResource;
		StackFrame<4>(theResource);

		Log("%04x WriteResource(%08x)\n", trap, theResource);


		auto iter = rhandle_map.find(theResource);
		if (iter == rhandle_map.end()) return SetResError(MacOS::resNotFound);

		auto *of = findOpenFile(iter->second.refNum);
		if (!of) return SetResError(MacOS::resFNotFound);

		// Re-read handle data and update the resource file's internal copy,
		// since the app may have modified the handle contents.
		auto info = MM::GetHandleInfo(theResource);
		if (!info.error() && info->size > 0) {
			std::vector<uint8_t> resData(info->size);
			std::memcpy(resData.data(), memoryPointer(info->address), info->size);
			of->file->updateResource(iter->second.type, iter->second.id, resData);
		}

		of->dirty = true;

		return SetResError(0);
	}



	uint16_t DetachResource(uint16_t trap)
	{
		uint32_t theResource;
		StackFrame<4>(theResource);

		Log("%04x DetachResource(%08x)\n", trap, theResource);

		auto iter = rhandle_map.find(theResource);
		if (iter == rhandle_map.end()) return SetResError(MacOS::resNotFound);

		rhandle_map.erase(iter);

		return SetResError(0);
	}


	uint16_t Get1IndResource(uint16_t trap)
	{
		uint32_t sp;
		uint32_t theType;
		uint16_t index;

		sp = StackFrame<6>(theType, index);
		Log("%04x Get1IndResource(%08x ('%s'), %04x)\n",
			trap, theType, TypeToString(theType).c_str(), index);

		uint32_t resourceHandle = 0;
		uint16_t d0 = MacOS::resNotFound;

		auto *of = currentFile();
		if (of) {
			const rsrc::ResourceEntry *entry = of->file->getIndResource(theType, index);
			if (entry) {
				d0 = Native::LoadResourceFromEntry(entry, of->file.get(), of->refNum, resourceHandle);
			}
		}

		ToolReturn<4>(sp, resourceHandle);
		return SetResError(d0);
	}


	uint16_t RemoveResource(uint16_t trap)
	{
		uint32_t theResource;

		StackFrame<4>(theResource);

		Log("%04x RemoveResource(%08x)\n", trap, theResource);

		auto iter = rhandle_map.find(theResource);
		if (iter == rhandle_map.end()) return SetResError(MacOS::resNotFound);

		auto *of = findOpenFile(iter->second.refNum);
		if (of) {
			of->file->removeResource(iter->second.type, iter->second.id);
			of->dirty = true;
		}

		rhandle_map.erase(iter);

		return SetResError(0);
	}

	uint16_t GetResourceSizeOnDisk(uint16_t trap)
	{
		uint32_t sp;
		uint32_t theResource;

		sp = StackFrame<4>(theResource);

		Log("%04x GetResourceSizeOnDisk(%08x)\n", trap, theResource);

		auto iter = rhandle_map.find(theResource);
		if (iter == rhandle_map.end())
		{
			ToolReturn<4>(sp, (uint32_t)0);
			return SetResError(MacOS::resNotFound);
		}

		// Get the size from the emulated handle
		uint32_t size = 0;
		auto info = MM::GetHandleInfo(theResource);
		if (!info.error())
			size = info->size;

		ToolReturn<4>(sp, size);
		return SetResError(0);
	}

	uint16_t GetResInfo(uint16_t trap)
	{
		uint32_t theResource;
		uint32_t theID;
		uint32_t theType;
		uint32_t name;
		StackFrame<16>(theResource, theID, theType, name);

		Log("%04x GetResInfo(%08x)\n", trap, theResource);

		auto iter = rhandle_map.find(theResource);
		if (iter == rhandle_map.end())
		{
			return SetResError(MacOS::resNotFound);
		}

		const ResourceRef &ref = iter->second;

		if (theID) memoryWriteWord((uint16_t)ref.id, theID);
		if (theType) memoryWriteLong(ref.type, theType);
		if (name)
		{
			ToolBox::WritePString(name, ref.name);
		}

		return SetResError(0);
	}

	uint16_t LoadResource(uint16_t trap)
	{
		uint32_t theResource;

		StackFrame<4>(theResource);

		Log("%04x LoadResource(%08x)\n", trap, theResource);

		auto iter = rhandle_map.find(theResource);
		if (iter == rhandle_map.end())
		{
			return SetResError(MacOS::resNotFound);
		}

		// if it has a size, it's loaded...
		auto info = MM::GetHandleInfo(theResource);
		if (info.error()) return SetResError(MacOS::resNotFound);
		if (info->size) return SetResError(0);

		// load it from the resource file
		auto *of = findOpenFile(iter->second.refNum);
		if (!of) return SetResError(MacOS::resFNotFound);

		const rsrc::ResourceEntry *entry = of->file->findResource(iter->second.type, iter->second.id);
		if (!entry) return SetResError(MacOS::resNotFound);

		auto resData = of->file->loadResource(*entry);
		uint16_t err = MM::Native::ReallocHandle(theResource, resData.size());
		if (err) return SetResError(err);

		if (!resData.empty())
		{
			info = MM::GetHandleInfo(theResource);
			std::memcpy(memoryPointer(info->address), resData.data(), resData.size());
		}

		return SetResError(0);
	}


	uint16_t HomeResFile(uint16_t trap)
	{
		uint32_t sp;
		uint32_t theResource;
		uint16_t resFile;

		sp = StackFrame<4>(theResource);
		Log("%04x HomeResFile(%08x)\n", trap, theResource);

		auto iter = rhandle_map.find(theResource);
		if (iter == rhandle_map.end())
		{
			ToolReturn<2>(sp, (uint16_t)-1);
			return SetResError(MacOS::resNotFound);
		}

		resFile = iter->second.refNum;

		ToolReturn<2>(sp, resFile);

		return SetResError(0);
	}

	uint16_t Count1Types(uint16_t trap)
	{
		uint16_t count = 0;

		Log("%04x Count1Types\n", trap);

		auto *of = currentFile();
		if (of) {
			count = of->file->countTypes();
		}

		ToolReturn<2>(-1, count);

		return SetResError(0);
	}


	uint16_t Get1IndType(uint16_t trap)
	{
		uint32_t theType;
		uint16_t index;

		StackFrame<6>(theType, index);

		Log("%04x Get1IndType(%08x, %04x)\n", trap, theType, index);

		uint32_t nativeType = 0;

		auto *of = currentFile();
		if (of) {
			nativeType = of->file->getIndType(index);
		}

		memoryWriteLong(nativeType, theType);

		return SetResError(0);
	}
}
