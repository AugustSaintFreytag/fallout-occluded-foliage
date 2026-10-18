#include "engine/havok.h"

#include "engine/addresses.h"
#include "engine/objects.h"
#include "engine/scene.h"
#include "utils/log.h"
#include "utils/memory.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <xmmintrin.h>

namespace sof::engine {

	using mem::Field;

	namespace {

		// Pick data for a Havok ray (like `bhkPickData`).
		// Unnamed fields are set as JIP's `_GetRayCastObject` sets them.
		struct alignas(16) PickProperties {
			float from[4];  // 00, Havok units
			float to[4];  // 10, Havok units
			std::uint8_t byte20;  // 20
			std::uint8_t pad21[3];  // 21
			std::uint32_t filterInfo;  // 24: layer (byte), filter flags (byte), group (word)
			std::uint32_t unk28[6];  // 28
			float hitFraction;  // 40, 1 = no hit
			std::int32_t unk44[15];  // 44
			void* cdBody;  // 80, hkCdBody of the hit
			std::uint32_t unk84[3];  // 84
			float vector90[4];  // 90
			std::uint32_t unkA0[3];  // A0
			std::uint8_t byteAC;  // AC
			std::uint8_t padAD[3];  // AD
		};

		static_assert(sizeof(PickProperties) == 0xB0);
		static_assert(offsetof(PickProperties, filterInfo) == 0x24);
		static_assert(offsetof(PickProperties, hitFraction) == 0x40);
		static_assert(offsetof(PickProperties, cdBody) == 0x80);

		using PickObjectFn = void*(__thiscall*)(void* tes, PickProperties* pickData, bool unknown);
		using CellPickObjectFn = void*(__thiscall*)(void* cell, PickProperties* pickData);
		using GlobalLockFn = void(__cdecl*)();
		using MemoryRouterConstructorFn = void(__thiscall*)(void* router);
		using GetMemorySystemFn = void*(__cdecl*)();
		using ThreadInitFn = void(__thiscall*)(void* memorySystem, void* router, const char* name, int flags);
		using InitThreadFn = void(__cdecl*)(void* router);

		constexpr std::uint32_t kFilterGroupMask = 0xFFFF0000;
		constexpr int kMaxCdBodyDepth = 16;

		const char* const kLayerNames[] = {
			"None", "Static", "AnimStatic", "Transparent", "Clutter", "Weapon", "Projectile", "Spell", "Biped",
			"Trees", "Props", "Water", "Trigger", "Terrain", "Trap", "NonCollidable", "CloudTrap", "Ground",
			"Portal", "DebrisSmall", "DebrisLarge", "AcousticSpace", "ActorZone", "ProjectileZone", "GasTrap",
			"ShellCasing", "TransparentSmall", "InvisibleWall", "TransparentSmallAnim", "DeadBip",
			"CharController", "AvoidBox", "CollisionBox", "CameraSphere", "DoorDetection", "CameraPick",
			"ItemPick", "LineOfSight", "PathPick", "CustomPick1", "CustomPick2", "SpellExplosion", "DroppingPick",
		};

		// Pick faults logged one by one after the plugin loads. Later faults are only counted.
		constexpr std::uint32_t kMaxLoggedPickFaults = 16;

		// Frame pointer chain entries logged per pick fault.
		constexpr int kLoggedCallers = 6;

		// Thread: Any
		std::atomic<bool> g_pickFailureLogged{false};
		std::atomic<std::uint32_t> g_pickFaults{0};

		// Copy of an engine exception, taken in the exception filter.
		struct PickFault {
			std::uint32_t code = 0;
			void* address = nullptr;
			std::uint32_t accessType = 0;  // 0 = read, 1 = write, 8 = execute
			std::uint32_t accessAddress = 0;
			bool hasAccess = false;
			std::uint32_t framePointer = 0;
		};

