// Copyright (c) 2026 Dogukan Sahil
// Modified for DoWATT v1.0 on 2026-10-10. See LICENSE and NVML-EXCEPTION.txt.
// Additional permission under GPL v3 section 7: linking with installed NVIDIA NVML
// is permitted as described in NVML-EXCEPTION.txt.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>

struct CpuPowerSample {
    BOOL measured;
    LONG wattsTenths;
    LONG cpuLimitTenths; // Zero means no hardware power limit was obtained.
    BOOL dramMeasured;
    LONG dramWattsTenths; // Separate DRAM domain, never included in wattsTenths.
};

// Call all functions on the sampling worker. No drivers are installed.
void CpuPowerInitialize();
CpuPowerSample CpuPowerRead();
void CpuPowerShutdown();
const wchar_t* CpuPowerSource();
