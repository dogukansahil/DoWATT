// Copyright (c) 2026 Dogukan Sahil
// Modified for DoWATT v1.0 on 2026-10-10. See LICENSE and NVML-EXCEPTION.txt.
// Additional permission under GPL v3 section 7: linking with installed NVIDIA NVML
// is permitted as described in NVML-EXCEPTION.txt.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
struct EstimateSample { LONG wattsTenths; LONG uncertaintyTenths; BOOL ready; };
void EstimateInitialize();
EstimateSample EstimateRead();
void EstimateShutdown();
BOOL EstimateWriteReport(const wchar_t *path);
void EstimateRequestRefresh();
