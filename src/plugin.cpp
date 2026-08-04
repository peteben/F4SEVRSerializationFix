#include <Windows.h>
#include <TlHelp32.h>
#include <stdio.h>

#define DLLEXPORT __declspec(dllexport)

void init_log() {
	std::optional<std::filesystem::path> logpath = logger::log_directory();

	const char* plugin_name = "F4SEVRserializationFix";
	*logpath /= fmt::format(FMT_STRING("{}.log"), plugin_name);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logpath->string(), true);

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

	log->set_level(spdlog::level::trace);
	log->flush_on(spdlog::level::trace);

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("[%T.%e] [%=5t] [%L] %v"s);
	}


/*
	This module exists to address some of the deficiencies in F4SEVR serialization.
	- The serialization of the modlist is outdated in F4SEVR compared to F4SE,
		so plugins cannot relocate saved formIds or handles. The workaround involves patching the F4SEVR dll's load routines
		to recognize the newer format.

*/


std::map<uint32_t, uint32_t> modMap;		// Map old mod indexes to new mod indices, for both regular and light mods
static uint8_t* F4SEbaseAddr;				// Base addr of the F4SE dll
const uint64_t o_switchDefault = 0x18e81;	// Offsets for the various routines we need to patch
const uint64_t o_savePluginList = 0x18c4d;
const uint64_t o_ResolveFormID = 0x61e50;
const uint64_t o_ResolveHandle = 0x61ef0;

const F4SE::SerializationInterface* g_serialization;
const F4SE::MessagingInterface* g_messaging;


// Get the load address for main F4SEVR dll
uint8_t* getF4SEbaseAddr() {
	DWORD procID = GetCurrentProcessId();
	HANDLE	snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, procID);
	uint8_t* ret = nullptr;

	if (snap != INVALID_HANDLE_VALUE) {
		MODULEENTRY32	module;
		bool modIsValid;

		module.dwSize = sizeof(module);

		for (modIsValid = Module32First(snap, &module); modIsValid; modIsValid = Module32Next(snap, &module)) {
			if (wcscmp(module.szModule, L"f4sevr_1_2_72.dll") == 0) {		// Get base address for the DLL
				ret = module.modBaseAddr;
				logger::info("f4sevr_1_2_72.dll base: {:p}", (void*)ret);
				break;
				}
			}

		CloseHandle(snap);
		}
	return ret;
	}


/* Hook:
	Replacement for F4SEVR's TESDataHandler::ResolveFormID, which doesn't deal with light mods.
	Changes the mod index of a formID (regular or light) to the new index, as calculated by the game.
*/

bool ResolveFormId(uint32_t formId, uint32_t* formIdOut) {
	uint8_t	modID = formId >> 24;
	uint32_t newID;
	std::map<uint32_t, uint32_t>::iterator search;

	//logger::info("ResolveFormID: %08X", formId);

	if (modID == 0xFF || formId == 0) {
		*formIdOut = formId;
		return true;
		}
	else if (modID == 0xFE) {
		uint16_t	modLightID = (formId & 0x00FFF000) >> 12;
		search = modMap.find(modLightID + 256);
		}
	else {
		search = modMap.find(modID);
		}

	if (search != modMap.end()) {
		newID = search->second;
		if (newID > 255) {
			*formIdOut = (formId & 0xFFF) | 0xFE000000 | ((newID - 256) << 12);
			}
		else {
			*formIdOut = (newID << 24) | (formId & (modID == 0xFE ? 0x0FFF : 0x00FFFFFF));
			}
		//logger::info("ResolveFormID: %08X %08X", formId, *formIdOut);
		return true;
		}
	else {
		return false;
		}
	}

bool ResolveHandle(uint64_t handle, uint64_t* handleOut) {
	uint8_t	modID = handle >> 24;
	std::map<uint32_t, uint32_t>::iterator search;

	//logger::info("ResolveHandle: %08X", handle);
	if (modID == 0xFF) {
		*handleOut = handle;
		return true;
		}
	else if (modID == 0xFE) {
		uint16_t	modLightID = (handle & 0x00FFF000) >> 12;
		search = modMap.find(modLightID + 256);
		}
	else {
		search = modMap.find(modID);
		}

	if (search != modMap.end()) {
		uint32_t newID = search->second;
		if (newID > 255) {
			*handleOut = (handle & 0xFFFFFFFF00000FFF) | 0xFE000000 | ((newID - 256) << 12);
			}
		else {																// A light mod keeps its object ID in the low 12 bits, so mask off the old light index
			*handleOut = (handle & (modID == 0xFE ? 0xFFFFFFFF00000FFFull : 0xFFFFFFFF00FFFFFFull)) | (newID << 24);
			}
		//logger::info("ResolveHandle: %08X %08X", handle, *handleOut);
		return true;
		}
	else {
		return false;
		}
	}

static const char* formatLight(uint16_t lightModIndex) {
	static char buffer[6];

	if (lightModIndex == 0xFFFF) {
		strcpy(buffer, "  ");
		}
	else {
		sprintf(buffer, "%03X", lightModIndex);
		}
	return buffer;
	}

