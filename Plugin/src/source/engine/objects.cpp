#include "engine/objects.h"

#include "engine/addresses.h"
#include "utils/memory.h"

#include <cstring>

namespace sof::engine {

	using mem::Field;

	void* GetPlayer() {
		return mem::Global<void*>(kPlayerSingleton);
	}

	void* GetParentCell(void* reference) {
		if (!reference) {
			return nullptr;
		}

		return Field<void*>(reference, kRefr_ParentCell);
	}

	// Returns the editor ID of a given form, or an empty string.
	// IDs of most forms are not available without JohnnyGuitar NVSE.
	const char* GetEditorID(void* form) {
		if (!form) {
			return "";
		}

		using GetEditorIDFn = const char*(__thiscall*)(void* form);

		void* vtable = Field<void*>(form, 0);

		const auto getEditorID = Field<GetEditorIDFn>(vtable, kFormVtbl_GetEditorID);
		const char* editorID = getEditorID(form);

		if (!editorID) {
			return "";
		}

		return editorID;
	}

	// Returns the form with a given editor ID, or null.
	// Most forms are found only with JohnnyGuitar NVSE, which keeps their editor IDs.
	//
	// Thread: Main
	void* GetFormByEditorID(const char* editorID) {
		using GetFormByEditorIDFn = void*(__cdecl*)(const char* editorID);

		return reinterpret_cast<GetFormByEditorIDFn>(kForm_GetFormByEditorID)(editorID);
	}

	std::uint32_t GetFormID(void* form) {
		if (!form) {
			return 0;
		}

		return Field<std::uint32_t>(form, kForm_RefID);
	}

	// Returns the form type ID of a given form, 0 for null.
	std::uint8_t GetFormType(void* form) {
		if (!form) {
			return 0;
		}

		return Field<std::uint8_t>(form, kForm_TypeID);
	}

	// Returns the record name of a form type, such as "STAT".
	// Returns "?" for types the plugin does not know.
	const char* FormTypeName(std::uint8_t formType) {
		switch (formType) {
		case kFormType_TESObjectACTI:
			return "ACTI";

		case kFormType_BGSTerminal:
			return "TERM";

		case kFormType_TESObjectCONT:
			return "CONT";

		case kFormType_TESObjectDOOR:
			return "DOOR";

		case kFormType_TESObjectMISC:
			return "MISC";

		case kFormType_TESObjectSTAT:
			return "STAT";

		case kFormType_BGSStaticCollection:
			return "SCOL";

		case kFormType_BGSMovableStatic:
			return "MSTT";

		case kFormType_TESObjectTREE:
			return "TREE";

		case kFormType_TESFurniture:
			return "FURN";

		case kFormType_TESNPC:
			return "NPC_";

		case kFormType_TESCreature:
			return "CREA";

		case kFormType_TESObjectREFR:
			return "REFR";

		case kFormType_Character:
			return "ACHR";

		case kFormType_Creature:
			return "ACRE";

		default:
			return "?";
		}
	}

	void* GetBaseForm(void* reference) {
		if (!reference) {
			return nullptr;
		}

		return Field<void*>(reference, kRefr_BaseForm);
	}

	Vector3 GetPosition(void* reference) {
		if (!reference) {
			return {};
		}

		return Field<Vector3>(reference, kRefr_Position);
	}

	// Returns the model path of a given base form, relative to "meshes\".
	// Returns an empty string if the form type has no model or the path is empty.
	const char* GetModelPath(void* baseForm) {
		std::uintptr_t modelOffset = 0;

		switch (GetFormType(baseForm)) {
		case kFormType_TESObjectSTAT:
		case kFormType_BGSStaticCollection:
		case kFormType_BGSMovableStatic:
		case kFormType_TESObjectTREE:
			modelOffset = kModelOffset_Static;
			break;

		case kFormType_TESObjectACTI:
		case kFormType_BGSTerminal:
		case kFormType_TESObjectDOOR:
		case kFormType_TESObjectMISC:
		case kFormType_TESFurniture:
			modelOffset = kModelOffset_Activator;
			break;

		case kFormType_TESObjectCONT:
			modelOffset = kModelOffset_Container;
			break;

		default:
			return "";
		}

		const char* path = Field<const char*>(baseForm, modelOffset + kModel_PathData);

		if (!path) {
			return "";
		}

		return path;
	}

	// Checks if the model of a given base form lies in the `Markers` mesh folder.
	// Marker meshes are editor only and never load 3D in game.
	bool HasMarkerModel(void* baseForm) {
		const char* path = GetModelPath(baseForm);

		if (_strnicmp(path, "meshes\\", 7) == 0) {
			path += 7;
		}

		return _strnicmp(path, "markers\\", 8) == 0;
	}

}