		// Copies the exception record and the frame pointer for the handler. Logging waits for the handler.
		int CapturePickFault(const EXCEPTION_POINTERS* exception, PickFault& fault) {
			const EXCEPTION_RECORD* record = exception->ExceptionRecord;

			fault.code = record->ExceptionCode;
			fault.address = record->ExceptionAddress;
			fault.hasAccess = record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2;

			if (fault.hasAccess) {
				fault.accessType = static_cast<std::uint32_t>(record->ExceptionInformation[0]);
				fault.accessAddress = static_cast<std::uint32_t>(record->ExceptionInformation[1]);
			}

			fault.framePointer = exception->ContextRecord->Ebp;

			return EXCEPTION_EXECUTE_HANDLER;
		}

		// Counts a pick fault and logs the first few with their return addresses along the frame pointer chain.
		// Frames of code built without frame pointers are missing from the chain.
		void RecordPickFault(const PickFault& fault) {
			const std::uint32_t count = g_pickFaults.fetch_add(1, std::memory_order_relaxed) + 1;

			if (count > kMaxLoggedPickFaults) {
				return;
			}

			char callers[kLoggedCallers * 10 + 1] = "";
			std::size_t length = 0;
			std::uint32_t frame = fault.framePointer;

			for (int depth = 0; depth < kLoggedCallers && frame != 0; ++depth) {
				std::uint32_t returnAddress = 0;
				std::uint32_t nextFrame = 0;

				if (!mem::SafeRead32(reinterpret_cast<void*>(frame + 4), returnAddress)
					|| !mem::SafeRead32(reinterpret_cast<void*>(frame), nextFrame)) {
					break;
				}

				length += std::snprintf(callers + length, sizeof(callers) - length, " %08X", returnAddress);

				if (nextFrame <= frame) {
					break;
				}

				frame = nextFrame;
			}

			if (fault.hasAccess) {
				SOF_LOG("[Havok] Pick fault %u: access violation at %p, %s %08X. Callers:%s", count, fault.address,
					fault.accessType == 1 ? "writing" : (fault.accessType == 8 ? "executing" : "reading"), fault.accessAddress,
					callers);
			} else {
				SOF_LOG("[Havok] Pick fault %u: exception %08X at %p. Callers:%s", count, fault.code, fault.address, callers);
			}
		}

		// Returns the collision group of the player's character controller. A ray with this group ignores the player.
		// Returns 0 if the chain is not available, for example before the player has loaded in.
		std::uint32_t PlayerCollisionGroup() {
			const std::uintptr_t chain[] = {
				kActor_BaseProcess,
				kProcess_CharController,
				kCharController_Unknown594,
				kHavokRef_Object,
			};

			std::uint32_t address = reinterpret_cast<std::uint32_t>(GetPlayer());

			for (const std::uintptr_t offset : chain) {
				if (address == 0 || !mem::SafeRead32(reinterpret_cast<void*>(address + offset), address)) {
					return 0;
				}
			}

			std::uint32_t filterInfo = 0;

			if (address == 0 || !mem::SafeRead32(reinterpret_cast<void*>(address + kWorldObject_FilterInfo), filterInfo)) {
				return 0;
			}

			return filterInfo & kFilterGroupMask;
		}

		// Runs an engine object pick with the given pick properties. Returns the object hit, or null.
		// Property `hitFraction` is less than 1 if the ray hit an object or geometry.
		// An exception in the engine counts as no hit, sets the given fault flag and is recorded.
		void* PickObject(PickProperties* pickProperties, bool& faulted) {
			void* tes = mem::Global<void*>(kTESSingleton);

			if (!tes) {
				return nullptr;
			}

			const auto pickObject = reinterpret_cast<PickObjectFn>(kTES_PickObject);

			PickFault fault;

			__try {
				return pickObject(tes, pickProperties, true);
			} __except (CapturePickFault(GetExceptionInformation(), fault)) {
				pickProperties->hitFraction = 1.0f;
				faulted = true;
			}

			RecordPickFault(fault);

			return nullptr;
		}

		// Runs a pick in the Havok world of a given cell, without TES state. Otherwise like `PickObject`.
		void* PickObjectInCell(void* cell, PickProperties* pickProperties, bool& faulted) {
			const auto pickObject = reinterpret_cast<CellPickObjectFn>(kCell_PickObject);

			PickFault fault;

			__try {
				return pickObject(cell, pickProperties);
			} __except (CapturePickFault(GetExceptionInformation(), fault)) {
				pickProperties->hitFraction = 1.0f;
				faulted = true;
			}

			RecordPickFault(fault);

			return nullptr;
		}

