#include "input/hotkeys.h"

#include "config/settings.h"
#include "engine/ui.h"

#include <Windows.h>

namespace sof::input {

	namespace {

		// Thread: Main
		bool g_reloadKeyWasDown = false;

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

	}

	// Polls the hotkeys and runs the action of each key that went down. Only while the game window has focus.
	//
	// Thread: Main (each frame)
	void PollHotkeys() {
		PollReloadKey();
	}

}
