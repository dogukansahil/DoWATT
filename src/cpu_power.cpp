// Copyright (c) 2026 Dogukan Sahil
// Modified for DoWATT v1.0 on 2026-10-10. See LICENSE and NVML-EXCEPTION.txt.
// Additional permission under GPL v3 section 7: linking with installed NVIDIA NVML
// is permitted as described in NVML-EXCEPTION.txt.
// SPDX-License-Identifier: GPL-3.0-only
#include "cpu_power.h"
#include <setupapi.h>

// Windows EMI wire ABI, independently expressed from Microsoft's public API
// documentation. Zig's Windows headers do not currently contain emi.h.
// https://learn.microsoft.com/windows-hardware/drivers/powermeter/energy-meter-interface
// https://learn.microsoft.com/windows/win32/api/emi/ns-emi-emi_channel_v2
// https://learn.microsoft.com/windows/win32/api/emi/ns-emi-emi_channel_measurement_data
namespace {
const GUID EnergyMeterGuid = {0x45bd8344, 0x7ed6, 0x49cf,
    {0xa4, 0x40, 0xc2, 0x76, 0xc9, 0x33, 0xb0, 0x53}};
const DWORD MaxPackages = 64;
const DWORD MaxChannels = 128;
const DWORD MaxMetadata = 65536;
struct EnergyReading { ULONGLONG energy; ULONGLONG time; };
struct Meter {
    HANDLE handle;
    DWORD channels;
    DWORD packageChannels[MaxPackages];
    DWORD dramChannels[MaxPackages];
    EnergyReading* readings;
};
struct Previous { EnergyReading value; ULONGLONG wall; BOOL valid; };
Meter meters[MaxPackages];
Previous previous[MaxPackages];
Previous previousDram[MaxPackages];
DWORD meterCount;
DWORD packageCount;
BOOL available;
BOOL dramAvailable;
BOOL dramMetadataGood;
BOOL dramSeen[MaxPackages];

DWORD Read32(const BYTE* p) {
    return (DWORD)p[0] | ((DWORD)p[1] << 8) |
        ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}
USHORT Read16(const BYTE* p) { return (USHORT)(p[0] | ((USHORT)p[1] << 8)); }
BOOL Query(HANDLE handle, DWORD function, void* output, DWORD size, DWORD* got) {
    return DeviceIoControl(handle,
        CTL_CODE(FILE_DEVICE_UNKNOWN, function, METHOD_BUFFERED, FILE_READ_ACCESS),
        0, 0, output, size, got, 0);
}

DWORD CountPackages() {
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorPackage, 0, &bytes);
    if (!bytes || bytes > 1024 * 1024) return 0;
    BYTE* data = (BYTE*)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!data) return 0;
    DWORD count = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorPackage,
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)data, &bytes)) {
        DWORD at = 0;
        while (at + sizeof(LOGICAL_PROCESSOR_RELATIONSHIP) + sizeof(DWORD) <= bytes) {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* info =
                (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(data + at);
            if (!info->Size || info->Size > bytes - at) { count = 0; break; }
            if (info->Relationship == RelationProcessorPackage) ++count;
            at += info->Size;
        }
        if (at != bytes) count = 0;
    }
    HeapFree(GetProcessHeap(), 0, data);
    return count <= MaxPackages ? count : 0;
}

BOOL Prefix(const wchar_t* name, const wchar_t* prefix, DWORD* length) {
    DWORD i = 0;
    while (prefix[i]) {
        wchar_t a = name[i], b = prefix[i];
        if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
        if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
        if (a != b) return FALSE;
        ++i;
    }
    *length = i;
    return TRUE;
}

