# Copies the plugin DLL and PDB into the MO2 mod folder, and the default INIs if the folder has none.
# Skips everything if the mod folder does not exist, so a build never creates a new MO2 mod.

# usage: cmake -DMOD_DIR=<mod folder> -DDLL=<dll> -DPDB=<pdb> -DINI=<default ini> -DFORMS=<default forms ini> -P deploy.cmake

if(NOT IS_DIRECTORY "${MOD_DIR}")
	message(STATUS "Mod folder '${MOD_DIR}' not found, skipped deploy.")
	return()
endif()

set(PLUGIN_DIR "${MOD_DIR}/nvse/plugins")
file(MAKE_DIRECTORY "${PLUGIN_DIR}")
file(COPY "${DLL}" "${PDB}" DESTINATION "${PLUGIN_DIR}")

foreach(DEFAULT_INI IN ITEMS "${INI}" "${FORMS}")
	get_filename_component(INI_NAME "${DEFAULT_INI}" NAME)

	if(NOT EXISTS "${PLUGIN_DIR}/${INI_NAME}")
		file(COPY "${DEFAULT_INI}" DESTINATION "${PLUGIN_DIR}")
		message(STATUS "Deployed default INI to ${PLUGIN_DIR}/${INI_NAME}")
	endif()
endforeach()
