#include "engine/grass.h"

#include "engine/addresses.h"
#include "utils/memory.h"

#include <Windows.h>

namespace sof::engine {

	using mem::Field;

	namespace {

		// Limits for the walk, far above any loaded grass.
		constexpr int kMaxDepth = 16;
		constexpr std::size_t kMaxObjects = 65536;

		// Returns the given object as a node, or null.
		// An exception in the virtual call counts as no node.
		void* AsNode(void* niObject) {
			using IsNodeFn = void*(__thiscall*)(void* object);

			__try {
				void* vtable = Field<void*>(niObject, 0);
				const auto isNode = Field<IsNodeFn>(vtable, kNiObjectVtbl_IsNode);

				return isNode(niObject);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		// Adds the given object to the given list if it is grass geometry with a grass shader property.
		void AddIfGrass(void* niObject, std::vector<GrassGeometry>& geometries) {
			std::uint32_t vtable = 0;

			if (!mem::SafeRead32(niObject, vtable)) {
				return;
			}

			const bool isStrips = vtable == kVtbl_TallGrassTriStrips;

			if (!isStrips && vtable != kVtbl_TallGrassTriShape) {
				return;
			}

			std::uint32_t property = 0;
			std::uint32_t shaderType = 0;

			if (!mem::SafeRead32(&Field<std::uint8_t>(niObject, kGeometry_ShaderProperty), property) || property == 0) {
				return;
			}

			void* propertyObject = reinterpret_cast<void*>(property);

			if (!mem::SafeRead32(&Field<std::uint8_t>(propertyObject, kShaderProperty_Type), shaderType)
				|| shaderType != kShaderType_TallGrass) {
				return;
			}

			std::uint32_t record = 0;
			mem::SafeRead32(&Field<std::uint8_t>(propertyObject, kGrassProperty_Record), record);

			geometries.push_back({niObject, propertyObject, reinterpret_cast<void*>(record), isStrips});
		}

		void Walk(void* niObject, int depth, std::size_t& visited, std::vector<GrassGeometry>& geometries) {
			if (!niObject || depth > kMaxDepth || visited >= kMaxObjects) {
				return;
			}

			++visited;

			void* node = AsNode(niObject);

			if (!node) {
				AddIfGrass(niObject, geometries);

				return;
			}

			std::uint32_t children = 0;
			std::uint32_t childrenEndWord = 0;

			if (!mem::SafeRead32(&Field<std::uint8_t>(node, kNiNode_Children), children) || children == 0
				|| !mem::SafeRead32(&Field<std::uint8_t>(node, kNiNode_ChildrenEnd), childrenEndWord)) {
				return;
			}

			const std::uint16_t childrenEnd = static_cast<std::uint16_t>(childrenEndWord & 0xFFFF);

			for (std::uint16_t index = 0; index < childrenEnd; ++index) {
				std::uint32_t child = 0;

				if (!mem::SafeRead32(reinterpret_cast<void*>(children + index * 4u), child)) {
					return;
				}

				Walk(reinterpret_cast<void*>(child), depth + 1, visited, geometries);
			}
		}

	}

	void* GetGrassRoot() {
		return mem::Global<void*>(kGrassRootNode);
	}

	// Returns every grass geometry under the grass root node.
	// Grass geometry is recognized by its vtable and a shader property of type `TallGrass`.
	//
	// Thread: Main
	std::vector<GrassGeometry> CollectGrassGeometries() {
		std::vector<GrassGeometry> geometries;
		std::size_t visited = 0;

		Walk(GetGrassRoot(), 0, visited, geometries);

		return geometries;
	}

	std::uint16_t GetCapacity(void* record) {
		if (!record) {
			return 0;
		}

		return Field<std::uint16_t>(record, kGrassRecord_Capacity);
	}

	std::uint16_t GetInstanceCount(void* record) {
		if (!record) {
			return 0;
		}

		return Field<std::uint16_t>(record, kGrassRecord_InstanceCount);
	}

	GrassInstance* GetInstances(void* record) {
		if (!record) {
			return nullptr;
		}

		return Field<GrassInstance*>(record, kGrassRecord_Instances);
	}

	// Returns a given object space position in world space, with the world transform of a given scene graph object.
	// The world rotation is row major (Experimental Acoustics, camera forward).
	Vector3 ObjectToWorld(void* niObject, const Vector3& local) {
		const float* rotation = &Field<float>(niObject, kNiObject_WorldRotate);
		const Vector3 translation = Field<Vector3>(niObject, kNiObject_WorldTranslate);
		const float scale = Field<float>(niObject, kNiObject_WorldScale);

		const Vector3 rotated{
			rotation[0] * local.x + rotation[1] * local.y + rotation[2] * local.z,
			rotation[3] * local.x + rotation[4] * local.y + rotation[5] * local.z,
			rotation[6] * local.x + rotation[7] * local.y + rotation[8] * local.z,
		};

		return translation + rotated * scale;
	}

}