// EMI provides names rather than a typed component identifier. Deliberately
// accept only explicit whole-package names, never CPU/Core/DRAM/rail labels.
// Multi-package IDs must form a unique contiguous zero-based set.
LONG RaplIndex(const wchar_t* name, const wchar_t* domain, BOOL microsoftPpm) {
    DWORD at = 0;
    // Microsoft PPM's installed EMI provider identifies Intel RAPL domains.
    // PKG is the whole package; PP0, PP1 and DRAM must never be added to it.
    if (microsoftPpm && Prefix(name, L"RAPL_Package", &at)) {
        DWORD value = 0;
        if (name[at] < L'0' || name[at] > L'9') return -1;
        while (name[at] >= L'0' && name[at] <= L'9') {
            value = value * 10 + (DWORD)(name[at++] - L'0');
            if (value >= MaxPackages) return -1;
        }
        return value < packageCount && lstrcmpiW(name + at, domain) == 0 ? (LONG)value : -1;
    }
    return -1;
}

LONG PackageIndex(const wchar_t* name, BOOL microsoftPpm) {
    LONG rapl = RaplIndex(name, L"_PKG", microsoftPpm);
    if (rapl >= 0) return rapl;
    DWORD at = 0;
    if (!Prefix(name, L"CPU Package", &at) &&
        !Prefix(name, L"Processor Package", &at)) return -1;
    if (!name[at]) return packageCount == 1 ? 0 : -1;
    if (name[at++] != L' ' || name[at] < L'0' || name[at] > L'9') return -1;
    DWORD value = 0;
    while (name[at] >= L'0' && name[at] <= L'9') {
        value = value * 10 + (DWORD)(name[at++] - L'0');
        if (value >= MaxPackages) return -1;
    }
    return !name[at] && value < packageCount ? (LONG)value : -1;
}

// Returns FALSE on malformed metadata or duplicate package providers. A valid
// device without an explicit package channel is ignored without an error.
BOOL AddMeter(HANDLE handle, BOOL* seen) {
    USHORT version = 0;
    DWORD got = 0, bytes = 0;
    if (!Query(handle, 0, &version, sizeof(version), &got) || got != sizeof(version) ||
        (version != 1 && version != 2)) return TRUE;
    DWORD minimumMetadata = version == 1 ? 74 : 70;
    if (!Query(handle, 1, &bytes, sizeof(bytes), &got) || got != sizeof(bytes) ||
        bytes < minimumMetadata || bytes > MaxMetadata) return TRUE;
    BYTE* metadata = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    if (!metadata) return FALSE;
    BOOL good = Query(handle, 2, metadata, bytes, &got) && got >= minimumMetadata && got <= bytes;
    DWORD found[MaxPackages];
    DWORD foundDram[MaxPackages];
    for (DWORD p = 0; p < MaxPackages; ++p) { found[p] = MAXDWORD; foundDram[p] = MAXDWORD; }
    DWORD channels = version == 1 ? 1 : Read16(metadata + 66);
    // Bound the fixed OEM/model strings before treating them as strings.
    DWORD oemAt = version == 1 ? 4 : 0;
    BOOL microsoftPpm = Read16(metadata + oemAt + 30) == 0 &&
        Read16(metadata + oemAt + 62) == 0 &&
        lstrcmpiW((const wchar_t*)(metadata + oemAt), L"Microsoft") == 0 &&
        lstrcmpiW((const wchar_t*)(metadata + oemAt + 32), L"PPM") == 0;
    if (!channels || channels > MaxChannels) good = FALSE;
    DWORD at = 68;
    BOOL any = FALSE;
    for (DWORD c = 0; good && c < channels; ++c) {
        DWORD unit = 0, nameBytes = 0, nameAt = 0;
        if (version == 1) {
            unit = Read32(metadata);
            nameBytes = Read16(metadata + 70);
            nameAt = 72;
        } else {
            if (at > got || got - at < 6) { good = FALSE; break; }
            unit = Read32(metadata + at);
            nameBytes = Read16(metadata + at + 4);
            nameAt = at + 6;
        }
        if (nameBytes < 2 || (nameBytes & 1) || nameAt > got ||
            nameBytes > got - nameAt || Read16(metadata + nameAt + nameBytes - 2)) {
            good = FALSE; break;
        }
        // Windows PPM reserves a padded name field. Permit trailing zero
        // padding, but reject text hidden after an embedded terminator.
        BOOL ended = FALSE;
        for (DWORD n = 0; n < nameBytes; n += 2) {
            USHORT character = Read16(metadata + nameAt + n);
            if (!character) ended = TRUE;
            else if (ended) good = FALSE;
        }
        if (!good) break;
        LONG p = PackageIndex((const wchar_t*)(metadata + nameAt), microsoftPpm);
        if (unit == 0 && p >= 0) {
            if (seen[p] || found[p] != MAXDWORD) { good = FALSE; break; }
            found[p] = c;
            any = TRUE;
        }
        // Intel SDM Vol.3B: the package domain is the processor die, while
        // the DRAM domain covers directly-attached DRAM. Only the explicit
        // Microsoft PPM RAPL DRAM domain is accepted for separate memory power.
        LONG dram = RaplIndex((const wchar_t*)(metadata + nameAt), L"_DRAM", microsoftPpm);
        if (unit == 0 && dram >= 0) {
            if (dramSeen[dram] || foundDram[dram] != MAXDWORD) dramMetadataGood = FALSE;
            foundDram[dram] = c;
            any = TRUE;
        }
        at = nameAt + nameBytes;
    }
    HeapFree(GetProcessHeap(), 0, metadata);
    if (!good) return FALSE;
    if (!any) return TRUE;
    if (meterCount >= MaxPackages) return FALSE;
    EnergyReading* readings = (EnergyReading*)HeapAlloc(GetProcessHeap(),
        HEAP_ZERO_MEMORY, channels * sizeof(EnergyReading));
    if (!readings) return FALSE;
    Meter* meter = &meters[meterCount++];
    meter->handle = handle;
    meter->channels = channels;
    meter->readings = readings;
    for (DWORD p = 0; p < MaxPackages; ++p) {
        meter->packageChannels[p] = found[p];
        meter->dramChannels[p] = foundDram[p];
        if (found[p] != MAXDWORD) seen[p] = TRUE;
        if (foundDram[p] != MAXDWORD) dramSeen[p] = TRUE;
    }
    return TRUE;
}

