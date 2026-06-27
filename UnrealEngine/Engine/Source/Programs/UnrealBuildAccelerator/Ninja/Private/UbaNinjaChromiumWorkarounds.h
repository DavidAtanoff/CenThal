// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaStringBuffer.h"

namespace uba
{
	// Per-edge workarounds for known issues when building Chromium through UbaNinja.
	//
	// Some edges must be forced to run locally:
	//   - A handful of files hit bugs in Wine when compiled on Windows remote helpers.
	//   - A few steps are heavy / transfer huge numbers of files and are simply faster locally.
	//   - v8_context_snapshot.bin needs glibc 2.38 but Epic's Horde farm runs AL2023.
	//
	// TODO: The list is matched against the edge description (e.g. "C++ color_data.cc").
	//       Longer term this should move to either:
	//         (a) a well-known edge variable in the .ninja file (e.g. `uba_local = 1`)
	//             that GN / the generator can set, so fixes live at source, or
	//         (b) a sidecar file next to build.ninja listing output paths to force-local.
	//       Until then, entries should be added / removed here as new Wine bugs surface
	//       and are fixed.
	inline bool ShouldForceRunLocally(const TString& description)
	{
		static const StringView forceRunLocally[] =
		{
			// Requires three bug fixes in Wine
			TCV("color_data.cc"),
			TCV("tracing_service_idl.h"),
			TCV("elevation_service_idl.h"),
			TCV("dxcetw.h"),
			TCV("updater_idl.h"),
			TCV("updater_internal_idl.h"),
			TCV("updater_legacy_idl.h"),
			TCV("at_rule_descriptors.cc"),
			TCV("css_value_keywords.cc"),
			TCV("css_property_names.cc"),
			TCV("acls.stamp"),

			// Heavy steps, don't schedule on slow machine
			TCV("eslint.config.mjs"),

			// Crazy much traffic, lots of files.. runs much faster locally
			TCV("generated_resources__strings_grit.d.stamp"),
			TCV("components_strings__strings_grit.d.stamp"),

			#if PLATFORM_LINUX
			TCV("v8_context_snapshot.bin"), // Requires glibc 2.38 and Epic's horde farm uses AWS AL2023
			#endif
		};

		for (StringView str : forceRunLocally)
			if (description == str.data)
				return true;
		return false;
	}
}
