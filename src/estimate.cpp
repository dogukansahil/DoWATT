// Copyright (c) 2026 Dogukan Sahil
// Modified for DoWATT v1.0 on 2026-10-10. See LICENSE and NVML-EXCEPTION.txt.
// Additional permission under GPL v3 section 7: linking with installed NVIDIA NVML
// is permitted as described in NVML-EXCEPTION.txt.
// SPDX-License-Identifier: GPL-3.0-only
// Device inventory and explicit heuristics; no whole-PC power sensor is claimed.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include "estimate.h"
#include "cpu_power.h"
#include "graphics_inventory.h"
#include <wbemidl.h>
#include <winioctl.h>
#include <nvme.h>
#include <shlobj.h>
struct RamItem { ULONGLONG bytes; UINT type,speed; wchar_t part[96],maker[96]; };
struct DiskItem { wchar_t name[160],unique[160],id[64]; ULONGLONG bytes; UINT bus,media,number; BOOL included,numberValid,activityValid,baseline; LONG tenths,busy,nvIdle,nvActive; LONGLONG idle,time; HANDLE handle; };
struct GpuItem { wchar_t name[160],pnp[160]; };
typedef int (__cdecl *NvInit)();
typedef int (__cdecl *NvCount)(unsigned int *);
typedef int (__cdecl *NvHandle)(unsigned int,void **);
typedef int (__cdecl *NvPower)(void *,unsigned int *);
typedef int (__cdecl *NvEnergy)(void *,ULONGLONG *);
typedef int (__cdecl *NvName)(void *,char *,unsigned int);
static RamItem ram[32]; static DiskItem disks[64]; static GpuItem gpu[32];
static UINT ramCount,diskCount,gpuCount,nvidiaCount,cores,threads,cpuCount;
static wchar_t cpuName[192];
static BOOL cpuOK,ramOK,diskOK,gpuOK,unknownGpu,invalidRam,ramMismatch,notebook,gpuPowerValid,cpuMeasured,loadValid;
static ULONGLONG ramBytes,physicalRamBytes,lastRefresh;
static BOOL dramMeasured;static LONG dramTenths;
static LONG ramTenths,diskTenths,cpuTenths,gpuTenths,busyPermille,cpuIdleTenths,cpuMaxTenths,boardTenths;
static LONG refreshRequested;
static ULONGLONG priorIdle[64],priorKernel[64],priorUser[64]; static BOOL baseline[64];
static EstimateSample latest;
static HMODULE nvModule; static BOOL nvStarted; static unsigned int nvDevices;
static NvInit nvShutdown; static NvHandle nvHandle; static NvPower nvPower; static NvEnergy nvEnergy;
static NvName nvName;static wchar_t nvNames[32][160];static LONG nvWatts[32];static BOOL nvRead[32],nvUsedEnergy[32];
static LONG nvStatus; static ULONGLONG nvPriorEnergy[32],nvPriorTime[32]; static BOOL nvEnergyBaseline[32];
static void Copy(wchar_t *d, UINT cap, const wchar_t *s) {
    UINT i = 0; if (s) while (s[i] && i + 1 < cap) { d[i] = s[i]; ++i; }
    d[i] = 0;
}
static BOOL Contains(const wchar_t *s, const wchar_t *needle) {
    for (; *s; ++s) { UINT i=0; while (needle[i] && s[i] == needle[i]) ++i; if (!needle[i]) return TRUE; }
    return FALSE;
}
static ULONGLONG Value(IWbemClassObject *o, const wchar_t *key) {
    VARIANT v; VariantInit(&v); ULONGLONG n=0;
    if (SUCCEEDED(o->Get(key,0,&v,0,0))) {
        if (v.vt==VT_BSTR && v.bstrVal) {
            const wchar_t *p=v.bstrVal; BOOL valid=(*p!=0);
            while (*p) {
                if (*p<L'0' || *p>L'9') { valid=FALSE; break; }
                ULONGLONG digit=(ULONGLONG)(*p-L'0');
                if(n>(0xffffffffffffffffULL-digit)/10) { valid=FALSE; break; }
                n=n*10+digit; ++p;
            }
            if(!valid) n=0;
        }
        else if (v.vt==VT_I4 && v.lVal>=0) n=(ULONG)v.lVal;
        else if (v.vt==VT_UI4) n=v.ulVal;
        else if (v.vt==VT_I2 && v.iVal>=0) n=(USHORT)v.iVal;
        else if (v.vt==VT_UI2) n=v.uiVal;
        else if (v.vt==VT_UI8) n=v.ullVal;
    }
    VariantClear(&v); return n;
}
static void Text(IWbemClassObject *o, const wchar_t *key, wchar_t *out, UINT cap) {
    VARIANT v; VariantInit(&v); out[0]=0;
    if (SUCCEEDED(o->Get(key,0,&v,0,0)) && v.vt==VT_BSTR) Copy(out,cap,v.bstrVal);
    VariantClear(&v);
}
typedef void (*Visitor)(IWbemClassObject *);
static BOOL Query(const wchar_t *space, const wchar_t *query, Visitor visit, UINT limit) {
    IWbemLocator *locator=0; IWbemServices *service=0; IEnumWbemClassObject *rows=0;
    HRESULT hr=CoCreateInstance(CLSID_WbemLocator,0,CLSCTX_INPROC_SERVER,IID_IWbemLocator,(void **)&locator);
    if (FAILED(hr)) return FALSE;
    BSTR ns=SysAllocString(space);
    hr=ns ? locator->ConnectServer(ns,0,0,0,WBEM_FLAG_CONNECT_USE_MAX_WAIT,0,0,&service) : E_OUTOFMEMORY;
    SysFreeString(ns); locator->Release(); if (FAILED(hr)) return FALSE;
    hr=CoSetProxyBlanket(service,RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,0,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,0,EOAC_NONE);
    BSTR lang=SysAllocString(L"WQL"), sql=SysAllocString(query);
    if (SUCCEEDED(hr)) hr=(lang && sql) ? service->ExecQuery(lang,sql,WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,0,&rows) : E_OUTOFMEMORY;
    SysFreeString(lang); SysFreeString(sql); service->Release(); if (FAILED(hr)) return FALSE;
    BOOL complete=FALSE; ULONGLONG deadline=GetTickCount64()+8000; UINT count=0;
    while (count<limit && GetTickCount64()<deadline) {
        IWbemClassObject *o=0; ULONG got=0;
        hr=rows->Next(500,1,&o,&got);
        if (got && o) { visit(o); o->Release(); ++count; }
        if (hr==WBEM_S_FALSE) { complete=TRUE; break; }
        if (FAILED(hr)) break;
    }
    rows->Release(); return complete;
}
static BOOL Same(const wchar_t *a,const wchar_t *b) { return *a && *b && lstrcmpiW(a,b)==0; }
static BOOL Number(const wchar_t *s,UINT *n) {
    if(!*s) return FALSE; UINT v=0;
    while(*s) { if(*s<L'0'||*s>L'9'||v>100000) return FALSE; v=v*10+(UINT)(*s++-L'0'); } *n=v; return TRUE;
}
static void CpuVisit(IWbemClassObject *o) { wchar_t name[192]; Text(o,L"Name",name,192); if(!cpuCount) Copy(cpuName,192,name); cores+=(UINT)Value(o,L"NumberOfCores"); threads+=(UINT)Value(o,L"NumberOfLogicalProcessors"); ++cpuCount; }
static void SystemVisit(IWbemClassObject *o) { notebook=(Value(o,L"PCSystemType")==2); }
static void RamVisit(IWbemClassObject *o) {
    if(ramCount>=32) { invalidRam=TRUE; return; }
    RamItem *r=&ram[ramCount++]; r->bytes=Value(o,L"Capacity"); r->type=(UINT)Value(o,L"SMBIOSMemoryType"); r->speed=(UINT)Value(o,L"ConfiguredClockSpeed"); if(!r->speed) r->speed=(UINT)Value(o,L"Speed");
    Text(o,L"PartNumber",r->part,96); Text(o,L"Manufacturer",r->maker,96);
    if(!r->bytes || r->bytes>1024ULL*1024*1024*1024 || ramBytes>0xffffffffffffffffULL-r->bytes) { invalidRam=TRUE; return; }
    ramBytes+=r->bytes; ramTenths+=(r->type==34?18:r->type==26?15:20)+(LONG)(r->bytes*8/(1024ULL*1024*1024*10));
}
// Identify is read-only; power state0 declared references are not current telemetry.
static ULONGLONG nvQueryBuffer[530];
static LONG NvScale(USHORT value,UCHAR scale) {if(!value)return -1;if(scale==1)return (value+500)/1000;if(scale==2)return (value+5)/10;return -1;}
static void NvmeReferences(DiskItem *d) {
    d->nvIdle=d->nvActive=-1;if(d->bus!=17||d->handle==INVALID_HANDLE_VALUE)return;
    for(UINT i=0;i<530;++i)nvQueryBuffer[i]=0;
    BYTE *buffer=(BYTE *)nvQueryBuffer;STORAGE_PROPERTY_QUERY *q=(STORAGE_PROPERTY_QUERY *)buffer;
    q->PropertyId=StorageDeviceProtocolSpecificProperty;q->QueryType=PropertyStandardQuery;
    STORAGE_PROTOCOL_SPECIFIC_DATA *p=(STORAGE_PROTOCOL_SPECIFIC_DATA *)q->AdditionalParameters;
    p->ProtocolType=ProtocolTypeNvme;p->DataType=NVMeDataTypeIdentify;p->ProtocolDataRequestValue=NVME_IDENTIFY_CNS_CONTROLLER;p->ProtocolDataOffset=sizeof(*p);p->ProtocolDataLength=sizeof(NVME_IDENTIFY_CONTROLLER_DATA);
    DWORD got=0;if(!DeviceIoControl(d->handle,IOCTL_STORAGE_QUERY_PROPERTY,buffer,sizeof(nvQueryBuffer),buffer,sizeof(nvQueryBuffer),&got,0)||got<sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR)||got>sizeof(nvQueryBuffer))return;
    STORAGE_PROTOCOL_DATA_DESCRIPTOR *desc=(STORAGE_PROTOCOL_DATA_DESCRIPTOR *)buffer;p=&desc->ProtocolSpecificData;
    UINT start=(UINT)((BYTE *)p-buffer);if(desc->Version!=sizeof(*desc)||desc->Size!=sizeof(*desc)||p->ProtocolType!=ProtocolTypeNvme||p->ProtocolDataOffset<sizeof(*p)||p->ProtocolDataOffset>got-start||p->ProtocolDataLength<sizeof(NVME_IDENTIFY_CONTROLLER_DATA)||sizeof(NVME_IDENTIFY_CONTROLLER_DATA)>got-start-p->ProtocolDataOffset)return;
    NVME_IDENTIFY_CONTROLLER_DATA *data=(NVME_IDENTIFY_CONTROLLER_DATA *)((BYTE *)p+p->ProtocolDataOffset);
    NVME_POWER_STATE_DESC *state=&data->PDS[0];if(state->NOPS)return;
    LONG idle=NvScale(state->IDLP,state->IPS),active=NvScale(state->ACTP,state->APS);
    if(idle>=0&&active>0&&active>=idle&&active<=1000){d->nvIdle=idle;d->nvActive=active;}
}
static void DiskVisit(IWbemClassObject *o) {
    wchar_t unique[160],id[64]; Text(o,L"UniqueId",unique,160); Text(o,L"DeviceId",id,64); UINT number=0; BOOL numberValid=Number(id,&number);
    for(UINT i=0;i<diskCount;++i) if(Same(id,disks[i].id)||(numberValid&&disks[i].numberValid&&number==disks[i].number)||(!numberValid&&!disks[i].numberValid&&Same(unique,disks[i].unique))) return;
    if(diskCount>=64) return; DiskItem *d=&disks[diskCount++];
    Copy(d->unique,160,unique); Copy(d->id,64,id); Text(o,L"FriendlyName",d->name,160); d->bytes=Value(o,L"Size"); d->bus=(UINT)Value(o,L"BusType"); d->media=(UINT)Value(o,L"MediaType"); d->number=number; d->numberValid=numberValid;
    d->included=(d->bus==0||d->bus==1||d->bus==2||d->bus==3||d->bus==8||d->bus==10||d->bus==11||d->bus==17);
    if(Contains(d->name,L"Virtual")||Contains(d->name,L"VIRTUAL")||Contains(d->name,L"VMware")||Contains(d->name,L"VBOX")||Contains(d->name,L"QEMU")) d->included=FALSE;
    d->handle=INVALID_HANDLE_VALUE; d->nvIdle=d->nvActive=-1; d->baseline=FALSE; d->activityValid=FALSE; d->busy=0; d->tenths=0;
    if(d->included && numberValid) {
        wchar_t path[64]; Copy(path,64,L"\\\\.\\PhysicalDrive"); UINT pos=17; wchar_t digits[12]; UINT n=0,v=number; do {digits[n++]=(wchar_t)(L'0'+v%10);v/=10;}while(v); while(n) path[pos++]=digits[--n]; path[pos]=0;
        d->handle=CreateFileW(path,0,FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_EXISTING,0,0);
        // Confirm DeviceId really resolves to the matching Windows physical disk.
        if(d->handle!=INVALID_HANDLE_VALUE) { STORAGE_DEVICE_NUMBER s; DWORD got=0; if(!DeviceIoControl(d->handle,IOCTL_STORAGE_GET_DEVICE_NUMBER,0,0,&s,sizeof(s),&got,0)||s.DeviceType!=FILE_DEVICE_DISK||s.DeviceNumber!=number) {CloseHandle(d->handle);d->handle=INVALID_HANDLE_VALUE;d->numberValid=FALSE;} }
    }
    if(d->handle!=INVALID_HANDLE_VALUE&&d->bytes) {GET_LENGTH_INFORMATION length;DWORD got=0;if(DeviceIoControl(d->handle,IOCTL_DISK_GET_LENGTH_INFO,0,0,&length,sizeof(length),&got,0)&&(ULONGLONG)length.Length.QuadPart!=d->bytes){CloseHandle(d->handle);d->handle=INVALID_HANDLE_VALUE;d->numberValid=FALSE;}}
    NvmeReferences(d);
}
static void GpuVisit(IWbemClassObject *o) {
    wchar_t pnp[160];Text(o,L"PNPDeviceID",pnp,160);for(UINT i=0;i<gpuCount;++i)if(Same(pnp,gpu[i].pnp))return;
    if(gpuCount>=32) return; GpuItem *g=&gpu[gpuCount++]; Text(o,L"Name",g->name,160);Copy(g->pnp,160,pnp);
    if(Contains(g->pnp,L"VEN_10DE")) ++nvidiaCount;
    else if(Contains(g->pnp,L"VEN_8086")&&(Contains(g->name,L"UHD")||Contains(g->name,L"HD Graphics")||IntelGpuIsUma(g->name,g->pnp))) { }
    else if(Contains(g->pnp,L"PCI\\")) unknownGpu=TRUE;
    else if(!Contains(g->name,L"Remote")&&!Contains(g->name,L"Virtual")&&!Contains(g->name,L"Indirect")&&!Contains(g->name,L"Basic Render")) unknownGpu=TRUE;
}
static ULONGLONG Ticks(FILETIME f) {return ((ULONGLONG)f.dwHighDateTime<<32)|f.dwLowDateTime;}
static BOOL ReadLoad() {
    WORD groups=GetActiveProcessorGroupCount(); if(!groups||groups>64) return FALSE;
    GROUP_AFFINITY original; if(!GetThreadGroupAffinity(GetCurrentThread(),&original)) return FALSE;
    BOOL valid=TRUE; ULONGLONG totalSum=0,busySum=0;
    for(WORD group=0;group<groups;++group) {
        GROUP_AFFINITY affinity={}; affinity.Group=group; DWORD count=GetActiveProcessorCount(group); if(!count||count>64) {valid=FALSE;break;} affinity.Mask=count==64?~(KAFFINITY)0:(((KAFFINITY)1<<count)-1);
        if(groups>1&&!SetThreadGroupAffinity(GetCurrentThread(),&affinity,0)) {valid=FALSE;break;}
        FILETIME idle,kernel,user; if(!GetSystemTimes(&idle,&kernel,&user)) {valid=FALSE;break;}
        ULONGLONG i=Ticks(idle),k=Ticks(kernel),u=Ticks(user),total=(k-priorKernel[group])+(u-priorUser[group]),delta=i-priorIdle[group];
        if(!baseline[group]||k<priorKernel[group]||u<priorUser[group]||i<priorIdle[group]||!total||delta>total) valid=FALSE;
        else {totalSum+=total;busySum+=total-delta;}
        priorIdle[group]=i;priorKernel[group]=k;priorUser[group]=u;baseline[group]=TRUE;
    }
    if(groups>1&&!SetThreadGroupAffinity(GetCurrentThread(),&original,0)) valid=FALSE;
    if(valid&&totalSum) busyPermille=(LONG)(busySum*1000/totalSum); return valid&&totalSum;
}
static BOOL DiskBusy(LONGLONG time,LONGLONG idle,LONG *busy) {
    // QueryTime and IdleTime are collected from different clocks. Tiny positive
    // idle overshoot is normal on an idle drive; do not turn it into 50% load.
    if(time<=0||time>150000000||idle<0||idle>time+100000)return FALSE;
    if(idle>time)idle=time;
    *busy=(LONG)((time-idle)*1000/time);return TRUE;
}
static void ReadDisks() {
    diskTenths=0;
    for(UINT i=0;i<diskCount;++i) {
        DiskItem *d=&disks[i]; if(!d->included) continue; d->activityValid=FALSE;
        if(d->handle!=INVALID_HANDLE_VALUE) {DISK_PERFORMANCE p;DWORD got=0;
            if(DeviceIoControl(d->handle,IOCTL_DISK_PERFORMANCE,0,0,&p,sizeof(p),&got,0)&&got>=sizeof(p)) {
                LONGLONG time=p.QueryTime.QuadPart-d->time,idle=p.IdleTime.QuadPart-d->idle;
                if(d->baseline&&DiskBusy(time,idle,&d->busy))d->activityValid=TRUE;
                d->time=p.QueryTime.QuadPart;d->idle=p.IdleTime.QuadPart;d->baseline=TRUE;
            } else d->baseline=FALSE;
        }
        // OS busy fraction is activity telemetry, not a watt sensor. Missing activity uses midpoint.
        LONG idle=d->bus==17?5:d->media==4?5:d->media==3?40:20;
        LONG active=d->bus==17?80:d->media==4?40:d->media==3?100:100;
        if(d->nvIdle>=0&&d->nvActive>0){idle=d->nvIdle;active=d->nvActive;}
        LONG busy=d->activityValid?d->busy:500; d->tenths=idle+(active-idle)*busy/1000;diskTenths+=d->tenths;
    }
}
void EstimateRequestRefresh() {InterlockedExchange(&refreshRequested,1);}
void EstimateInitialize() {
    InterlockedExchange(&refreshRequested,0);
    EstimateShutdown(); ramCount=diskCount=gpuCount=nvidiaCount=cpuCount=cores=threads=0;cpuName[0]=0;ramBytes=physicalRamBytes=0;
    ramTenths=diskTenths=cpuTenths=gpuTenths=busyPermille=0;dramMeasured=FALSE;dramTenths=0;cpuOK=ramOK=diskOK=gpuOK=unknownGpu=invalidRam=ramMismatch=notebook=gpuPowerValid=cpuMeasured=loadValid=FALSE;
    nvDevices=0;for(UINT i=0;i<64;++i) baseline[i]=FALSE;for(UINT i=0;i<32;++i) nvEnergyBaseline[i]=FALSE;
    latest.ready=FALSE;latest.wattsTenths=-1;latest.uncertaintyTenths=0;
    cpuOK=Query(L"ROOT\\CIMV2",L"SELECT Name,NumberOfCores,NumberOfLogicalProcessors FROM Win32_Processor",CpuVisit,17);
    Query(L"ROOT\\CIMV2",L"SELECT PCSystemType FROM Win32_ComputerSystem",SystemVisit,2);
    ramOK=Query(L"ROOT\\CIMV2",L"SELECT Capacity,SMBIOSMemoryType,ConfiguredClockSpeed,Speed,PartNumber,Manufacturer FROM Win32_PhysicalMemory",RamVisit,33);
    ULONGLONG kb=0;if(GetPhysicallyInstalledSystemMemory(&kb)) {physicalRamBytes=kb*1024;ramMismatch=physicalRamBytes!=ramBytes;}
    diskOK=Query(L"ROOT\\Microsoft\\Windows\\Storage",L"SELECT FriendlyName,UniqueId,DeviceId,Size,BusType,MediaType FROM MSFT_PhysicalDisk",DiskVisit,65);
    gpuOK=Query(L"ROOT\\CIMV2",L"SELECT Name,PNPDeviceID FROM Win32_VideoController",GpuVisit,33);
    cpuIdleTenths=(notebook?40:120)*(LONG)(cpuCount?cpuCount:1);cpuMaxTenths=(notebook?450:1500)*(LONG)(cpuCount?cpuCount:1);boardTenths=notebook?100:300;
    CpuPowerInitialize();CpuPowerRead();ReadLoad();ReadDisks();lastRefresh=GetTickCount64();
    nvStatus=-1;
    // Trusted driver locations only. Never search current directory or PATH.
    nvModule=LoadLibraryExW(L"nvml.dll",0,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!nvModule) {wchar_t path[512];if(SUCCEEDED(SHGetFolderPathW(0,CSIDL_PROGRAM_FILES,0,SHGFP_TYPE_CURRENT,path))){UINT n=0;while(path[n]&&n<430)++n;if(n<430){Copy(path+n,512-n,L"\\NVIDIA Corporation\\NVSMI\\nvml.dll");nvModule=LoadLibraryExW(path,0,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);}}}
    if(!nvModule) {nvStatus=-2;return;}
    NvInit init=(NvInit)GetProcAddress(nvModule,"nvmlInit_v2");if(!init)init=(NvInit)GetProcAddress(nvModule,"nvmlInit");
    NvCount count=(NvCount)GetProcAddress(nvModule,"nvmlDeviceGetCount_v2");if(!count)count=(NvCount)GetProcAddress(nvModule,"nvmlDeviceGetCount");
    nvHandle=(NvHandle)GetProcAddress(nvModule,"nvmlDeviceGetHandleByIndex_v2");if(!nvHandle)nvHandle=(NvHandle)GetProcAddress(nvModule,"nvmlDeviceGetHandleByIndex");
    nvName=(NvName)GetProcAddress(nvModule,"nvmlDeviceGetName");for(UINT i=0;i<32;++i){nvNames[i][0]=0;nvRead[i]=nvUsedEnergy[i]=FALSE;}
    nvPower=(NvPower)GetProcAddress(nvModule,"nvmlDeviceGetPowerUsage");nvEnergy=(NvEnergy)GetProcAddress(nvModule,"nvmlDeviceGetTotalEnergyConsumption");nvShutdown=(NvInit)GetProcAddress(nvModule,"nvmlShutdown");
    if(!init||!count||!nvHandle||(!nvPower&&!nvEnergy)||!nvShutdown){nvStatus=-3;return;}
    nvStatus=init();if(nvStatus)return;nvStarted=TRUE;nvStatus=count(&nvDevices);if(!nvStatus&&(!nvDevices||nvDevices>32||nvDevices<nvidiaCount))nvStatus=-4;
}
EstimateSample EstimateRead() {
    if((GetTickCount64()-lastRefresh>=10000&&InterlockedExchange(&refreshRequested,0))||GetTickCount64()-lastRefresh>=300000) EstimateInitialize();
    latest.ready=FALSE;latest.wattsTenths=-1;latest.uncertaintyTenths=0;gpuPowerValid=FALSE;gpuTenths=0;for(UINT i=0;i<32;++i)nvRead[i]=nvUsedEnergy[i]=FALSE;
    loadValid=ReadLoad(); ReadDisks(); CpuPowerSample sample=CpuPowerRead();cpuMeasured=sample.measured&&sample.wattsTenths>=0;dramMeasured=sample.dramMeasured&&sample.dramWattsTenths>=0;dramTenths=dramMeasured?sample.dramWattsTenths:ramTenths;
    if(cpuMeasured) {cpuTenths=sample.wattsTenths;if(sample.cpuLimitTenths>0)cpuMaxTenths=sample.cpuLimitTenths;}
    else {if(!loadValid)return latest;cpuTenths=cpuIdleTenths+(LONG)((LONGLONG)(cpuMaxTenths-cpuIdleTenths)*busyPermille/1000);}
    if(!cpuOK||!cpuCount||!cores||!threads||!cpuName[0]||!ramOK||!ramCount||!ramBytes||invalidRam||ramMismatch||!diskOK||!gpuOK||unknownGpu)return latest;
    if(nvidiaCount||nvDevices) {
        if(!nvStarted||nvDevices<nvidiaCount||!nvDevices||nvDevices>32)return latest;
        for(UINT i=0;i<nvDevices;++i) {void *device=0;unsigned int mw=0;nvStatus=nvHandle(i,&device);if(nvStatus||!device)return latest;
            if(nvName&&!nvNames[i][0]){char name[160];if(!nvName(device,name,160))MultiByteToWideChar(CP_UTF8,0,name,-1,nvNames[i],160);}
            // Prefer an interval average matching CPU's sampling window when
            // the board energy counter is supported. Otherwise use board power.
            BOOL havePower=FALSE;
            if(nvEnergy) {ULONGLONG energy=0,now=GetTickCount64();nvStatus=nvEnergy(device,&energy);
                BOOL valid=!nvStatus&&nvEnergyBaseline[i]&&now>nvPriorTime[i]&&now-nvPriorTime[i]<=10000&&energy>nvPriorEnergy[i];
                if(valid&&energy-nvPriorEnergy[i]>2000*(now-nvPriorTime[i]))valid=FALSE;
                ULONGLONG power=valid?(energy-nvPriorEnergy[i])*1000/(now-nvPriorTime[i]):0;
                nvPriorEnergy[i]=energy;nvPriorTime[i]=now;nvEnergyBaseline[i]=!nvStatus;
                if(valid&&power<=2000000){mw=(unsigned int)power;nvUsedEnergy[i]=TRUE;havePower=TRUE;}
            }
            if(!havePower)nvStatus=nvPower?nvPower(device,&mw):3;
            if(nvStatus||mw>2000000)return latest;nvWatts[i]=(LONG)((mw+50)/100);nvRead[i]=TRUE;gpuTenths+=nvWatts[i];
        }gpuPowerValid=TRUE;
    }
    // Notebook result is modeled charger-equivalent input, excluding charging and display.
    latest.wattsTenths=(cpuTenths+gpuTenths+dramTenths+diskTenths+boardTenths)*10/9;
    LONG allowance=200+(cpuMeasured?cpuTenths/5:cpuMaxTenths)+(dramMeasured?dramTenths/5:ramTenths/2)+diskTenths+gpuTenths*15/100;
    latest.uncertaintyTenths=allowance*10/9+latest.wattsTenths/10;latest.ready=TRUE;return latest;
}
void EstimateShutdown() {
    CpuPowerShutdown();if(nvStarted&&nvShutdown)nvShutdown();nvStarted=FALSE;if(nvModule)FreeLibrary(nvModule);nvModule=0;nvHandle=0;nvPower=0;nvEnergy=0;nvShutdown=0;nvName=0;
    for(UINT i=0;i<diskCount;++i) if(disks[i].handle!=INVALID_HANDLE_VALUE) {CloseHandle(disks[i].handle);disks[i].handle=INVALID_HANDLE_VALUE;}
}
// Fixed-size JSON writer, no CRT, locale, streams, or third-party code.
static char json[65536]; static UINT jsonLength; static BOOL jsonFull;
static void Add(const char *s) { while (*s) { if (jsonLength+1>=sizeof(json)) { jsonFull=TRUE; return; } json[jsonLength++]=*s++; } }
static void Num(LONGLONG value) {
    if (value<0) { Add("-"); value=-value; }
    char digits[24]; UINT n=0; do { digits[n++]=(char)('0'+value%10); value/=10; } while(value);
    while(n) { char s[2]={digits[--n],0}; Add(s); }
}
static void Str(const wchar_t *s) {
    Add("\"");
    while (*s) {
        if (*s==L'"') Add("\\\""); else if (*s==L'\\') Add("\\\\");
        else if (*s<32) { Add(" "); }
        else { char bytes[8]={0}; UINT length=1; if (*s>=0xD800 && *s<=0xDBFF && s[1]>=0xDC00 && s[1]<=0xDFFF) length=2;
            int n=WideCharToMultiByte(CP_UTF8,0,s,length,bytes,7,0,0); if(n>0) { bytes[n]=0; Add(bytes); } if(length==2) ++s; }
        ++s;
    }
    Add("\"");
}
static void Field(const char *name,LONGLONG value) { Add("\""); Add(name); Add("\":"); Num(value); }

