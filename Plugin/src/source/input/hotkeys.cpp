#include "input/hotkeys.h"

#include "config/settings.h"
#include "debug/crosshair_probe.h"
#include "debug/grass_census.h"
#include "engine/ui.h"

#include <Windows.h>

namespace sof::input {

	namespace {

		// Thread: Main
		bool g_reloadKeyWasDown = false;
		bool g_probeKeyWasDown = false;
		bool g_grassCensusKeyWasDown = false;
		bool g_grassHideTestKeyWasDown = false;

		bool GameHasFocus() {
			DWORD foregroundProcessId = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcessId);

			return foregroundProcessId == GetCurrentProcessId();
		}

		// Checks if the given key went down since the last poll. Updates the given key state.
		bool WasPressed(int key, bool& keyWasDown) {
			const bool keyIsDown = (GetAsyncKeyState(key) & 0x8000) != 0 && GameHasFocus();
			const bool keyWentDown = keyIsDown && !keyWasDown;

			keyWasDown = keyIsDown;

			return keyWentDown;
		}

		// Reloads the INI on `[Debug] iReloadKey` and shows a notification.
		void PollReloadKey() {
			const int reloadKey = config::Get().debug.reloadKey;

			if (!reloadKey) {
				return;
			}

			if (!WasPressed(reloadKey, g_reloadKeyWasDown)) {
				return;
			}

			config::Reload();
			engine::ShowNotification("Occluded Foliage Settings: Reloaded");
		}

		// Probes the grid point under the crosshair on `[Debug] iProbeKey`.
		void PollProbeKey() {
			const int probeKey = config::Get().debug.probeKey;

			if (!probeKey) {
				return;
			}

			if (WasPressed(probeKey, g_probeKeyWasDown)) {
				debug::RunCrosshairProbe();
			}
		}

		// Logs the grass geometry on `[Debug] iGrassCensusKey`.
		void PollGrassCensusKey() {
			const int grassCensusKey = config::Get().debug.grassCensusKey;

			if (!grassCensusKey) {
				return;
			}

			if (WasPressed(grassCensusKey, g_grassCensusKeyWasDown)) {
				debug::RunGrassCensus();
			}
		}

		// Hides or restores the nearest grass geometry on `[Debug] iGrassHideTestKey`.
		void PollGrassHideTestKey() {
			const int grassHideTestKey = config::Get().debug.grassHideTestKey;

			if (!grassHideTestKey) {
				return;
			}

			if (WasPressed(grassHideTestKey, g_grassHideTestKeyWasDown)) {
				debug::ToggleGrassHideTest();
			}
		}

	}

	// Polls the hotkeys and runs the action of each key that went down. Only while the game window has focus.
	//
	// Thread: Main (each frame)
	void PollHotkeys() {
		PollReloadKey();
		PollProbeKey();
		PollGrassCensusKey();
		PollGrassHideTestKey();
	}

}