		// Checks if objects on a given layer move or get destroyed.
		// The scene graph of such objects may change while a worker reads it, so their hits are not followed.
		bool IsDynamicLayer(std::uint8_t layer) {
			switch (layer) {
			case kLayer_Clutter:
			case kLayer_Weapon:
			case kLayer_Projectile:
			case kLayer_Biped:
			case kLayer_Props:
			case kLayer_DebrisSmall:
			case kLayer_DebrisLarge:
			case kLayer_ShellCasing:
			case kLayer_TransparentSmall:
			case kLayer_TransparentSmallAnim:
			case kLayer_DeadBip:
			case kLayer_CharController:
				return true;

			default:
				return false;
			}
		}

		void FillPickProperties(PickProperties& pickProperties, const Vector3& from, const Vector3& to, std::uint32_t filterInfo) {
			std::memset(&pickProperties, 0, sizeof(pickProperties));

			pickProperties.from[0] = from.x * kHavokScale;
			pickProperties.from[1] = from.y * kHavokScale;
			pickProperties.from[2] = from.z * kHavokScale;
			pickProperties.to[0] = to.x * kHavokScale;
			pickProperties.to[1] = to.y * kHavokScale;
			pickProperties.to[2] = to.z * kHavokScale;

			pickProperties.hitFraction = 1.0f;
			pickProperties.unk44[0] = -1;  // +0x44
			pickProperties.unk44[3] = -1;  // +0x50

			pickProperties.filterInfo = filterInfo;
		}

		// Returns the collision layer of a hit object, read from the root of its `hkCdBody` parents.
		// Returns 0 if unreadable.
		std::uint8_t HitLayer(void* cdBody) {
			std::uint32_t body = reinterpret_cast<std::uint32_t>(cdBody);

			for (int depth = 0; body != 0 && depth < kMaxCdBodyDepth; ++depth) {
				std::uint32_t parent = 0;

				if (!mem::SafeRead32(reinterpret_cast<void*>(body + kCdBody_Parent), parent)) {
					return 0;
				}

				if (parent == 0) {
					std::uint32_t layerWord = 0;

					if (!mem::SafeRead32(reinterpret_cast<void*>(body + kRootCdBody_Layer), layerWord)) {
						return 0;
					}

					return static_cast<std::uint8_t>(layerWord & 0xFF);
				}

				body = parent;
			}

			return 0;
		}

	}

	// Returns the name of a collision layer for the log, or "?" for unknown values.
	const char* CollisionLayerName(std::uint8_t layer) {
		if (layer >= std::size(kLayerNames)) {
			return "?";
		}

		return kLayerNames[layer];
	}

	// Casts a ray between two points in game units and returns the nearest hit.
	// The ray ignores the player's own collision. Follows hits on every layer to their reference.
	//
	// Reference at "Acoustics/src/source/engine/havok.cpp", "CastRay".
	//
	// Thread: Main
	RaycastResult CastRay(const Vector3& from, const Vector3& to, std::uint8_t layer) {
		PickProperties pickProperties;
		FillPickProperties(pickProperties, from, to, PlayerCollisionGroup() | (layer & 0x7F));

		RaycastResult result;
		void* object = PickObject(&pickProperties, result.faulted);

		if (pickProperties.hitFraction >= 1.0f) {
			return result;
		}

		if (pickProperties.hitFraction < 0.0f && !g_pickFailureLogged.exchange(true)) {
			SOF_LOG("[Havok] PickObject returned a negative hit fraction (%f).", pickProperties.hitFraction);
		}

		result.hit = true;
		result.fraction = pickProperties.hitFraction;
		result.point = from + (to - from) * pickProperties.hitFraction;
		result.object = object;
		result.reference = GetParentReference(object);
		result.layer = HitLayer(pickProperties.cdBody);

		return result;
	}