void SavePluginList(const F4SE::SerializationInterface* intfc) {
	RE::TESDataHandler* dhand = RE::TESDataHandler::GetSingleton();
	uint16_t modCount = 0;

	for (auto& modInfo : dhand->files) {
		if (modInfo != nullptr && modInfo->IsActive()) {
			modCount++;
			}
		}

	logger::info("Saving {} mods", modCount);

	intfc->OpenRecord('PLGN', 0);
	intfc->WriteRecordData(&modCount, sizeof(modCount));

	for (auto& modInfo : dhand->files) {
		if (modInfo != nullptr && modInfo->IsActive()) {
			intfc->WriteRecordData(&modInfo->compileIndex, sizeof(modInfo->compileIndex));
			if (modInfo->compileIndex == 0xFE) {
				intfc->WriteRecordData(&modInfo->smallFileCompileIndex, sizeof(modInfo->smallFileCompileIndex));
				}

			uint16_t nameLen = strlen(modInfo->filename);
			intfc->WriteRecordData(&nameLen, sizeof(nameLen));
			intfc->WriteRecordData(modInfo->filename, nameLen);
			if (modInfo->compileIndex != 0xFE) {
				logger::info("\t[{}]     \t\t{}", modInfo->compileIndex, modInfo->filename);
				}
			else {
				logger::info("\t[FE:{}]\t\t\t{}", modInfo->smallFileCompileIndex, modInfo->filename);
				}
			}
		}
	}

// Recent versions of F4SE store the mod list in a new format, which F4SEVR cannot read
// So the module list never gets read, leading to plugins that don't work
// F4SE can read the old modlist from F4SEVR, though, so roundtripping is possible

void LoadPluginList(const F4SE::SerializationInterface* intfc) {
	RE::TESDataHandler* dhand = RE::TESDataHandler::GetSingleton();

	char name[0x104] = { 0 };
	uint16_t nameLen = 0;

	uint16_t modCount = 0;
	intfc->ReadRecordData(&modCount, sizeof(modCount));		// Number of mods in the save file
	logger::info("Loading plugin list: {} mods", modCount);

	for (uint32_t i = 0; i < modCount; i++) {
		uint8_t modIndex = 0xFF;
		uint16_t lightModIndex = 0xFFFF;

		intfc->ReadRecordData(&modIndex, sizeof(modIndex));					//  Get Index of mod when it was saved
		if (modIndex == 0xFE) {
			intfc->ReadRecordData(&lightModIndex, sizeof(lightModIndex));
			//logger::info("lightmod index %04X", lightModIndex);
			}

		intfc->ReadRecordData(&nameLen, sizeof(nameLen));
		//logger::info("Namelen %d", nameLen);
		if (nameLen >= sizeof(name)) {										// Garbage length: the rest of the record can't be trusted, so stop here
			logger::error("Bad plugin name length {} at entry {}, abandoning plugin list", nameLen, i);
			return;
			}
		intfc->ReadRecordData(&name, nameLen);
		name[nameLen] = 0;

		if (modIndex == 0xff) {												// Wasn't loaded when the game was saved; nothing to map,
			continue;														// but the name had to be consumed to stay in sync with the record
			}

		const RE::TESFile* minfo = dhand->LookupModByName(name);

		//logger::info("%s: minfo %p, ModIndex:lightModIndex %02X:%04X", name, minfo, modIndex, lightModIndex);
		if (minfo == nullptr) {
			logger::info("minfo is NULL for {}", name);
			}
		else {
			uint32_t newIndex = minfo->IsLight() ? minfo->smallFileCompileIndex + 256 : minfo->compileIndex;
			std::string lightModIndexstr(formatLight(lightModIndex));

			logger::info("{:<60} {:02X} {:>3}  -> {:02X} {:>3}", name, modIndex, lightModIndexstr,
				minfo->IsLight() ? 0xFE : minfo->compileIndex, formatLight(minfo->IsLight() ? minfo->smallFileCompileIndex : 0xFFFF));

			if (lightModIndex != 0xffff) {
				modMap.insert({ lightModIndex + 256, newIndex });
				}
			else {
				modMap.insert({ modIndex, newIndex });
				}
			}
		}
	logger::info("Done reading plugins");
	}

// Redirect here from the 'default' case of the Core_LoadCallback switch, 
// so we can handle the otherwise unknown plugin record type

void switchDefault_hook(const F4SE::SerializationInterface* intfc, uint32_t type) {
	if (type == 'PLGN') {
		LoadPluginList(intfc);
		}
	else {
		logger::info("Unhandled chunk type in Core_LoadCallback: {:08X} ({:4s})", type, (char*)&type);
		}
	}

uint8_t* savepatchloc;