BOOL DeltaPower(EnergyReading now, Previous* old, ULONGLONG wall,
        BOOL requirePositive, double* tenths) {
    BOOL good = old->valid && now.time > old->value.time &&
        now.energy >= old->value.energy && wall - old->wall <= 15000;
    if (good) {
        ULONGLONG dt = now.time - old->value.time;
        ULONGLONG de = now.energy - old->value.energy;
        ULONGLONG wallTicks = (wall - old->wall) * 10000;
        *tenths = (double)de * 0.36 / (double)dt;
        good = dt >= 1000000 && dt <= 150000000 && dt <= wallTicks + 10000000 &&
            dt + 10000000 >= wallTicks && *tenths >= 0 && *tenths <= 100000 &&
            (!requirePositive || de > 0);
    }
    old->value = now;
    old->wall = wall;
    old->valid = TRUE;
    return good;
}
}

void CpuPowerShutdown() {
    for (DWORD m = 0; m < meterCount; ++m) {
        CloseHandle(meters[m].handle);
        HeapFree(GetProcessHeap(), 0, meters[m].readings);
    }
    meterCount = 0;
    packageCount = 0;
    available = FALSE;
    dramAvailable = FALSE;
    dramMetadataGood = TRUE;
    for (DWORD p = 0; p < MaxPackages; ++p) {
        previous[p].valid = FALSE;
        previousDram[p].valid = FALSE;
        dramSeen[p] = FALSE;
    }
}

