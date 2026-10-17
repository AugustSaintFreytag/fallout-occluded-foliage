#pragma once
// Engine addresses and object layouts for FalloutNV.exe 1.4.0.525.
// Each value has a tag that shows its source, as in `Documentation/Engine Reference.md`:
//   EXE   = verified in the disassembly of the unpacked exe
//   SEA   = Saint's Experimental Acoustics, verified in game there
//   TSTG  = There Stands the Grass
//   JIP, JG, xNVSE = JIP LN NVSE, JohnnyGuitar NVSE, xNVSE

#include <cstdint>

namespace sof::engine {

	// Globals
	constexpr std::uintptr_t kPlayerSingleton = 0x11DEA3C;  // PlayerCharacter** (JIP)
	constexpr std::uintptr_t kTESSingleton = 0x11DEA10;  // TES* (JIP)
	constexpr std::uintptr_t kSceneGraphSingleton = 0x11DEB7C;  // SceneGraph* (JIP)

	// Functions
	constexpr std::uintptr_t kQueueUIMessage = 0x7052F0;  // Corner notification (JIP, xNVSE, SEA)

	// NiAVObject* __thiscall TES::PickObject(PickData*, bool). Nearest hit only. (JIP, SEA; EXE: ret 8)
	constexpr std::uintptr_t kTES_PickObject = 0x458440;

	// bool __thiscall TES::GetLandHeight(const NiPoint3*, float*). Reads x and y.
	// Writes a default height first, returns false if no loaded cell holds the position.
	// Calls `TESObjectLAND::GetLandHeight` (0x53F180) for the land of that cell. (JIP, JG; EXE: ret 8)
	constexpr std::uintptr_t kTES_GetLandHeight = 0x4572E0;

	// TESForm
	constexpr std::uintptr_t kForm_TypeID = 0x04;  // UInt8
	constexpr std::uintptr_t kForm_RefID = 0x0C;
	constexpr std::uintptr_t kFormVtbl_GetEditorID = 0x130;  // const char* (__thiscall*)() (JIP)

	constexpr std::uint8_t kFormType_TESObjectACTI = 0x15;
	constexpr std::uint8_t kFormType_BGSTerminal = 0x17;
	constexpr std::uint8_t kFormType_TESObjectCONT = 0x1B;
	constexpr std::uint8_t kFormType_TESObjectDOOR = 0x1C;
	constexpr std::uint8_t kFormType_TESObjectMISC = 0x1F;
	constexpr std::uint8_t kFormType_TESObjectSTAT = 0x20;
	constexpr std::uint8_t kFormType_BGSStaticCollection = 0x21;
	constexpr std::uint8_t kFormType_BGSMovableStatic = 0x22;
	constexpr std::uint8_t kFormType_TESObjectTREE = 0x25;
	constexpr std::uint8_t kFormType_TESFurniture = 0x27;
	constexpr std::uint8_t kFormType_TESNPC = 0x2A;
	constexpr std::uint8_t kFormType_TESCreature = 0x2B;
	constexpr std::uint8_t kFormType_TESObjectREFR = 0x3A;
	constexpr std::uint8_t kFormType_Character = 0x3B;
	constexpr std::uint8_t kFormType_Creature = 0x3C;

	// TESModel component in a base form. Offset by form type, from the top byte of JIP's `kBaseComponentOffsets` (× 4).
	constexpr std::uintptr_t kModelOffset_Static = 0x30;  // STAT, SCOL, MSTT, TREE
	constexpr std::uintptr_t kModelOffset_Activator = 0x3C;  // ACTI, TERM, DOOR, MISC, FURN
	constexpr std::uintptr_t kModelOffset_Container = 0x48;  // CONT
	constexpr std::uintptr_t kModel_PathData = 0x04;  // String.m_data, path relative to "meshes\" (JIP)

	// TESObjectREFR
	constexpr std::uintptr_t kRefr_BaseForm = 0x20;  // TESForm* (JIP)
	constexpr std::uintptr_t kRefr_Position = 0x30;  // NiVector3 (JIP)

	// Actor
	constexpr std::uintptr_t kActor_BaseProcess = 0x68;  // BaseProcess* (JIP)

	// The ray takes the collision group of the player, so it ignores the player.
	// Pointer chain: player +0x68 (BaseProcess) +0x138 (bhkCharacterController) +0x594 +0x08 -> hkpWorldObject,
	// collision filter info (UInt32: layer, flags, group) at +0x2C. (JIP, SEA)
	constexpr std::uintptr_t kProcess_CharController = 0x138;  // (JIP HighProcess::charCtrl)
	constexpr std::uintptr_t kCharController_Unknown594 = 0x594;  // (JIP, unnamed)
	constexpr std::uintptr_t kHavokRef_Object = 0x08;  // bhkRefObject -> hkReferencedObject (JIP)
	constexpr std::uintptr_t kWorldObject_FilterInfo = 0x2C;  // hkpWorldObject: layer +0x2C, flags +0x2D, group +0x2E (JIP)

	// Scene graph
	constexpr std::uintptr_t kSceneGraph_Camera = 0xAC;  // NiCamera* (JIP; NiNode is 0xAC bytes)
	constexpr std::uintptr_t kNiObject_Parent = 0x18;  // NiNode* (JIP NiAVObject)
	constexpr std::uintptr_t kNiObject_WorldRotate = 0x68;  // NiMatrix33, row major (JIP)
	constexpr std::uintptr_t kNiObject_WorldTranslate = 0x8C;  // NiVector3 (JIP)
	constexpr std::uintptr_t kVtbl_BSFadeNode = 0x10A8F90;  // (JIP)
	constexpr std::uintptr_t kFadeNode_Reference = 0xCC;  // TESObjectREFR*, can be null (JIP NiAVObject::GetParentRef)

	// Havok
	constexpr float kHavokScale = 0.1428571f;  // Game units -> Havok units (1/7) (JIP kUnitConv)
	constexpr std::uintptr_t kCdBody_Parent = 0x0C;  // hkCdBody*, null for the root collidable (JIP)
	constexpr std::uintptr_t kRootCdBody_Layer = 0x1C;  // Root hkCdBody is hkpWorldObject +0x10. Layer at hkpWorldObject +0x2C. (JIP)

}