void patchSerialization() {
#pragma pack(push, 1)

	// Need to patch the 'default' clause in the F4SE CoreLoadCallback switch statement,
	// as the 'PLGN' type record used by newer version of F4SE is not handled by F4SEVR
	// So we just patch the code to call our own routine
	// This requires a 64bit displacement, hence custom code here

	logger::info("F4SEVR base addr: {:p}", (void*)F4SEbaseAddr);
	logger::info("PatchSerialization");

	struct {
		uint8_t movcxbx[3];
		uint8_t nop[2];
		uint8_t movaxval[2];
		uint8_t* addr;
		uint8_t callax[2];
		} opcodes;

	opcodes.movcxbx[0] = 0x48;				// Move intfc from RBX into RCX
	opcodes.movcxbx[1] = 0x89;
	opcodes.movcxbx[2] = 0xd9;
	opcodes.nop[0] = 0x90;					// Just a pair of NOPs, makes the address fall on an 8-byte boundary
	opcodes.nop[1] = 0x90;
	opcodes.movaxval[0] = 0x48;				// MOV RAX, 'address of hook'
	opcodes.movaxval[1] = 0xb8;
	opcodes.addr = (uint8_t*)switchDefault_hook;
	opcodes.callax[0] = 0xff;				// CALL RAX
	opcodes.callax[1] = 0xd0;

	REL::safe_write((uintptr_t)(F4SEbaseAddr + o_switchDefault), &opcodes, sizeof(opcodes));

	// Now we patch the ResolveFormID and ResolveHandle routines, which don't handle light mods properly

	struct {
		uint8_t nop[6];
		uint8_t movaxval[2];
		uint8_t* addr;
		uint8_t jmpax[2];
		} jmpcodes;

	memset(jmpcodes.nop, 0x90, sizeof(jmpcodes.nop));
	jmpcodes.movaxval[0] = 0x48;				// MOV AX, 'address of hook'
	jmpcodes.movaxval[1] = 0xb8;
	jmpcodes.addr = (uint8_t*)ResolveFormId;
	jmpcodes.jmpax[0] = 0xff;					// JMP RAX
	jmpcodes.jmpax[1] = 0xe0;

	logger::info("Patching ResolveFormID at {:08X}", o_ResolveFormID);

	REL::safe_write((uintptr_t)(o_ResolveFormID + F4SEbaseAddr), &jmpcodes, sizeof(jmpcodes));
	jmpcodes.addr = (uint8_t*)ResolveHandle;
	logger::info("Patching ResolveHandle at {:08X}", o_ResolveHandle);
	REL::safe_write((uintptr_t)(o_ResolveHandle + F4SEbaseAddr), &jmpcodes, sizeof(jmpcodes));

	struct {
		uint8_t nop;					// 18C4D
		uint8_t movaxval[2];			// 18C4E
		uint8_t* addr;					// 18C50
		uint8_t callrax[2];				// 18C58
		uint8_t jmp20[2];				// 18C5A
		} savecodes;

	savecodes.nop = 0x90;
	savecodes.movaxval[0] = 0x48;				// MOV RAX, 'address of hook'
	savecodes.movaxval[1] = 0xb8;
	savecodes.addr = (uint8_t*)SavePluginList;
	savecodes.callrax[0] = 0xff;				// CALL RAX
	savecodes.callrax[1] = 0xd0;
	savecodes.jmp20[0] = 0xeb;					// JMP to the next useful instruction after the call, which is 0x20 bytes away
	savecodes.jmp20[1] = 0x20;

	logger::info("Patching SavePluginList at {:08X}", o_savePluginList);
	savepatchloc = o_savePluginList + F4SEbaseAddr;
	REL::safe_write((uintptr_t)savepatchloc, &savecodes, sizeof(savecodes));

#pragma pack( pop)
	}

void pre_loadGame() {
	logger::info("pre_loadGame: clearing modMap");
	modMap.clear();
	}

bool bytesOk = true;

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info) {
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = "F4SEVRserializationFix";
	a_info->version = 1;

	F4SEbaseAddr = getF4SEbaseAddr();
	if (F4SEbaseAddr == nullptr) {
		logger::error("Could not find F4SEVR base address");
		return false;
		}

	savepatchloc = F4SEbaseAddr + o_savePluginList;
	const uint8_t existingBytes[5] = { 0xe8, 0x2e, 0xfa, 0xff, 0xff };	// The bytes we expect to find at the save patch location
	bytesOk = memcmp(savepatchloc, existingBytes, sizeof(existingBytes)) == 0;
	return true;		// Don't load if already patched.
	}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se) {
	F4SE::Init(a_f4se);

	init_log();

	logger::info("F4SEVRserializationFix loaded");
	logger::info("BytesOk: {}", bytesOk);
	if (!bytesOk) {
		logger::error("F4SEVRserializationFix: SavePluginList patch location unexpected bytes {:02X}:{:02X}:{:02X}:{:02X}",
			savepatchloc[0], savepatchloc[1], savepatchloc[2], savepatchloc[3]);
		}

	g_serialization = F4SE::GetSerializationInterface();
	g_messaging = F4SE::GetMessagingInterface();

	g_messaging->RegisterListener([](F4SE::MessagingInterface::Message* msg) {
		if (msg->type == F4SE::MessagingInterface::kPreLoadGame
			|| msg->type == F4SE::MessagingInterface::kNewGame) {
			pre_loadGame();
			}
		});

	patchSerialization();

	return true;
	}