// Copyright (c) 2026 Dogukan Sahil
// Modified for DoWATT v1.0 on 2026-10-10. See LICENSE and NVML-EXCEPTION.txt.
// Additional permission under GPL v3 section 7: linking with installed NVIDIA NVML
// is permitted as described in NVML-EXCEPTION.txt.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
// Ambiguous Intel Arc names are classified by the driver's architecture flag,
// not by the brand name or the amount of memory reserved by the BIOS.
BOOL IntelGpuIsUma(const wchar_t *description, const wchar_t *pnp);
