cmake_minimum_required(VERSION 3.25)
project(OccludedFoliage LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(FATAL_ERROR "FalloutNV is 32-bit: configure with -A Win32")
endif()

set(SOF_DEPLOY_DIR "F:/Games/Tale of Two Wastelands/mods/Saint's Occluded Foliage"
	CACHE PATH "MO2 mod folder to deploy into (empty = no deploy)")

add_library(OccludedFoliage SHARED
	source/main.cpp

	source/config/settings.cpp

	source/engine/ui.cpp

	source/input/hotkeys.cpp

	source/utils/log.cpp
	source/utils/threads.cpp
)

target_include_directories(OccludedFoliage PRIVATE include source)
target_compile_features(OccludedFoliage PRIVATE cxx_std_20)
target_compile_definitions(OccludedFoliage PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(OccludedFoliage PRIVATE /W4 /permissive- /utf-8 /Zi)
target_link_options(OccludedFoliage PRIVATE /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF)

# Static CRT, so users do not need the VC++ redistributable.
set_property(TARGET OccludedFoliage PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(SOF_DEPLOY_DIR)
	add_custom_command(TARGET OccludedFoliage POST_BUILD
		COMMAND ${CMAKE_COMMAND}
			"-DMOD_DIR=${SOF_DEPLOY_DIR}"
			"-DDLL=$<TARGET_FILE:OccludedFoliage>"
			"-DPDB=$<TARGET_PDB_FILE:OccludedFoliage>"
			"-DINI=${CMAKE_CURRENT_SOURCE_DIR}/data/OccludedFoliage.ini"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deploy.cmake"
		COMMENT "Deploying to ${SOF_DEPLOY_DIR}")
endif()