BOOL EstimateWriteReport(const wchar_t *path) {
    jsonLength=0;jsonFull=FALSE;
    Add("{\n\"schema\":2,\"method\":\"hardware-aware approximate AC input model\",\"measuredWholePc\":false,\"ready\":");Add(latest.ready?"true":"false");
    Add(",\"wattsTenths\":");if(latest.ready)Num(latest.wattsTenths);else Add("null");Add(",");Field("heuristicAllowanceTenths",latest.uncertaintyTenths);
    Add(",\"uncertaintyMeaning\":\"Broad model allowance; not a calibrated error bound or statistical confidence interval. CPU fallback has very low confidence.\"");
    Add(",\"inventoryComplete\":{");Field("cpu",cpuOK);Add(",");Field("ram",ramOK);Add(",");Field("storage",diskOK);Add(",");Field("gpu",gpuOK);Add("}");
    Add(",\"failureFlags\":{");Field("invalidRamCapacity",invalidRam);Add(",");Field("ramInventoryMismatch",ramMismatch);Add(",");Field("cpuActivityUnavailable",!loadValid);Add("}");
    Add(",\"cpu\":{\"name\":");Str(cpuName);Add(",");Field("sockets",cpuCount);Add(",");Field("cores",cores);Add(",");Field("logicalProcessors",threads);
    Add(",\"measured\":");Add(cpuMeasured?"true":"false");Add(",\"source\":");Str(cpuMeasured?CpuPowerSource():L"CPU utilization heuristic");Add(",\"sensorCapability\":");Str(CpuPowerSource());Add(",");Field("powerTenths",cpuTenths);Add(",\"busyPermille\":");if(loadValid)Num(busyPermille);else Add("null");
    Add(",");Field("idleAssumptionTenths",cpuIdleTenths);Add(",");Field("loadReferenceTenths",cpuMaxTenths);
    Add(",\"fallback\":\"LOW CONFIDENCE generic class, not CPU specifications: notebook 4W idle/45W load per socket; desktop 12W/150W. Linear OS utilization cannot observe turbo, instruction mix, voltage, P/E core placement, package idle or integrated GPU activity; references are not limits.\",\"activitySource\":\"GetSystemTimes sampled with worker affinity across every processor group\"}");
    Add(",\"ram\":{");Field("moduleCount",ramCount);Add(",");Field("installedBytes",(LONGLONG)ramBytes);Add(",\"physicalInstalledBytes\":");if(physicalRamBytes)Num((LONGLONG)physicalRamBytes);else Add("null");Add(",");Field("estimatedTenths",ramTenths);Add(",");Field("powerTenths",dramTenths);Add(",\"measured\":");Add(dramMeasured?"true":"false");Add(",\"powerSource\":");Add(dramMeasured?"\"Windows EMI RAPL DRAM domain\"":"\"generic DIMM model\"");
    Add(",\"source\":\"Win32_PhysicalMemory; total checked against GetPhysicallyInstalledSystemMemory when available\",\"assumption\":\"per DIMM DDR4 1.5W,DDR5 1.8W,other2W plus0.08W/GiB; generic estimate, not a module sensor or manufacturer specification\",\"modules\":[");
    for(UINT i=0;i<ramCount;++i){if(i)Add(",");Add("{");Field("capacityBytes",(LONGLONG)ram[i].bytes);Add(",");Field("smbiosMemoryType",ram[i].type);Add(",");Field("configuredSpeedMHz",ram[i].speed);Add(",\"partNumber\":");Str(ram[i].part);Add(",\"manufacturer\":");Str(ram[i].maker);Add("}");}Add("]}");
    UINT included=0,nvme=0,sataSsd=0,hdd=0,unknown=0,excluded=0;
    for(UINT i=0;i<diskCount;++i){if(!disks[i].included){++excluded;continue;}++included;if(disks[i].bus==17)++nvme;else if(disks[i].bus==11&&disks[i].media==4)++sataSsd;else if(disks[i].media==3)++hdd;else if(disks[i].media!=4)++unknown;}
    Add(",\"storage\":{");Field("physicalDeviceCount",included);Add(",");Field("nvmeCount",nvme);Add(",");Field("sataSsdCount",sataSsd);Add(",");Field("hddCount",hdd);Add(",");Field("unknownMediumCount",unknown);Add(",");Field("excludedDeviceCount",excluded);Add(",");Field("estimatedTenths",diskTenths);
    Add(",\"source\":\"MSFT_PhysicalDisk deduplicated by UniqueId,DeviceId,physical number; never counts logical volumes or mounted shares\",\"assumption\":\"linear idle/active typical reference watts: NVMe0.5/8,SSD0.5/4,HDD4/10,unknown2/10; generic defaults except validated NVMe power state0 idle/active declarations when provided; current power state unknown, not measured power. Missing busy counter uses midpoint. Excludes USB,network,virtual and Storage Spaces buses. RAID abstractions can hide member disks.\",\"activitySource\":\"read-only IOCTL_DISK_PERFORMANCE idle/query deltas; absent or inaccessible counters remain unavailable, never enabled or modified\",\"devices\":[");
    for(UINT i=0;i<diskCount;++i){DiskItem *d=&disks[i];if(i)Add(",");Add("{\"name\":");Str(d->name);Add(",\"uniqueId\":");Str(d->unique);Add(",\"deviceId\":");Str(d->id);Add(",\"physicalDriveNumber\":");if(d->numberValid)Num(d->number);else Add("null");Add(",");Field("capacityBytes",(LONGLONG)d->bytes);Add(",");Field("busType",d->bus);Add(",");Field("mediaType",d->media);Add(",\"included\":");Add(d->included?"true":"false");Add(",\"busyPermille\":");if(d->activityValid)Num(d->busy);else Add("null");Add(",");Field("estimatedTenths",d->tenths);Add(",\"nvmeDeclaredIdleTenths\":");if(d->nvIdle>=0)Num(d->nvIdle);else Add("null");Add(",\"nvmeDeclaredActiveTenths\":");if(d->nvActive>0)Num(d->nvActive);else Add("null");Add("}");}Add("]}");
    Add(",\"gpu\":{");Field("nvmlDeviceCount",nvDevices);Add(",");Field("nvidiaInventoryCount",nvidiaCount);Add(",");Field("nvmlStatus",nvStatus);Add(",\"measuredTenths\":");if(gpuPowerValid)Num(gpuTenths);else Add("null");Add(",\"unknownAdapter\":");Add(unknownGpu?"true":"false");
    Add(",\"method\":\"Installed NVIDIA NVML board power across ALL enumerated devices, no model whitelist; power usage mW or interval-average total energy mJ fallback. Sampling period/coverage depend on driver and GPU. NVML may enumerate more devices than WMI on hybrid systems; fewer than WMI fails closed. Integrated Intel graphics is within package telemetry when available; CPU fallback does not observe its load.\",\"adapters\":[");
    for(UINT i=0;i<gpuCount;++i){if(i)Add(",");Add("{\"name\":");Str(gpu[i].name);Add(",\"pnpDeviceId\":");Str(gpu[i].pnp);Add("}");}Add("]}");
    Add(",\"nvidiaSensors\":[");for(UINT i=0;i<nvDevices&&i<32;++i){if(i)Add(",");Add("{\"name\":");Str(nvNames[i]);Add(",\"powerTenths\":");if(nvRead[i])Num(nvWatts[i]);else Add("null");Add(",\"source\":");Add(nvUsedEnergy[i]?"\"NVML energy interval\"":"\"NVML power usage\"");Add("}");}Add("]");
    Add(",\"other\":{");Field("motherboardFansPeripheralTenths",boardTenths);Add(",\"deviceClass\":");Add(notebook?"\"notebook\"":"\"desktop_or_unknown\"");Add(",\"psuEfficiencyPermille\":900,\"efficiencyMeasured\":false,\"assumption\":\"Generic30W desktop/10W notebook board/fans/peripherals;90% PSU or charger conversion is only a default, not telemetry. External monitor excluded. Notebook display, battery charging/discharging and actual adapter efficiency unknown; AC estimate may differ greatly.\"}");
    Add(",\"refresh\":\"Inventory cached, refreshed every5minutes or next worker tick after user request with10second cooldown\",\"failurePolicy\":\"N/A for incomplete inventory, invalid/mismatched RAM capacities, unknown discrete GPU, missing NVIDIA telemetry or uncovered NVIDIA devices, or unavailable CPU power and utilization. Never substitutes partial GPU power as whole-PC power.\"");
    Add(",\"sources\":[\"https://docs.nvidia.com/deploy/nvml-api/latest/\",\"https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes\",\"https://learn.microsoft.com/en-us/windows-hardware/drivers/storage/msft-physicaldisk\",\"https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntdddisk/ns-ntdddisk-_disk_performance\"]\n}\n");
    if(jsonFull||!path)return FALSE;HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);if(file==INVALID_HANDLE_VALUE)return FALSE;
    DWORD written=0;BOOL ok=WriteFile(file,json,jsonLength,&written,0)&&written==jsonLength;CloseHandle(file);return ok;
}