void CpuPowerInitialize() {
    CpuPowerShutdown();
    packageCount = CountPackages();
    if (!packageCount) return;
    HDEVINFO devices = SetupDiGetClassDevsW(&EnergyMeterGuid, 0, 0,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return;
    BOOL seen[MaxPackages] = {};
    BOOL good = TRUE;
    for (DWORD d = 0; d < 256; ++d) {
        SP_DEVICE_INTERFACE_DATA interfaceData = {};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(devices, 0, &EnergyMeterGuid, d, &interfaceData)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) good = FALSE;
            break;
        }
        if (d == 255) { good = FALSE; break; }
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, 0, 0, &bytes, 0);
        if (!bytes || bytes > MaxMetadata) { good = FALSE; break; }
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* detail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)HeapAlloc(GetProcessHeap(), 0, bytes);
        if (!detail) { good = FALSE; break; }
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        HANDLE handle = INVALID_HANDLE_VALUE;
        if (SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, bytes, 0, 0))
            handle = CreateFileW(detail->DevicePath, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
        HeapFree(GetProcessHeap(), 0, detail);
        if (handle == INVALID_HANDLE_VALUE) continue;
        DWORD before = meterCount;
        good = AddMeter(handle, seen);
        if (meterCount == before) CloseHandle(handle);
        if (!good) break;
    }
    SetupDiDestroyDeviceInfoList(devices);
    for (DWORD p = 0; p < packageCount; ++p) if (!seen[p]) good = FALSE;
    if (!good || !meterCount) CpuPowerShutdown();
    else {
        available = TRUE;
        dramAvailable = dramMetadataGood;
        for (DWORD p = 0; p < packageCount; ++p) if (!dramSeen[p]) dramAvailable = FALSE;
    }
}

CpuPowerSample CpuPowerRead() {
    CpuPowerSample result = {};
    if (!available) return result;
    BOOL complete = TRUE;
    BOOL dramComplete = dramAvailable;
    double totalTenths = 0;
    double totalDramTenths = 0;
    ULONGLONG wall = GetTickCount64();
    for (DWORD m = 0; m < meterCount; ++m) {
        Meter* meter = &meters[m];
        DWORD bytes = meter->channels * sizeof(EnergyReading), got = 0;
        if (!Query(meter->handle, 3, meter->readings, bytes, &got) || got != bytes) {
            for (DWORD p = 0; p < packageCount; ++p) {
                if (meter->packageChannels[p] != MAXDWORD) {
                    previous[p].valid = FALSE;
                    complete = FALSE;
                }
                if (meter->dramChannels[p] != MAXDWORD) {
                    previousDram[p].valid = FALSE;
                    dramComplete = FALSE;
                }
            }
            continue;
        }
        for (DWORD p = 0; p < packageCount; ++p) {
            DWORD c = meter->packageChannels[p];
            // Resets, wrap, unchanged timestamps, resume and long gaps establish
            // a new baseline. Never publish stale values after a failed read.
            double tenths = 0;
            if (c != MAXDWORD) {
                if (DeltaPower(meter->readings[c], &previous[p], wall, FALSE, &tenths))
                    totalTenths += tenths;
                else complete = FALSE;
            }
            c = meter->dramChannels[p];
            if (dramAvailable && c != MAXDWORD) {
                if (DeltaPower(meter->readings[c], &previousDram[p], wall, TRUE, &tenths))
                    totalDramTenths += tenths;
                else dramComplete = FALSE;
            }
        }
    }
    if (dramComplete && totalDramTenths > 0 && totalDramTenths <= 2147483646.0) {
        result.dramMeasured = TRUE;
        result.dramWattsTenths = (LONG)(totalDramTenths + 0.5);
    }
    if (complete && totalTenths <= 2147483646.0) {
        result.measured = TRUE;
        result.wattsTenths = (LONG)(totalTenths + 0.5);
    }
    return result;
}

const wchar_t* CpuPowerSource() {
    return available ? L"Windows EMI CPU package energy" : L"CPU power sensor unavailable";
}
