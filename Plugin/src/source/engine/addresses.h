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

	// TES
	constexpr std::uintptr_t kTES_GridCellArray = 0x08;  // GridCellArray* (JIP, TSTG)
	constexpr std::uintptr_t kGridCellArray_GridSize = 0x0C;  // UInt32, uGridsToLoad (JIP)
	constexpr std::uintptr_t kGridCellArray_Cells = 0x10;  // TESObjectCELL*[gridSize * gridSize] (JIP)

	// TESObjectCELL
	constexpr std::uintptr_t kCell_Flags = 0x24;  // UInt8, bit 0 = interior (JIP)
	constexpr std::uintptr_t kCell_LoadingStage = 0x26;  // UInt8, 6 = attached (JIP)
	constexpr std::uintptr_t kCell_ExteriorCoords = 0x48;  // CellCoord* {SInt32 x, y}, exteriors only (JIP)
	constexpr std::uintptr_t kCell_ObjectList = 0xAC;  // tList<TESObjectREFR>, first node inline (JIP)
	constexpr std::uintptr_t kCell_WorldSpace = 0xC0;  // TESWorldSpace*, null for interiors (JIP, TSTG)
	constexpr std::uint8_t kCellFlag_Interior = 0x01;
	constexpr std::uint8_t kCellStage_Attached = 6;

	// tList node
	constexpr std::uintptr_t kListNode_Data = 0x00;
	constexpr std::uintptr_t kListNode_Next = 0x04;

	// TESForm
	constexpr std::uintptr_t kForm_Flags = 0x08;  // UInt32 (JIP)
	constexpr std::uint32_t kFormFlag_Deleted = 0x20;  // (JIP kFlags_Deleted)
	constexpr std::uint32_t kFormFlag_Disabled = 0x800;  // (JIP GetDisabled tests byte +9 bit 3)

	// bool __cdecl(TESObjectREFR*). Each checks a global reference set under a lock (0x11CACF8). (JIP GetDisabled; EXE)
	// With the disabled flag: disabled unless in the first set (0x11CACA8), else the second set decides.
	// Without the flag: the second set (0x11CACE8) decides. Covers references disabled by an enable parent.
	constexpr std::uintptr_t kRefr_InFlaggedEnabledSet = 0x5AA680;
	constexpr std::uintptr_t kRefr_InDisabledSet = 0x5AA630;
	constexpr std::uintptr_t kForm_TypeID = 0x04;  // UInt8
	constexpr std::uintptr_t kForm_RefID = 0x0C;

	// Form IDs below are engine built-ins, such as `XMarker` (0x3B), `MapMarker` (0x10) and `CollisionMarker` (0x21). (RUN)
	constexpr std::uint32_t kPluginHeadFormID = 0x800;
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
	constexpr std::uintptr_t kRefr_ParentCell = 0x40;  // TESObjectCELL* (JIP)
	constexpr std::uintptr_t kRefr_RenderState = 0x64;  // RenderState*, null without loaded 3D (JIP)
	constexpr std::uintptr_t kRenderState_RootNode = 0x14;  // NiNode* (JIP)

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
	constexpr std::uintptr_t kNiObject_WorldBound = 0x20;  // NiBound* {NiPoint3 center, float radius} (JIP)
	constexpr std::uintptr_t kNiObject_WorldRotate = 0x68;  // NiMatrix33, row major (JIP)
	constexpr std::uintptr_t kNiObject_WorldTranslate = 0x8C;  // NiVector3 (JIP)
	constexpr std::uintptr_t kVtbl_BSFadeNode = 0x10A8F90;  // (JIP)
	constexpr std::uintptr_t kFadeNode_Reference = 0xCC;  // TESObjectREFR*, can be null (JIP NiAVObject::GetParentRef)

	// NiAVObject, NiNode
	// NiObject vtable: 0 destructor, 1 `DeleteThis` (0xCFCC20, shared), 2 `GetRTTI`, 3 `IsNode`. (EXE)
	constexpr std::uintptr_t kNiObjectVtbl_IsNode = 0x0C;  // NiNode* (__thiscall*)(), null for other objects (EXE)
	constexpr std::uintptr_t kNiObject_WorldScale = 0x98;  // float (JIP NiTransform after rotate and translate)
	constexpr std::uintptr_t kNiNode_Children = 0xA0;  // NiTArray<NiAVObject*>::data (JIP m_children at 0x9C)
	constexpr std::uintptr_t kNiNode_ChildrenEnd = 0xA6;  // UInt16, NiTArray::firstFreeEntry (JIP)

	// Grass
	// Root node of all grass geometry. The grass manager (0x57D0A0) creates it, attaches it and updates it. (EXE)
	constexpr std::uintptr_t kGrassRootNode = 0x11CA438;  // NiNode*

	constexpr std::uintptr_t kVtbl_TallGrassTriStrips = 0x10BCC90;  // GetRTTI 0xC1A3A0 returns NiRTTI 0x12024B8 (EXE)
	constexpr std::uintptr_t kVtbl_TallGrassTriShape = 0x10BCDC8;  // GetRTTI 0xC1A650 returns NiRTTI 0x12024C0 (EXE)

	constexpr std::uintptr_t kGeometry_ShaderProperty = 0xA8;  // BSShaderProperty*, NiPropertyState at +0x9C, shade at +0x0C (JIP, EXE)
	constexpr std::uintptr_t kShaderProperty_Type = 0x1C;  // UInt32, 4 = TallGrass (JIP)
	constexpr std::uintptr_t kShaderProperty_Flags2 = 0x24;  // UInt32, bit 11 = UNIFORM_SCALE for grass (JG, EXE: 0xBACB73)
	constexpr std::uint32_t kShaderType_TallGrass = 4;

	// TallGrassShaderProperty (0x98 bytes, EXE: 0xB60F50). Its clone copies the record pointer raw (EXE: 0xB60B04).
	constexpr std::uintptr_t kGrassProperty_Record = 0x7C;  // Instance record

	// Instance record, one per cell and grass type. Pooled: the game reinitializes a record for another cell (0xBD8B00).
	// `0xBAAF40` copies capacity × 16 bytes from the array into the shader constants on every draw.
	// With fewer instances than the capacity, the primitive count becomes instances × triangles per instance. (EXE)
	constexpr std::uintptr_t kGrassRecord_Capacity = 0x10;  // UInt16, 228 in game (RUN)
	constexpr std::uintptr_t kGrassRecord_InstanceCount = 0x12;  // UInt16, entries past it are stale (RUN)
	constexpr std::uintptr_t kGrassRecord_Instances = 0x14;  // float[4] per instance, world position in xyz (RUN), c20[a0.x] (TSTG)
	constexpr std::uintptr_t kGrassRecord_Keys = 0x18;  // void*[capacity], one key per instance (EXE)
	constexpr std::uintptr_t kGrassRecord_CellX = 0x3C;  // SInt32 (EXE: 0xBD8B77, RUN)
	constexpr std::uintptr_t kGrassRecord_CellY = 0x40;  // SInt32 (EXE: 0xBD8B7A, RUN)

	// bool __thiscall AddInstance(record, const float[4]* instance, void* key). Appends one instance, false when full.
	// Only call site: the scatter loop at 0xB62FC1, which seeds `rand` from the position of each grass point. (EXE: ret 8)
	constexpr std::uintptr_t kGrassRecord_AddInstance = 0xBD8550;
	constexpr std::uintptr_t kGrassAddInstanceCall = 0xB62FC1;

	// Havok
	// void __thiscall bhkWorld::Update(). Steps the Havok world under `hkpWorld::lock` (0xC91B10, 0xC91BD0). (JIP; EXE: ret)
	// Both vtables point to 0xC6AE70. (EXE)
	constexpr std::uintptr_t kVtbl_bhkWorld = 0x10C40B4;  // (JIP)
	constexpr std::uintptr_t kVtbl_bhkWorldM = 0x10C69F4;  // (JIP)
	constexpr std::uintptr_t kWorldVtbl_Update = 0xC4;

	constexpr float kHavokScale = 0.1428571f;  // Game units -> Havok units (1/7) (JIP kUnitConv)
	constexpr std::uintptr_t kCdBody_Parent = 0x0C;  // hkCdBody*, null for the root collidable (JIP)
	constexpr std::uintptr_t kRootCdBody_Layer = 0x1C;  // Root hkCdBody is hkpWorldObject +0x10. Layer at hkpWorldObject +0x2C. (JIP)

}
