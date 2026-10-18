// xNVSE plugin entry points for Occluded Foliage.

#include "nvse/nvse_minimal.h"

#include "config/settings.h"
#include "debug/physics_probe.h"
#include "input/hotkeys.h"
#include "occlusion/builder.h"
#include "utils/log.h"
#include "utils/threads.h"

#include <cstdint>
#include <string>

namespace {

	constexpr const char* kPluginName = "SaintsOccludedFoliage";
	constexpr std::uint32_t kPluginVersion = 1;

	// Handles NVSE messages. Runs per-frame work on each main loop message.
	void OnMessage(NVSEMessagingInterface::Message* message) {
		switch (message->type) {
		case NVSEMessagingInterface::kMessage_MainGameLoop:
			sof::threads::RememberMainThread();
			sof::debug::UpdatePhysicsProbe();
			sof::input::PollHotkeys();

			if (sof::config::Get().general.enabled) {
				sof::occlusion::UpdateBuilder();
			}

			break;

		case NVSEMessagingInterface::kMessage_PostLoadGame:
			SOF_LOG("Game loaded.");
			sof::occlusion::ResetBuilder();
			break;

		case NVSEMessagingInterface::kMessage_ExitToMainMenu:
			// Probably never happens in a typical modern NV game.
			SOF_LOG("Exited to main menu.");
			sof::occlusion::ResetBuilder();
			break;

		default:
			break;
		}
	}

}

extern "C" {

// Accepts only the game (not the editor) at runtime 1.4.0.525.
__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = kPluginName;
	info->version = kPluginVersion;

	if (nvse->isEditor) {
		return false;
	}

	return nvse->runtimeVersion == RUNTIME_VERSION_1_4_0_525;
}

// Opens the log, reads the INI and registers for messages.
__declspec(dllexport) bool NVSEPlugin_Load(NVSEInterface* nvse) {
	sof::threads::RememberMainThread();

	const PluginHandle pluginHandle = nvse->GetPluginHandle();
	const std::string runtimeDirectory = nvse->GetRuntimeDirectory();

	sof::log::Open((runtimeDirectory + "OccludedFoliage.log").c_str());
	SOF_LOG("Loading %s v%u (xNVSE %08X, runtime %08X).", kPluginName, kPluginVersion, nvse->nvseVersion, nvse->runtimeVersion);

	sof::config::Load(runtimeDirectory + "Data\\NVSE\\Plugins\\OccludedFoliage.ini");

	auto* messaging = static_cast<NVSEMessagingInterface*>(nvse->QueryInterface(kInterface_Messaging));

	if (!messaging || messaging->version < 4 || !messaging->RegisterListener(pluginHandle, "NVSE", OnMessage)) {
		SOF_LOG("Error: Messaging interface is unavailable or unusable.");

		return false;
	}

	sof::occlusion::InitializeBuilder();

	if (!sof::debug::InstallPhysicsProbe()) {
		SOF_LOG("Error: Could not install Havok probing module.");
	}

	SOF_LOG("Plugin loaded.");

	return true;
}

}
