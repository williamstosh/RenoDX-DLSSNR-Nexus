/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

// Shared by C++, translations and Windows version resources.
#define NR_NEXUS_NAME "NR Nexus"
#define NR_NEXUS_DESCRIPTION "DLSS Neural Rendering Add-on for RenoDX"
#define NR_NEXUS_DEBUG_NAME NR_NEXUS_NAME " (Debug)"

#if defined(RENODX_DLSS5_DEBUG)
#define NR_NEXUS_ADDON_NAME NR_NEXUS_DEBUG_NAME
#else
#define NR_NEXUS_ADDON_NAME NR_NEXUS_NAME
#endif
