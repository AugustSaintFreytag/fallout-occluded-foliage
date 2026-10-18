#pragma once

#include "utils/vector3.h"

#include <cstdint>

namespace sof::engine {

	void* GetPlayer();
	void* GetParentCell(void* reference);

	const char* GetEditorID(void* form);
	void* GetFormByEditorID(const char* editorID);

	std::uint32_t GetFormID(void* form);
	std::uint8_t GetFormType(void* form);

	const char* FormTypeName(std::uint8_t formType);

	void* GetBaseForm(void* reference);
	Vector3 GetPosition(void* reference);

	const char* GetModelPath(void* baseForm);
	bool HasMarkerModel(void* baseForm);

}