	// Casts a ray in the Havok world of a given cell and returns the nearest hit. Reads no TES or player state.
	// The ray does not ignore the player. Hits on dynamic layers keep a null reference.
	// Off the main thread, the caller holds the global Havok lock.
	//
	// Thread: Any
	RaycastResult CastRayInCell(void* cell, const Vector3& from, const Vector3& to, std::uint8_t layer) {
		PickProperties pickProperties;
		FillPickProperties(pickProperties, from, to, layer & 0x7F);

		RaycastResult result;
		void* object = PickObjectInCell(cell, &pickProperties, result.faulted);

		if (pickProperties.hitFraction >= 1.0f) {
			return result;
		}

		result.hit = true;
		result.fraction = pickProperties.hitFraction;
		result.point = from + (to - from) * pickProperties.hitFraction;
		result.object = object;
		result.layer = HitLayer(pickProperties.cdBody);

		if (!IsDynamicLayer(result.layer)) {
			result.reference = GetParentReference(object);
		}

		return result;
	}

	// Returns the number of engine picks that raised an exception since load.
	//
	// Thread: Any
	std::uint32_t GetPickFaultCount() {
		return g_pickFaults.load(std::memory_order_relaxed);
	}

	// Takes the global Havok lock that `hkpWorld::lock` and the world step take. Recursive per thread.
	// Returns false if Havok has not created the lock yet.
	//
	// Thread: Any
	bool LockHavok() {
		if (!mem::Global<void*>(kHavokGlobalLock)) {
			return false;
		}

		reinterpret_cast<GlobalLockFn>(kHavok_EnterGlobalLock)();
		return true;
	}

	// Sets up the Havok memory router, stack area and monitor stream for the calling thread, like Bethesda's threads.
	// Picks and other Havok calls fault or write through a null stack on a thread without them.
	// Call once per thread, before any Havok call. The state lives until the thread ends.
	// Returns false if Havok has no memory system yet or the stack area could not be allocated.
	//
	// The stack area is followed by a reserved page. An overflow faults there instead of writing past the area.
	//
	// Thread: Any, except threads the game created
	bool InitializeHavokThread(const char* name) {
		// Router size taken from `threadInit`, padded
		alignas(16) thread_local std::uint8_t router[0x40] = {};
		static_assert(sizeof(router) >= kHavokMemoryRouterSize);

		void* memorySystem = reinterpret_cast<GetMemorySystemFn>(kHavok_GetMemorySystem)();

		if (!memorySystem) {
			return false;
		}

		SYSTEM_INFO system{};
		GetSystemInfo(&system);

		const std::size_t stackSize = kHavokThreadStackSize;
		auto* stack = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, stackSize + system.dwPageSize, MEM_RESERVE, PAGE_NOACCESS));

		if (!stack || !VirtualAlloc(stack, stackSize, MEM_COMMIT, PAGE_READWRITE)) {
			return false;
		}

		// Havok job threads run with flush-to-zero.
		_mm_setcsr(_mm_getcsr() | _MM_FLUSH_ZERO_ON);

		reinterpret_cast<MemoryRouterConstructorFn>(kHavok_MemoryRouterConstructor)(router);

		const auto threadInit = Field<ThreadInitFn>(Field<void*>(memorySystem, 0), kMemorySystemVtbl_ThreadInit);
		threadInit(memorySystem, router, name, kHavokThreadInitHeap);

		reinterpret_cast<InitThreadFn>(kHavok_InitThread)(router);

		void* heap = Field<void*>(router, kMemoryRouter_Heap);

		Field<void*>(router, kMemoryRouter_Temp) = heap;
		Field<void*>(router, kMemoryRouter_Solver) = heap;
		Field<void*>(router, kMemoryRouter_StackCursor) = stack;
		Field<void*>(router, kMemoryRouter_StackBase) = stack;
		Field<void*>(router, kMemoryRouter_StackEnd) = stack + stackSize;

		return heap != nullptr;
	}

	// Releases the global Havok lock taken by `LockHavok`.
	//
	// Thread: Any
	void UnlockHavok() {
		reinterpret_cast<GlobalLockFn>(kHavok_LeaveGlobalLock)();
	}

}
