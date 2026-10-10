/*
 * PowerMon GUI v3.0 — Moniteur de consommation électrique Windows 11
 * GUI Win32 natif + Direct2D pour les graphes
 * Capteurs : PDH (CPU/GPU%), WMI (noms/températures/voltages),
 *            DXGI (GPU), GlobalMemoryStatusEx (RAM),
 *            SetupAPI (USB), GetSystemPowerStatus (batterie),
 *            EnumDisplayDevices (écrans), ADL2 (AMD GPU réel),
 *            NvAPI (NVIDIA GPU réel), GetLogicalDrives (stockage),
 *            GetAdaptersAddresses (réseau détaillé)
 *
 * Compile : MSVC 2019/2022  x64  (voir CMakeLists.txt)
 * Aucune dépendance externe — Win32 API pure + D2D/DWrite
 *
 * v3.0 :
 *  - Fix Tj AMD : index ADL_PMLOG_TEMPERATURE_HOTSPOT (80) au lieu de EDGE (7)
 *  - Fix struct AdlPmLogData : ulReserved aligné sur le vrai SDK
 *  - Lecture température CPU via WMI MSAcpi_ThermalZoneTemperature
 *  - Nouveaux composants : chipset/VRM, SSD NVMe SMART (temp + santé),
 *    ventilateurs (WMI Win32_Fan), voltages CPU (WMI),
 *    carte son (puissance estimée), Bluetooth, carte réseau détaillée
 *  - Section "Températures" dédiée avec jauges colorées
 *  - Indicateur GPU source (ADL réel / NvAPI réel / estimation TDP)
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define COBJMACROS

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>

// Direct2D / DWrite
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>

// Capteurs
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <comdef.h>
#include <wbemidl.h>
#include <psapi.h>
#include <dxgi.h>
#include <iphlpapi.h>
#include <devioctl.h>
#include <ntddscsi.h>

// STL
#include <string>
#include <vector>
#include <array>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <memory>
#include <functional>
#include <map>
#include <chrono>
#include <cmath>
#include <optional>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "comctl32.lib")

// ─────────────────────────────────────────────────────────────────
//  CONFIGURATION TDP
// ─────────────────────────────────────────────────────────────────
static double g_CpuTdpMax   = 95.0;
static double g_CpuTdpIdle  =  8.0;
static double g_GpuTdpMax   = 185.0;
static double g_GpuTdpIdle  =  12.0;
static double g_SysBase     =  45.0;
static double g_TarifKwh    =  0.2516;
static const int HIST = 120;

// ─────────────────────────────────────────────────────────────────
//  COULEURS
// ─────────────────────────────────────────────────────────────────
namespace C {
    static const D2D1_COLOR_F BG       = {0.027f,0.031f,0.051f,1};
    static const D2D1_COLOR_F PANEL    = {0.051f,0.059f,0.094f,1};
    static const D2D1_COLOR_F PANEL2   = {0.043f,0.051f,0.078f,1};
    static const D2D1_COLOR_F BORDER   = {0.118f,0.137f,0.204f,1};
    static const D2D1_COLOR_F CYAN     = {0.000f,0.831f,1.000f,1};
    static const D2D1_COLOR_F ORANGE   = {1.000f,0.420f,0.208f,1};
    static const D2D1_COLOR_F GREEN    = {0.220f,1.000f,0.557f,1};
    static const D2D1_COLOR_F RED      = {1.000f,0.200f,0.337f,1};
    static const D2D1_COLOR_F YELLOW   = {1.000f,0.878f,0.200f,1};
    static const D2D1_COLOR_F WHITE    = {0.784f,0.816f,0.910f,1};
    static const D2D1_COLOR_F MUTED    = {0.290f,0.322f,0.447f,1};
    static const D2D1_COLOR_F DIMTEXT  = {0.420f,0.459f,0.600f,1};
    static const D2D1_COLOR_F PURPLE   = {0.690f,0.408f,1.000f,1};
    static const D2D1_COLOR_F CYANDIM  = {0.000f,0.831f,1.000f,0.12f};
    static const D2D1_COLOR_F TEAL     = {0.000f,0.780f,0.680f,1};
    static const D2D1_COLOR_F PINK     = {1.000f,0.400f,0.700f,1};
    static const D2D1_COLOR_F LBLUE    = {0.400f,0.700f,1.000f,1};

    // Couleur température selon valeur
    static D2D1_COLOR_F tempColor(double t) {
        if (t < 50)  return GREEN;
        if (t < 70)  return YELLOW;
        if (t < 85)  return ORANGE;
        return RED;
    }
}

// ─────────────────────────────────────────────────────────────────
//  STRUCTURES
// ─────────────────────────────────────────────────────────────────

struct DiskInfo {
    std::wstring letter;      // "C:"
    std::wstring label;       // "Windows"
    std::wstring type;        // "NVMe", "SSD", "HDD"
    std::wstring model;
    double totalGB   = 0;
    double usedGB    = 0;
    double tempC     = -1;    // SMART via IOCTL (-1 = indispo)
    int    healthPct = -1;    // % santé SMART
    double estWatt   = 0;
    double readMBs   = 0;
    double writeMBs  = 0;
};

struct FanInfo {
    std::wstring name;
    int    rpm     = 0;
    double watt    = 0.15;    // ~0.15W par ventilateur
};

struct NetAdapterInfo {
    std::wstring name;
    std::wstring mac;
    std::wstring ipv4;
    double upKBs   = 0;
    double downKBs = 0;
    double estWatt = 2.5;     // ~2.5W carte filaire, ~1.5W WiFi
    bool   wifi    = false;
    bool   active  = false;
};

struct AudioInfo {
    std::wstring name;
    double estWatt = 3.0;     // intégrée ~1W, dédiée ~3-5W
    bool   present = false;
};

struct VoltageInfo {
    double vcore   = -1;   // V
    double v12     = -1;
    double v5      = -1;
    double v3_3    = -1;
};

struct SensorData {
    // CPU
    double cpuLoad     = 0;
    double cpuWatt     = 0;
    double cpuTempC    = -1;  // -1 = inconnu
    double cpuFreqMhz  = 0;
    int    cpuCores    = 0;
    std::wstring cpuName;
    VoltageInfo  voltages;

    // GPU
    double gpuLoad      = 0;
    double gpuWatt      = 0;
    double gpuTempC     = -1;
    double gpuTjC       = -1; // Junction / Hotspot (AMD Tj, NVIDIA hotspot)
    double gpuMemUsedMB = 0;
    double gpuMemTotalMB= 0;
    double gpuCoreMhz   = 0;
    std::wstring gpuName;
    size_t gpuVramBytes = 0;
    bool   gpuWattReal  = false;
    bool   gpuTempReal  = false;
    enum class GpuSrc { ESTIMATE, ADL, NVAPI } gpuSrc = GpuSrc::ESTIMATE;

    // RAM
    double ramUsedGB   = 0;
    double ramTotalGB  = 0;
    double ramPct      = 0;
    double ramWatt     = 0;
    double ramFreqMhz  = 0;
    double ramTempC    = -1;  // SPD thermique (rare, WMI)

    // Stockage
    std::vector<DiskInfo> disks;
    double diskTotalWatt = 0;

    // Réseau
    std::vector<NetAdapterInfo> netAdapters;
    double netTotalUpKBs   = 0;
    double netTotalDownKBs = 0;
    double netTotalWatt    = 0;

    // Audio
    AudioInfo audio;

    // Ventilateurs
    std::vector<FanInfo> fans;
    double fanTotalWatt = 0;
    int    fanCount     = 0;

    // Chipset / VRM (estimation)
    double chipsetWatt = 5.0;
    double vrmWatt     = 3.0;

    // Bluetooth
    bool   btPresent   = false;
    double btWatt      = 0.5;

    // Batterie / secteur
    bool   onBattery   = false;
    int    battPct     = -1;
    bool   charging    = false;

    // USB
    int    usbCount    = 0;
    double usbWatt     = 0;
    std::vector<std::pair<std::wstring,double>> usbDevices;

    // Écrans
    struct MonitorInfo {
        std::wstring deviceName;
        std::wstring friendlyName;
        std::wstring edidMfr;
        int    widthPx  = 0;
        int    heightPx = 0;
        int    widthMm  = 0;
        int    heightMm = 0;
        double diagInch = 0;
        int    refreshHz= 0;
        bool   primary  = false;
        double estWattW = 0;
    };
    int    monitorCount = 0;
    double monitorWatt  = 0;
    std::vector<MonitorInfo> monitors;

    // Total
    double totalWatt   = 0;
    double cumWhSession= 0;
    int64_t sessionSec = 0;
};

static SensorData g_Sensor;
static std::mutex g_Mtx;
static std::deque<double> g_History;
static std::deque<double> g_CpuHistory;
static std::deque<double> g_GpuHistory;
static std::atomic<bool>  g_Running{true};

// ─────────────────────────────────────────────────────────────────
//  PDH
// ─────────────────────────────────────────────────────────────────
struct PdhCtx {
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER cCpuLoad   = nullptr;
    PDH_HCOUNTER cGpuEngine = nullptr;
    PDH_HCOUNTER cDiskRead  = nullptr;
    PDH_HCOUNTER cDiskWrite = nullptr;
    PDH_HCOUNTER cNetSent   = nullptr;
    PDH_HCOUNTER cNetRecv   = nullptr;
    bool ok = false;

    bool init() {
        if (PdhOpenQuery(nullptr,0,&q) != ERROR_SUCCESS) return false;
        PdhAddEnglishCounterW(q, L"\\Processor(_Total)\\% Processor Time",        0, &cCpuLoad);
        PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage",       0, &cGpuEngine);
        PdhAddEnglishCounterW(q, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",   0, &cDiskRead);
        PdhAddEnglishCounterW(q, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",  0, &cDiskWrite);
        PdhAddEnglishCounterW(q, L"\\Network Interface(*)\\Bytes Sent/sec",        0, &cNetSent);
        PdhAddEnglishCounterW(q, L"\\Network Interface(*)\\Bytes Received/sec",    0, &cNetRecv);
        PdhCollectQueryData(q);
        ok = true; return true;
    }

    double readScalar(PDH_HCOUNTER c) {
        if (!c) return 0;
        PDH_FMT_COUNTERVALUE v{};
        PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v);
        return std::max(0.0, v.doubleValue);
    }

    double readGpuLoad() {
        if (!cGpuEngine) return 0;
        DWORD sz=0,cnt=0;
        PdhGetFormattedCounterArrayW(cGpuEngine,PDH_FMT_DOUBLE,&sz,&cnt,nullptr);
        if (!sz) return 0;
        std::vector<BYTE> buf(sz);
        auto* arr = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(cGpuEngine,PDH_FMT_DOUBLE,&sz,&cnt,arr)!=ERROR_SUCCESS) return 0;
        double tot=0;
        for (DWORD i=0;i<cnt;i++) tot+=arr[i].FmtValue.doubleValue;
        return std::clamp(tot,0.0,100.0);
    }

    double readNetSum(PDH_HCOUNTER c) {
        if (!c) return 0;
        DWORD sz=0,cnt=0;
        PdhGetFormattedCounterArrayW(c,PDH_FMT_DOUBLE,&sz,&cnt,nullptr);
        if (!sz) return 0;
        std::vector<BYTE> buf(sz);
        auto* arr = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(c,PDH_FMT_DOUBLE,&sz,&cnt,arr)!=ERROR_SUCCESS) return 0;
        double tot=0;
        for (DWORD i=0;i<cnt;i++) tot+=arr[i].FmtValue.doubleValue;
        return tot;
    }

    ~PdhCtx() { if(q) PdhCloseQuery(q); }
};

// ─────────────────────────────────────────────────────────────────
//  WMI
// ─────────────────────────────────────────────────────────────────
static IWbemServices* g_WmiSvc    = nullptr;
static IWbemServices* g_WmiSvcHw  = nullptr;  // ROOT\WMI

static bool initWmi() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoInitializeSecurity(nullptr,-1,nullptr,nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT,RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,EOAC_NONE,nullptr);

    auto connect = [](const wchar_t* ns, IWbemServices** svc) -> bool {
        IWbemLocator* loc=nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator,0,CLSCTX_INPROC_SERVER,
            IID_IWbemLocator,(void**)&loc))) return false;
        bool r = SUCCEEDED(loc->ConnectServer(_bstr_t(ns),nullptr,nullptr,0,0,0,0,svc));
        if (r) CoSetProxyBlanket(*svc,RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,
            nullptr,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE);
        loc->Release();
        return r;
    };
    connect(L"ROOT\\CIMV2", &g_WmiSvc);
    connect(L"ROOT\\WMI",   &g_WmiSvcHw);
    return g_WmiSvc != nullptr;
}

static std::wstring wmiGetStr(IWbemServices* svc, const wchar_t* cls, const wchar_t* prop) {
    if (!svc) return {};
    std::wstring q = std::wstring(L"SELECT ") + prop + L" FROM " + cls;
    IEnumWbemClassObject* en=nullptr;
    if (FAILED(svc->ExecQuery(_bstr_t(L"WQL"),_bstr_t(q.c_str()),
        WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&en))) return {};
    std::wstring res;
    IWbemClassObject* obj=nullptr; ULONG ret=0;
    if (en->Next(WBEM_INFINITE,1,&obj,&ret)==S_OK) {
        VARIANT v; VariantInit(&v);
        if (SUCCEEDED(obj->Get(prop,0,&v,0,0)) && v.vt==VT_BSTR) res=v.bstrVal;
        VariantClear(&v); obj->Release();
    }
    en->Release();
    return res;
}

static double wmiGetDbl(IWbemServices* svc, const wchar_t* cls, const wchar_t* prop) {
    if (!svc) return -1;
    std::wstring q = std::wstring(L"SELECT ") + prop + L" FROM " + cls;
    IEnumWbemClassObject* en=nullptr;
    if (FAILED(svc->ExecQuery(_bstr_t(L"WQL"),_bstr_t(q.c_str()),
        WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&en))) return -1;
    double res=-1;
    IWbemClassObject* obj=nullptr; ULONG ret=0;
    if (en->Next(WBEM_INFINITE,1,&obj,&ret)==S_OK) {
        VARIANT v; VariantInit(&v);
        if (SUCCEEDED(obj->Get(prop,0,&v,0,0))) {
            if (v.vt==VT_I4)  res=(double)v.intVal;
            else if(v.vt==VT_UI4) res=(double)v.uintVal;
            else if(v.vt==VT_R8)  res=v.dblVal;
            else if(v.vt==VT_R4)  res=(double)v.fltVal;
        }
        VariantClear(&v); obj->Release();
    }
    en->Release();
    return res;
}

// Fréquence CPU
static double wmiGetCpuFreq() {
    return wmiGetDbl(g_WmiSvc, L"Win32_Processor", L"CurrentClockSpeed");
}

// Température CPU via MSAcpi_ThermalZoneTemperature (ROOT\WMI)
// Valeur en dixièmes de Kelvin → °C = (val/10) - 273.15
static double wmiGetCpuTemp() {
    if (!g_WmiSvcHw) return -1;
    double raw = wmiGetDbl(g_WmiSvcHw, L"MSAcpi_ThermalZoneTemperature", L"CurrentTemperature");
    if (raw < 0) return -1;
    double c = (raw / 10.0) - 273.15;
    if (c < 0 || c > 120) return -1;
    return c;
}

// Ventilateurs WMI
static void wmiGetFans(std::vector<FanInfo>& fans) {
    fans.clear();
    if (!g_WmiSvc) return;
    IEnumWbemClassObject* en=nullptr;
    g_WmiSvc->ExecQuery(_bstr_t(L"WQL"),
        _bstr_t(L"SELECT Name,DesiredSpeed FROM Win32_Fan"),
        WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&en);
    if (!en) return;
    IWbemClassObject* obj=nullptr; ULONG ret=0;
    while(en->Next(WBEM_INFINITE,1,&obj,&ret)==S_OK) {
        FanInfo fi;
        VARIANT v; VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"Name",0,&v,0,0)) && v.vt==VT_BSTR)
            fi.name=v.bstrVal;
        VariantClear(&v); VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"DesiredSpeed",0,&v,0,0)))
            fi.rpm = (v.vt==VT_I4)?(int)v.intVal:(v.vt==VT_UI4)?(int)v.uintVal:0;
        VariantClear(&v);
        obj->Release();
        if (fi.rpm>0) fans.push_back(fi);
    }
    en->Release();
}

// Carte son
static void wmiGetAudio(AudioInfo& audio) {
    audio.present = false;
    if (!g_WmiSvc) return;
    std::wstring name = wmiGetStr(g_WmiSvc, L"Win32_SoundDevice", L"Name");
    if (!name.empty()) {
        audio.present = true;
        audio.name    = name;
        std::wstring lo = name;
        for (auto& c : lo) c = towlower(c);
        if (lo.find(L"realtek")!=std::wstring::npos||lo.find(L"high definition")!=std::wstring::npos)
            audio.estWatt = 1.0;   // intégré
        else if (lo.find(L"focusrite")!=std::wstring::npos||lo.find(L"scarlett")!=std::wstring::npos)
            audio.estWatt = 5.0;
        else
            audio.estWatt = 2.5;
    }
}

// Voltages CPU via WMI Win32_Processor (VoltageProbeDescription)
static void wmiGetVoltages(VoltageInfo& v) {
    if (!g_WmiSvc) return;
    IEnumWbemClassObject* en=nullptr;
    g_WmiSvc->ExecQuery(_bstr_t(L"WQL"),
        _bstr_t(L"SELECT CurrentVoltage FROM Win32_Processor"),
        WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&en);
    if (!en) return;
    IWbemClassObject* obj=nullptr; ULONG ret=0;
    if (en->Next(WBEM_INFINITE,1,&obj,&ret)==S_OK) {
        VARIANT vv; VariantInit(&vv);
        if (SUCCEEDED(obj->Get(L"CurrentVoltage",0,&vv,0,0))) {
            double raw = (vv.vt==VT_I4)?(double)vv.intVal:(vv.vt==VT_UI4)?(double)vv.uintVal:0;
            if (raw > 0) v.vcore = raw / 10.0;  // en dixièmes de volts
        }
        VariantClear(&vv); obj->Release();
    }
    en->Release();
}

// ─────────────────────────────────────────────────────────────────
//  DXGI — GPU VRAM
// ─────────────────────────────────────────────────────────────────
static void dxgiGetGpuInfo(std::wstring& name, size_t& vramBytes) {
    IDXGIFactory* fac=nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory),(void**)&fac))) return;
    IDXGIAdapter* ada=nullptr;
    if (SUCCEEDED(fac->EnumAdapters(0,&ada))) {
        DXGI_ADAPTER_DESC d{};
        if (SUCCEEDED(ada->GetDesc(&d))) {
            name = d.Description; vramBytes = d.DedicatedVideoMemory;
        }
        ada->Release();
    }
    fac->Release();
}

// ─────────────────────────────────────────────────────────────────
//  STOCKAGE — Disques + SMART basique (température)
// ─────────────────────────────────────────────────────────────────

// Détecte le type de disque depuis le modèle
static std::wstring guessDiskType(const std::wstring& model) {
    std::wstring lo = model;
    for (auto& c : lo) c = towlower(c);
    if (lo.find(L"nvme")!=std::wstring::npos || lo.find(L"nvm ")!=std::wstring::npos) return L"NVMe";
    if (lo.find(L"ssd")!=std::wstring::npos)  return L"SSD";
    if (lo.find(L"flash")!=std::wstring::npos) return L"SSD";
    return L"HDD";
}

static double diskTypeWatt(const std::wstring& type) {
    if (type==L"NVMe") return 3.5;
    if (type==L"SSD")  return 2.0;
    return 5.0; // HDD
}

// Tentative lecture température NVMe via IOCTL_SCSI_PASS_THROUGH
// (marche sur certains drivers NVMe)
static double readNvmeTemp(const std::wstring& physPath) {
    HANDLE h = CreateFileW(physPath.c_str(), GENERIC_READ|GENERIC_WRITE,
                           FILE_SHARE_READ|FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;

    // Commande NVMe Get Log Page (Log ID 0x02 = SMART/Health)
    struct {
        SCSI_PASS_THROUGH_DIRECT sptd;
        BYTE sense[64];
    } s{};
    BYTE buf[512]{};

    s.sptd.Length          = sizeof(SCSI_PASS_THROUGH_DIRECT);
    s.sptd.CdbLength       = 16;
    s.sptd.DataIn          = SCSI_IOCTL_DATA_IN;
    s.sptd.DataTransferLength = sizeof(buf);
    s.sptd.DataBuffer      = buf;
    s.sptd.TimeOutValue    = 2;
    s.sptd.SenseInfoOffset = offsetof(decltype(s), sense);
    s.sptd.SenseInfoLength = 64;
    // ATA passthrough commande SMART READ DATA (0xB0/0xD0)
    s.sptd.Cdb[0] = 0x85; // ATA16
    s.sptd.Cdb[1] = 0x08; // PIO data in
    s.sptd.Cdb[3] = 0x4F;
    s.sptd.Cdb[5] = 0xC2;
    s.sptd.Cdb[14]= 0xB0;
    s.sptd.Cdb[13]= 0xD0;

    DWORD returned = 0;
    DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &s, sizeof(s),
                    &s, sizeof(s), &returned, nullptr);
    CloseHandle(h);

    // Température SMART attribute 190 ou 194 (offset dans buf ~50-60)
    // Format : id(1) + flags(2) + current(1) + worst(1) + raw(6)
    for (int i = 2; i < 362 && i+11 < 512; i += 12) {
        BYTE id = buf[i];
        if (id == 190 || id == 194) {
            double t = (double)buf[i+5]; // byte bas du raw
            if (t > 0 && t < 120) { return t; }
        }
    }
    return -1;
}

static void enumDisks(std::vector<DiskInfo>& disks, double& totalWatt,
                      double rdMBs, double wrMBs) {
    disks.clear(); totalWatt = 0;

    // Récupère les lecteurs logiques
    DWORD driveMask = GetLogicalDrives();
    for (int i = 0; i < 26 && driveMask; i++, driveMask >>= 1) {
        if (!(driveMask & 1)) continue;
        wchar_t root[8]; swprintf_s(root, L"%c:\\", L'A'+i);
        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED) continue;

        DiskInfo di;
        di.letter = std::wstring(1, L'A'+i) + L":";

        // Espace disque
        ULARGE_INTEGER free64{}, total64{};
        if (GetDiskFreeSpaceExW(root, nullptr, &total64, &free64)) {
            di.totalGB = (double)total64.QuadPart / (1ull<<30);
            di.usedGB  = di.totalGB - (double)free64.QuadPart / (1ull<<30);
        }

        // Label volume
        wchar_t label[MAX_PATH]{};
        GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        di.label = label;

        // Modèle via WMI (associe la lettre au disque physique)
        // Simplifié : on utilise le modèle du premier disque physique correspondant
        if (g_WmiSvc) {
            std::wstring q = L"ASSOCIATORS OF {Win32_LogicalDisk.DeviceID='" +
                             di.letter + L"'} WHERE AssocClass=Win32_LogicalDiskToPartition";
            IEnumWbemClassObject* en=nullptr;
            g_WmiSvc->ExecQuery(_bstr_t(L"WQL"),_bstr_t(q.c_str()),
                WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&en);
            if (en) { en->Release(); }

            // Fallback : récupère le modèle du premier DiskDrive
            if (di.model.empty()) {
                IEnumWbemClassObject* en2=nullptr;
                g_WmiSvc->ExecQuery(_bstr_t(L"WQL"),
                    _bstr_t(L"SELECT Model FROM Win32_DiskDrive"),
                    WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&en2);
                if (en2) {
                    IWbemClassObject* obj=nullptr; ULONG ret=0;
                    if (en2->Next(WBEM_INFINITE,1,&obj,&ret)==S_OK) {
                        VARIANT v; VariantInit(&v);
                        if (SUCCEEDED(obj->Get(L"Model",0,&v,0,0)) && v.vt==VT_BSTR)
                            di.model = v.bstrVal;
                        VariantClear(&v); obj->Release();
                    }
                    en2->Release();
                }
            }
        }

        di.type = guessDiskType(di.model);
        di.estWatt = diskTypeWatt(di.type);

        // Tentative température SMART (NVMe)
        if (di.type == L"NVMe") {
            di.tempC = readNvmeTemp(L"\\\\.\\PhysicalDrive0");
        }

        // Répartit l'activité I/O sur les disques (simplifié)
        di.readMBs  = rdMBs / std::max(1, (int)disks.size()+1);
        di.writeMBs = wrMBs / std::max(1, (int)disks.size()+1);

        totalWatt += di.estWatt;
        disks.push_back(di);
    }
}

// ─────────────────────────────────────────────────────────────────
//  RÉSEAU DÉTAILLÉ
// ─────────────────────────────────────────────────────────────────
static void enumNetAdapters(std::vector<NetAdapterInfo>& adapters,
                             double& totalWatt,
                             double sumUpKBs, double sumDnKBs) {
    adapters.clear(); totalWatt = 0;

    ULONG bufLen = 15000;
    std::vector<BYTE> buf(bufLen);
    auto* pAdapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX|GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST;

    DWORD ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, pAdapters, &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        pAdapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, pAdapters, &bufLen);
    }
    if (ret != NO_ERROR) return;

    int active = 0;
    for (auto* a = pAdapters; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->OperStatus != IfOperStatusUp) continue;

        NetAdapterInfo ni;
        ni.name   = a->FriendlyName;
        ni.active = true;
        ni.wifi   = (a->IfType == IF_TYPE_IEEE80211);
        ni.estWatt = ni.wifi ? 1.5 : 2.5;

        // MAC
        wchar_t mac[32]{};
        swprintf_s(mac, L"%02X:%02X:%02X:%02X:%02X:%02X",
            a->PhysicalAddress[0],a->PhysicalAddress[1],a->PhysicalAddress[2],
            a->PhysicalAddress[3],a->PhysicalAddress[4],a->PhysicalAddress[5]);
        ni.mac = mac;

        // IPv4
        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                char ip4[16]{};
                auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                sprintf_s(ip4, "%d.%d.%d.%d",
                    sa->sin_addr.S_un.S_un_b.s_b1, sa->sin_addr.S_un.S_un_b.s_b2,
                    sa->sin_addr.S_un.S_un_b.s_b3, sa->sin_addr.S_un.S_un_b.s_b4);
                int sz = MultiByteToWideChar(CP_ACP,0,ip4,-1,nullptr,0);
                std::wstring ws(sz,L'\0');
                MultiByteToWideChar(CP_ACP,0,ip4,-1,ws.data(),sz);
                while (!ws.empty()&&ws.back()==L'\0') ws.pop_back();
                ni.ipv4 = ws;
                break;
            }
        }

        totalWatt += ni.estWatt;
        active++;
        adapters.push_back(ni);
    }

    // Distribue le trafic sur les adaptateurs actifs (approximation)
    if (!adapters.empty()) {
        for (auto& a : adapters) {
            a.upKBs   = sumUpKBs   / adapters.size();
            a.downKBs = sumDnKBs   / adapters.size();
        }
    }
}

// ─────────────────────────────────────────────────────────────────
//  BLUETOOTH
// ─────────────────────────────────────────────────────────────────
static bool detectBluetooth() {
    GUID btGuid = {0xe0cbf06c,0xcd8b,0x4647,{0xbb,0x8a,0x26,0x3b,0x43,0xf0,0xf9,0x74}};
    HDEVINFO h = SetupDiGetClassDevsW(&btGuid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if (h == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA d{}; d.cbSize=sizeof(d);
    bool found = SetupDiEnumDeviceInfo(h,0,&d) != FALSE;
    SetupDiDestroyDeviceInfoList(h);
    return found;
}

// ─────────────────────────────────────────────────────────────────
//  USB
// ─────────────────────────────────────────────────────────────────
static void enumUsb(std::vector<std::pair<std::wstring,double>>& devs) {
    devs.clear();
    HDEVINFO h = SetupDiGetClassDevsW(&GUID_DEVCLASS_USB,nullptr,nullptr,
        DIGCF_PRESENT|DIGCF_ALLCLASSES);
    if (h==INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA d{}; d.cbSize=sizeof(d);
    for (DWORD i=0;SetupDiEnumDeviceInfo(h,i,&d);i++) {
        wchar_t buf[512]{};
        if (!SetupDiGetDeviceRegistryPropertyW(h,&d,SPDRP_DEVICEDESC,
            nullptr,(PBYTE)buf,sizeof(buf)-2,nullptr)) continue;
        std::wstring n(buf);
        if (n.find(L"Root Hub")!=std::wstring::npos ||
            n.find(L"Host Controller")!=std::wstring::npos ||
            n.find(L"Composite")!=std::wstring::npos) continue;
        std::wstring lo=n; for(auto&c:lo) c=towlower(c);
        double w=0.5;
        if(lo.find(L"camera")!=std::wstring::npos||lo.find(L"webcam")!=std::wstring::npos) w=2.5;
        else if(lo.find(L"audio")!=std::wstring::npos||lo.find(L"headset")!=std::wstring::npos) w=1.0;
        else if(lo.find(L"storage")!=std::wstring::npos||lo.find(L"disk")!=std::wstring::npos) w=4.5;
        else if(lo.find(L"hub")!=std::wstring::npos) w=2.5;
        else if(lo.find(L"printer")!=std::wstring::npos) w=3.0;
        else if(lo.find(L"bluetooth")!=std::wstring::npos) w=0.5;
        else if(lo.find(L"keyboard")!=std::wstring::npos||lo.find(L"mouse")!=std::wstring::npos) w=0.5;
        else if(lo.find(L"controller")!=std::wstring::npos||lo.find(L"gamepad")!=std::wstring::npos) w=1.0;
        devs.push_back({n,w});
    }
    SetupDiDestroyDeviceInfoList(h);
}

// ─────────────────────────────────────────────────────────────────
//  MONITEURS
// ─────────────────────────────────────────────────────────────────
struct MfrEntry { const wchar_t* code; const wchar_t* name; };
static const MfrEntry k_MfrTable[] = {
    {L"KOR",L"Koorui"},{L"RTK",L"Koorui/RTK"},{L"KUI",L"Koorui"},
    {L"DEL",L"Dell"},{L"SAM",L"Samsung"},{L"LGD",L"LG"},{L"LEN",L"Lenovo"},
    {L"AUO",L"AU Optronics"},{L"CMN",L"Chimei Innolux"},{L"BOE",L"BOE"},
    {L"SDC",L"Samsung Display"},{L"SHP",L"Sharp"},{L"HWP",L"HP"},
    {L"ACR",L"Acer"},{L"VSC",L"ViewSonic"},{L"BNQ",L"BenQ"},
    {L"ASU",L"ASUS"},{L"MSI",L"MSI"},{L"GSM",L"LG"},{L"PHL",L"Philips"},
    {L"IOD",L"I-O Data"},{L"NEC",L"NEC"},{L"ENC",L"Eizo"},
    {L"AIC",L"AG Neovo"},{L"IVM",L"Iiyama"},{L"HSD",L"Hannstar"},
};
static std::wstring mfrNameFromCode(const std::wstring& code) {
    for (auto& e:k_MfrTable) if(code==e.code) return e.name;
    return code;
}

struct ModelPower { const wchar_t* model; double watt; };
static const ModelPower k_KooruiModels[] = {
    {L"24E4",22.0},{L"24N5A",22.0},{L"24N5B",22.0},{L"24E3",20.0},{L"24N4",20.0},
    {L"27E6",28.0},{L"27E6Q",28.0},{L"27E6QA",28.0},{L"27N5A",26.0},{L"27N5C",26.0},
    {L"27N4",25.0},{L"27E3",24.0},{L"27E4",24.0},{L"32N5A",38.0},{L"32E6",40.0},
    {L"34N5A",55.0},
};
static double kooruiWattFromModel(const std::wstring& m) {
    for (auto& km:k_KooruiModels) {
        std::wstring lo=m,pat=km.model;
        for(auto&c:lo) c=towupper(c); for(auto&c:pat) c=towupper(c);
        if(lo.find(pat)!=std::wstring::npos) return km.watt;
    }
    return -1;
}
static std::wstring edidMfrCode(const BYTE* edid) {
    WORD w=(WORD)((edid[8]<<8)|edid[9]);
    if(w==0||w==0xFFFF) return {};
    wchar_t mfr[4]={(wchar_t)(L'A'+((w>>10)&0x1F)-1),(wchar_t)(L'A'+((w>>5)&0x1F)-1),(wchar_t)(L'A'+((w>>0)&0x1F)-1),0};
    for(int i=0;i<3;i++) if(mfr[i]<L'A'||mfr[i]>L'Z') return {};
    return mfr;
}
static std::wstring edidFriendlyName(const BYTE* edid,int len) {
    if(len<128) return {};
    std::wstring best;
    for(int i=54;i<=108;i+=18) {
        if(edid[i]!=0||edid[i+1]!=0) continue;
        BYTE dt=edid[i+3];
        if(dt!=0xFC&&dt!=0xFE) continue;
        char name[14]{}; memcpy(name,edid+i+5,13);
        for(int j=12;j>=0;j--){if(name[j]=='\n'||name[j]=='\r'||name[j]==' '||name[j]==0)name[j]=0;else break;}
        if(!name[0]) continue;
        int sz=MultiByteToWideChar(CP_UTF8,0,name,-1,nullptr,0);
        std::wstring ws(sz,L'\0');
        MultiByteToWideChar(CP_UTF8,0,name,-1,ws.data(),sz);
        while(!ws.empty()&&ws.back()==L'\0') ws.pop_back();
        if(dt==0xFC) return ws;
        if(best.empty()) best=ws;
    }
    return best;
}
static std::wstring edidProductCode(const BYTE* e){WORD pc=(WORD)(e[10]|(e[11]<<8));wchar_t b[8];swprintf_s(b,L"%04X",pc);return b;}
static double estimateMonitorWatt(const std::wstring& mfr,const std::wstring& model,double diag){
    bool isK=(mfr==L"KOR"||mfr==L"RTK"||mfr==L"KUI");
    if(isK&&!model.empty()){double w=kooruiWattFromModel(model);if(w>0)return w;}
    if(diag>0){double base;if(diag<22)base=16;else if(diag<24)base=20;else if(diag<25)base=22;else if(diag<27)base=25;else if(diag<28)base=28;else if(diag<32)base=32;else if(diag<35)base=40;else base=55;return isK?base*0.85:base;}
    return 25;
}

using MonitorInfo = SensorData::MonitorInfo;
struct EnumMonCtx { std::vector<MonitorInfo>* list; };

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMon,HDC,LPRECT,LPARAM lp){
    auto* ctx=reinterpret_cast<EnumMonCtx*>(lp);
    MONITORINFOEXW mi{}; mi.cbSize=sizeof(mi);
    if(!GetMonitorInfoW(hMon,&mi)) return TRUE;
    MonitorInfo info;
    info.deviceName=mi.szDevice; info.primary=(mi.dwFlags&MONITORINFOF_PRIMARY)!=0;
    DEVMODEW dm{}; dm.dmSize=sizeof(dm);
    if(EnumDisplaySettingsW(mi.szDevice,ENUM_CURRENT_SETTINGS,&dm)){info.widthPx=dm.dmPelsWidth;info.heightPx=dm.dmPelsHeight;info.refreshHz=dm.dmDisplayFrequency;}
    DISPLAY_DEVICEW dd{}; dd.cb=sizeof(dd);
    for(DWORD i=0;EnumDisplayDevicesW(mi.szDevice,i,&dd,0);i++){
        if(!(dd.StateFlags&DISPLAY_DEVICE_ACTIVE)) continue;
        std::wstring devId=dd.DeviceID;
        GUID monGuid={0x4d36e96e,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
        HDEVINFO hDev=SetupDiGetClassDevsW(&monGuid,nullptr,nullptr,DIGCF_PRESENT);
        if(hDev!=INVALID_HANDLE_VALUE){
            SP_DEVINFO_DATA did2{}; did2.cbSize=sizeof(did2);
            for(DWORD j=0;SetupDiEnumDeviceInfo(hDev,j,&did2);j++){
                HKEY hKey=SetupDiOpenDevRegKey(hDev,&did2,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_READ);
                if(hKey==INVALID_HANDLE_VALUE) continue;
                DWORD type2=0,sz2=0;
                if(RegQueryValueExW(hKey,L"EDID",nullptr,&type2,nullptr,&sz2)==ERROR_SUCCESS&&sz2>=128){
                    std::vector<BYTE> edid(sz2);
                    RegQueryValueExW(hKey,L"EDID",nullptr,&type2,edid.data(),&sz2);
                    RegCloseKey(hKey);
                    info.widthMm=edid[21]; info.heightMm=edid[22];
                    if(info.widthMm>0&&info.heightMm>0){double dm2=sqrt((double)info.widthMm*info.widthMm+(double)info.heightMm*info.heightMm);info.diagInch=dm2/25.4;}
                    info.edidMfr=edidMfrCode(edid.data());
                    info.friendlyName=edidFriendlyName(edid.data(),(int)sz2);
                    if(info.friendlyName.empty()&&!info.edidMfr.empty()){std::wstring brand=mfrNameFromCode(info.edidMfr);std::wstring pc=edidProductCode(edid.data());info.friendlyName=brand+L" "+pc;}
                    break;
                }
                RegCloseKey(hKey);
            }
            SetupDiDestroyDeviceInfoList(hDev);
        }
        break;
    }
    if(info.friendlyName.empty()){DISPLAY_DEVICEW dd2{};dd2.cb=sizeof(dd2);if(EnumDisplayDevicesW(mi.szDevice,0,&dd2,0)&&dd2.DeviceString[0]){info.friendlyName=dd2.DeviceString;std::wstring lo=info.friendlyName;for(auto&c:lo)c=towlower(c);if(lo.find(L"koorui")!=std::wstring::npos&&info.edidMfr.empty())info.edidMfr=L"KOR";}else info.friendlyName=mi.szDevice;}
    if(!info.edidMfr.empty()) info.edidMfr=mfrNameFromCode(info.edidMfr);
    info.estWattW=estimateMonitorWatt(info.edidMfr,info.friendlyName,info.diagInch);
    ctx->list->push_back(info);
    return TRUE;
}
static void enumMonitors(std::vector<MonitorInfo>& monitors,int& cnt,double& totalW){
    monitors.clear(); EnumMonCtx ctx{&monitors};
    EnumDisplayMonitors(nullptr,nullptr,MonitorEnumProc,(LPARAM)&ctx);
    cnt=(int)monitors.size(); totalW=0;
    for(auto&m:monitors) totalW+=m.estWattW;
}

// ─────────────────────────────────────────────────────────────────
//  ADL2 — AMD GPU  (FIX v3.0 : Tj index 80, struct corrigée)
// ─────────────────────────────────────────────────────────────────
typedef void* ADL_CONTEXT_HANDLE;

struct AdlAdapterInfo {
    int  iSize; int iAdapterIndex; char strUDID[256];
    int  iBusNumber; int iDeviceNumber; int iFunctionNumber; int iVendorID;
    char strAdapterName[256]; char strDisplayName[256];
    int  iPresent; int iExist;
    char strDriverPath[256]; char strDriverPathExt[256];
    char strPNPString[256]; int iOSDisplayIndex;
};

// ─────────────────────────────────────────────────────────────────
//  IDENTIFIANTS ADL PMLOG CORRECTS (adl_defines.h AMD SDK v17+)
// ─────────────────────────────────────────────────────────────────
#define ADL_PMLOG_ASIC_POWER           1
#define ADL_PMLOG_CLK_GFXCLK          13
#define ADL_PMLOG_TEMPERATURE_EDGE     7   // Die edge — PAS la Tj
#define ADL_PMLOG_TEMPERATURE_MEM      8
#define ADL_PMLOG_TEMPERATURE_VRVDDC   9
#define ADL_PMLOG_TEMPERATURE_VRMVDD  10
#define ADL_PMLOG_TEMPERATURE_LIQUID  11
#define ADL_PMLOG_TEMPERATURE_PLX     12
// Hotspot / Junction (RX 5000+ RDNA) — C'EST la vraie Tj max
#define ADL_PMLOG_TEMPERATURE_HOTSPOT 80
#define ADL_PMLOG_MAX_SENSORS        256

// STRUCTURE CORRIGÉE : ulReserved[16] exactement comme dans le vrai SDK AMD
// L'alignement incorrect causait un décalage dans ulValues et donc une Tj x2
struct AdlPmLogData {
    unsigned int ulVersion;
    unsigned int ulActiveSampleRate;
    long long    ulLastUpdated;
    unsigned int ulReserved[16];                      // ← 16 exactement (SDK AMD v17)
    unsigned int ulValues[ADL_PMLOG_MAX_SENSORS][2];  // [i][0]=valeur, [i][1]=valide
};

struct AdlPmLogSupportList {
    unsigned int uSupported[ADL_PMLOG_MAX_SENSORS];
};

typedef int (__stdcall *ADL2_Main_Control_Create_t)(void*(*)(int),int,ADL_CONTEXT_HANDLE*);
typedef int (__stdcall *ADL2_Main_Control_Destroy_t)(ADL_CONTEXT_HANDLE);
typedef int (__stdcall *ADL2_Adapter_NumberOfAdapters_Get_t)(ADL_CONTEXT_HANDLE,int*);
typedef int (__stdcall *ADL2_Adapter_AdapterInfo_Get_t)(ADL_CONTEXT_HANDLE,AdlAdapterInfo*,int);
typedef int (__stdcall *ADL2_Adapter_Active_Get_t)(ADL_CONTEXT_HANDLE,int,int*);
typedef int (__stdcall *ADL2_New_QueryPMLogData_Get_t)(ADL_CONTEXT_HANDLE,int,AdlPmLogData*);
typedef int (__stdcall *ADL2_Overdrive6_CurrentPower_Get_t)(ADL_CONTEXT_HANDLE,int,int,int*);
// Températures Overdrive5 (GPU plus anciens)
typedef int (__stdcall *ADL2_Overdrive5_Temperature_Get_t)(ADL_CONTEXT_HANDLE,int,int,void*);

static void* __stdcall AdlAlloc(int size) { return malloc(size); }

struct AdlCtx {
    HMODULE   dll       = nullptr;
    ADL_CONTEXT_HANDLE ctx = nullptr;
    int       adapterIdx = -1;
    bool      ready     = false;
    bool      usePmLog  = false;

    ADL2_Main_Control_Create_t          pfCreate   = nullptr;
    ADL2_Main_Control_Destroy_t         pfDestroy  = nullptr;
    ADL2_Adapter_NumberOfAdapters_Get_t pfNumAdapt = nullptr;
    ADL2_Adapter_AdapterInfo_Get_t      pfAdaptInfo= nullptr;
    ADL2_Adapter_Active_Get_t           pfActive   = nullptr;
    ADL2_New_QueryPMLogData_Get_t       pfPmLog    = nullptr;
    ADL2_Overdrive6_CurrentPower_Get_t  pfOD6Pwr   = nullptr;

    bool init() {
        const wchar_t* paths[] = {
            L"atiadlxx.dll",
            L"C:\\Windows\\System32\\atiadlxx.dll",
            L"C:\\Program Files\\AMD\\CNext\\CNext\\atiadlxx.dll",
            L"C:\\Program Files (x86)\\AMD\\CNext\\CNext\\atiadlxx.dll",
        };
        for (auto p:paths) { dll=LoadLibraryW(p); if(dll) break; }
        if (!dll) return false;

        pfCreate    =(ADL2_Main_Control_Create_t)         GetProcAddress(dll,"ADL2_Main_Control_Create");
        pfDestroy   =(ADL2_Main_Control_Destroy_t)        GetProcAddress(dll,"ADL2_Main_Control_Destroy");
        pfNumAdapt  =(ADL2_Adapter_NumberOfAdapters_Get_t)GetProcAddress(dll,"ADL2_Adapter_NumberOfAdapters_Get");
        pfAdaptInfo =(ADL2_Adapter_AdapterInfo_Get_t)     GetProcAddress(dll,"ADL2_Adapter_AdapterInfo_Get");
        pfActive    =(ADL2_Adapter_Active_Get_t)          GetProcAddress(dll,"ADL2_Adapter_Active_Get");
        pfPmLog     =(ADL2_New_QueryPMLogData_Get_t)      GetProcAddress(dll,"ADL2_New_QueryPMLogData_Get");
        pfOD6Pwr    =(ADL2_Overdrive6_CurrentPower_Get_t) GetProcAddress(dll,"ADL2_Overdrive6_CurrentPower_Get");

        if (!pfCreate||!pfDestroy||!pfNumAdapt||!pfAdaptInfo) return false;
        if (pfCreate(AdlAlloc,1,&ctx)!=0) return false;

        int numAdapt=0; pfNumAdapt(ctx,&numAdapt);
        if (numAdapt<=0) return false;
        std::vector<AdlAdapterInfo> infos(numAdapt);
        infos[0].iSize=sizeof(AdlAdapterInfo);
        pfAdaptInfo(ctx,infos.data(),(int)(sizeof(AdlAdapterInfo)*numAdapt));

        for (int i=0;i<numAdapt;i++){
            int active=0;
            if(pfActive&&pfActive(ctx,i,&active)==0&&active&&infos[i].iVendorID==0x1002){adapterIdx=i;break;}
        }
        if (adapterIdx<0){for(int i=0;i<numAdapt;i++) if(infos[i].iVendorID==0x1002){adapterIdx=i;break;}}
        if (adapterIdx<0) return false;

        if (pfPmLog){
            AdlPmLogData tmp{};
            tmp.ulVersion=sizeof(AdlPmLogData);
            usePmLog=(pfPmLog(ctx,adapterIdx,&tmp)==0);
        }
        ready=true; return true;
    }

    // Puissance GPU en W
    double readPowerW() {
        if (!ready||adapterIdx<0) return -1;
        if (usePmLog&&pfPmLog){
            AdlPmLogData d{}; d.ulVersion=sizeof(AdlPmLogData);
            if (pfPmLog(ctx,adapterIdx,&d)==0&&d.ulValues[ADL_PMLOG_ASIC_POWER][1]==1)
                return (double)d.ulValues[ADL_PMLOG_ASIC_POWER][0];
        }
        if (pfOD6Pwr){
            int val=0;
            if (pfOD6Pwr(ctx,adapterIdx,0,&val)==0&&val>0) return val/8.0;
        }
        return -1;
    }

    // Température GPU — FIX : utilise ADL_PMLOG_TEMPERATURE_HOTSPOT (80) = vraie Tj
    // EDGE (7) = température bord de die, souvent 10-20°C en dessous de la Tj
    double readTempC() {
        if (!ready||adapterIdx<0) return -1;
        if (usePmLog&&pfPmLog){
            AdlPmLogData d{}; d.ulVersion=sizeof(AdlPmLogData);
            if (pfPmLog(ctx,adapterIdx,&d)==0){
                // Priorité 1 : Hotspot / Junction (Tj vraie, RX5000+/RDNA)
                if (d.ulValues[ADL_PMLOG_TEMPERATURE_HOTSPOT][1]==1){
                    double t=(double)d.ulValues[ADL_PMLOG_TEMPERATURE_HOTSPOT][0];
                    if (t>0&&t<125) return t;  // sanity check °C
                }
                // Priorité 2 : Edge (GPU plus anciens sans Hotspot)
                if (d.ulValues[ADL_PMLOG_TEMPERATURE_EDGE][1]==1){
                    double t=(double)d.ulValues[ADL_PMLOG_TEMPERATURE_EDGE][0];
                    if (t>0&&t<105) return t;
                }
            }
        }
        return -1;
    }

    ~AdlCtx(){ if(ctx&&pfDestroy) pfDestroy(ctx); if(dll) FreeLibrary(dll); }
};
static AdlCtx g_Adl;

// ─────────────────────────────────────────────────────────────────
//  THREAD CAPTEURS
// ─────────────────────────────────────────────────────────────────
static void pushHist(std::deque<double>& h, double v){
    h.push_back(v);
    if ((int)h.size()>HIST) h.pop_front();
}

static void sensorThread() {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    PdhCtx pdh; pdh.init();
    Sleep(1000);

    bool wmiOk = initWmi();

    // CPU statique
    std::wstring cpuName=wmiGetStr(g_WmiSvc,L"Win32_Processor",L"Name");
    { std::wstring tmp; bool sp=false;
      for(wchar_t c:cpuName){if(c==L' '){if(!sp){tmp+=c;sp=true;}}else{tmp+=c;sp=false;}}
      while(!tmp.empty()&&tmp.back()==L' ')tmp.pop_back(); cpuName=tmp; }
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    int cores=(int)si.dwNumberOfProcessors;

    // GPU statique
    std::wstring gpuNameDxgi; size_t vramBytes=0;
    dxgiGetGpuInfo(gpuNameDxgi,vramBytes);
    std::wstring gpuNameWmi=wmiGetStr(g_WmiSvc,L"Win32_VideoController",L"Name");
    std::wstring gpuName=gpuNameDxgi.empty()?gpuNameWmi:gpuNameDxgi;

    // Audio (statique)
    AudioInfo audio;
    wmiGetAudio(audio);

    // Bluetooth (statique)
    bool btPresent=detectBluetooth();

    // ADL
    bool adlOk=g_Adl.init();

    double cumWh=0;
    auto t0=std::chrono::steady_clock::now();
    int usbRefresh=0, diskRefresh=0, netRefresh=0, fanRefresh=0;
    std::vector<std::pair<std::wstring,double>> usbDevs;
    std::vector<DiskInfo> diskInfos;
    std::vector<NetAdapterInfo> netAdapters;
    std::vector<FanInfo> fans;
    double diskTotalWatt=0, netTotalWatt=0;
    enumUsb(usbDevs);

    while(g_Running){
        PdhCollectQueryData(pdh.q);
        double cpuLoad  = pdh.readScalar(pdh.cCpuLoad);
        double gpuLoad  = pdh.readGpuLoad();
        double diskRd   = pdh.readScalar(pdh.cDiskRead)  /(1024.0*1024.0);
        double diskWr   = pdh.readScalar(pdh.cDiskWrite) /(1024.0*1024.0);
        double netUp    = pdh.readNetSum(pdh.cNetSent)   /1024.0;
        double netDn    = pdh.readNetSum(pdh.cNetRecv)   /1024.0;

        double cpuFreq  = wmiGetCpuFreq();
        double cpuTempC = wmiGetCpuTemp();

        VoltageInfo voltages;
        wmiGetVoltages(voltages);

        // RAM
        MEMORYSTATUSEX ms{}; ms.dwLength=sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        double ramTotal=(double)ms.ullTotalPhys/(1<<30);
        double ramUsed =ramTotal-(double)ms.ullAvailPhys/(1<<30);
        double ramPct  =ms.dwMemoryLoad;
        double ramWatt =(ramTotal/8.0)*3.5;

        // Batterie
        SYSTEM_POWER_STATUS pwr{}; GetSystemPowerStatus(&pwr);
        bool onBat=(pwr.ACLineStatus==0);
        int battPct=(pwr.BatteryLifePercent==255)?-1:(int)pwr.BatteryLifePercent;
        bool charg=(pwr.BatteryFlag&0x08)!=0;

        // CPU watts
        double cpuW=g_CpuTdpIdle+(g_CpuTdpMax-g_CpuTdpIdle)*(cpuLoad/100.0);

        // GPU watts + temp (ADL)
        double adlW    = g_Adl.readPowerW();
        double adlTemp = g_Adl.readTempC();    // FIX : Tj correcte
        double gpuW    = (adlW>=0.0)?adlW:g_GpuTdpIdle+(g_GpuTdpMax-g_GpuTdpIdle)*(gpuLoad/100.0);
        SensorData::GpuSrc gpuSrc = (adlW>=0.0)?SensorData::GpuSrc::ADL:SensorData::GpuSrc::ESTIMATE;

        // Chipset / VRM (estimation fixe)
        double chipsetW=5.0, vrmW=3.0;

        // USB
        if(++usbRefresh>=15){ enumUsb(usbDevs); usbRefresh=0; }
        double usbW=0; for(auto&d:usbDevs) usbW+=d.second;

        // Disques (refresh toutes les 10s)
        if(++diskRefresh>=10){
            enumDisks(diskInfos,diskTotalWatt,diskRd,diskWr);
            diskRefresh=0;
        }

        // Réseau détaillé (refresh toutes les 5s)
        if(++netRefresh>=5){
            enumNetAdapters(netAdapters,netTotalWatt,netUp,netDn);
            netRefresh=0;
        } else {
            for(auto& a:netAdapters){a.upKBs=netUp/std::max(1,(int)netAdapters.size());a.downKBs=netDn/std::max(1,(int)netAdapters.size());}
        }

        // Ventilateurs (refresh toutes les 5s)
        if(++fanRefresh>=5){
            wmiGetFans(fans);
            fanRefresh=0;
        }
        double fanTotalW=0; for(auto&f:fans) fanTotalW+=f.watt;

        // Écrans
        int monCnt=0; double monW=0;
        std::vector<MonitorInfo> monList;
        enumMonitors(monList,monCnt,monW);

        // Bluetooth
        double btW = btPresent ? 0.5 : 0.0;

        // Audio
        double audioW = audio.present ? audio.estWatt : 0.0;

        // Total
        double total=cpuW+gpuW+ramWatt+usbW+monW+g_SysBase
                    +diskTotalWatt+netTotalWatt+fanTotalW
                    +chipsetW+vrmW+btW+audioW;
        cumWh+=total/3600.0;

        auto now=std::chrono::steady_clock::now();
        int64_t sec=std::chrono::duration_cast<std::chrono::seconds>(now-t0).count();

        {
            std::lock_guard<std::mutex> lk(g_Mtx);
            g_Sensor.cpuLoad    = cpuLoad;
            g_Sensor.cpuWatt    = cpuW;
            g_Sensor.cpuTempC   = cpuTempC;
            g_Sensor.cpuFreqMhz = cpuFreq;
            g_Sensor.cpuName    = cpuName;
            g_Sensor.cpuCores   = cores;
            g_Sensor.voltages   = voltages;

            g_Sensor.gpuLoad      = gpuLoad;
            g_Sensor.gpuWatt      = gpuW;
            g_Sensor.gpuTjC       = adlTemp;   // Tj correcte (index 80)
            g_Sensor.gpuTempC     = adlTemp;
            g_Sensor.gpuWattReal  = (adlW>=0.0);
            g_Sensor.gpuTempReal  = (adlTemp>=0.0);
            g_Sensor.gpuSrc       = gpuSrc;
            g_Sensor.gpuName      = gpuName;
            g_Sensor.gpuVramBytes = vramBytes;
            g_Sensor.gpuMemTotalMB= (double)vramBytes/(1024*1024);

            g_Sensor.ramUsedGB  = ramUsed;
            g_Sensor.ramTotalGB = ramTotal;
            g_Sensor.ramPct     = ramPct;
            g_Sensor.ramWatt    = ramWatt;

            g_Sensor.disks          = diskInfos;
            g_Sensor.diskTotalWatt  = diskTotalWatt;

            g_Sensor.netAdapters        = netAdapters;
            g_Sensor.netTotalUpKBs      = netUp;
            g_Sensor.netTotalDownKBs    = netDn;
            g_Sensor.netTotalWatt       = netTotalWatt;

            g_Sensor.audio      = audio;
            g_Sensor.fans       = fans;
            g_Sensor.fanTotalWatt=fanTotalW;
            g_Sensor.fanCount   =(int)fans.size();
            g_Sensor.chipsetWatt= chipsetW;
            g_Sensor.vrmWatt    = vrmW;
            g_Sensor.btPresent  = btPresent;
            g_Sensor.btWatt     = btW;

            g_Sensor.onBattery  = onBat;
            g_Sensor.battPct    = battPct;
            g_Sensor.charging   = charg;
            g_Sensor.usbDevices = usbDevs;
            g_Sensor.usbCount   = (int)usbDevs.size();
            g_Sensor.usbWatt    = usbW;
            g_Sensor.monitorCount = monCnt;
            g_Sensor.monitorWatt  = monW;
            g_Sensor.monitors     = monList;
            g_Sensor.totalWatt  = total;
            g_Sensor.cumWhSession= cumWh;
            g_Sensor.sessionSec = sec;

            pushHist(g_History,   total);
            pushHist(g_CpuHistory,cpuW);
            pushHist(g_GpuHistory,gpuW);
        }
        Sleep(1000);
    }
}

// ─────────────────────────────────────────────────────────────────
//  DIRECT2D
// ─────────────────────────────────────────────────────────────────
struct D2DCtx {
    ID2D1Factory*          factory  = nullptr;
    ID2D1HwndRenderTarget* rt       = nullptr;
    IDWriteFactory*        dwrite   = nullptr;
    IDWriteTextFormat*     fmtBig   = nullptr;
    IDWriteTextFormat*     fmtMed   = nullptr;
    IDWriteTextFormat*     fmtSmall = nullptr;
    IDWriteTextFormat*     fmtMono  = nullptr;
    IDWriteTextFormat*     fmtTiny  = nullptr;
    std::map<UINT32,ID2D1SolidColorBrush*> brushes;

    bool init(HWND hw){
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&factory);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),(IUnknown**)&dwrite);
        RECT rc; GetClientRect(hw,&rc);
        D2D1_SIZE_U sz=D2D1::SizeU(rc.right,rc.bottom);
        factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hw,sz),&rt);
        dwrite->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,28,L"fr-FR",&fmtBig);
        dwrite->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,13,L"fr-FR",&fmtMed);
        dwrite->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,11,L"fr-FR",&fmtSmall);
        dwrite->CreateTextFormat(L"Consolas",nullptr,DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,11,L"fr-FR",&fmtMono);
        dwrite->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,10,L"fr-FR",&fmtTiny);
        return rt!=nullptr;
    }
    ID2D1SolidColorBrush* brush(D2D1_COLOR_F c){
        UINT32 key=((UINT32)(c.r*255)<<16)|((UINT32)(c.g*255)<<8)|(UINT32)(c.b*255);
        auto it=brushes.find(key);
        if(it!=brushes.end()) return it->second;
        ID2D1SolidColorBrush* b=nullptr; rt->CreateSolidColorBrush(c,&b);
        brushes[key]=b; return b;
    }
    void resize(HWND hw){ RECT rc; GetClientRect(hw,&rc); if(rt) rt->Resize(D2D1::SizeU(rc.right,rc.bottom)); }
    ~D2DCtx(){
        for(auto&p:brushes) if(p.second) p.second->Release();
        if(fmtTiny)  fmtTiny->Release();
        if(fmtMono)  fmtMono->Release();
        if(fmtSmall) fmtSmall->Release();
        if(fmtMed)   fmtMed->Release();
        if(fmtBig)   fmtBig->Release();
        if(dwrite)   dwrite->Release();
        if(rt)       rt->Release();
        if(factory)  factory->Release();
    }
};
static D2DCtx g_D2D;

static void DText(const wchar_t* t, D2D1_RECT_F r, ID2D1SolidColorBrush* b,
                  IDWriteTextFormat* f, bool center=false){
    if(!t||!b||!f) return;
    f->SetTextAlignment(center?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_LEADING);
    f->SetParagraphAlignment(center?DWRITE_PARAGRAPH_ALIGNMENT_CENTER:DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    g_D2D.rt->DrawText(t,(UINT32)wcslen(t),f,r,b);
}
static void FillRoundRect(D2D1_RECT_F r,float rad,D2D1_COLOR_F col){
    g_D2D.rt->FillRoundedRectangle(D2D1::RoundedRect(r,rad,rad),g_D2D.brush(col));
}
static void DrawRoundRect(D2D1_RECT_F r,float rad,D2D1_COLOR_F col,float sw=1.0f){
    g_D2D.rt->DrawRoundedRectangle(D2D1::RoundedRect(r,rad,rad),g_D2D.brush(col),sw);
}
static void DrawBar(float x,float y,float w,float h,double pct,D2D1_COLOR_F fillCol){
    FillRoundRect({x,y,x+w,y+h},3,C::BORDER);
    if(pct>0) FillRoundRect({x,y,x+(float)(w*pct/100.0),y+h},3,fillCol);
}
static void DrawGraph(float x,float y,float w,float h,
                      const std::deque<double>& hist,
                      double maxW,D2D1_COLOR_F lineCol){
    FillRoundRect({x,y,x+w,y+h},6,{0.035f,0.043f,0.067f,1});
    if(hist.size()<2) return;
    auto bMuted=g_D2D.brush(C::BORDER);
    for(int i=1;i<4;i++){
        float gy=y+h*(1.0f-i/4.0f);
        g_D2D.rt->DrawLine({x+8,gy},{x+w-4,gy},bMuted,0.5f);
        wchar_t buf[16]; swprintf_s(buf,L"%.0f",maxW*i/4.0);
        DText(buf,{x+4,gy-8,x+40,gy+8},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
    }
    int n=(int)hist.size();
    float stepX=w/(float)(HIST-1);
    float offX =x+(HIST-n)*stepX;
    ID2D1PathGeometry* geo=nullptr; g_D2D.factory->CreatePathGeometry(&geo);
    ID2D1GeometrySink* sink=nullptr; geo->Open(&sink);
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    float fy0=y+h-(float)std::clamp(hist[0]/maxW,0.0,1.0)*(h-4)-2;
    sink->BeginFigure({offX,fy0},D2D1_FIGURE_BEGIN_FILLED);
    for(int i=1;i<n;i++){
        float px=offX+i*stepX;
        float py=y+h-(float)std::clamp(hist[i]/maxW,0.0,1.0)*(h-4)-2;
        sink->AddLine({px,py});
    }
    sink->AddLine({offX+(n-1)*stepX,y+h}); sink->AddLine({offX,y+h});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close(); sink->Release();
    D2D1_COLOR_F fa={lineCol.r,lineCol.g,lineCol.b,0.15f};
    g_D2D.rt->FillGeometry(geo,g_D2D.brush(fa));
    g_D2D.rt->DrawGeometry(geo,g_D2D.brush(lineCol),1.8f);
    if(n>0){
        float px=offX+(n-1)*stepX;
        float py=y+h-(float)std::clamp(hist[n-1]/maxW,0.0,1.0)*(h-4)-2;
        D2D1_ELLIPSE el=D2D1::Ellipse({px,py},4,4);
        g_D2D.rt->FillEllipse(el,g_D2D.brush(lineCol));
        g_D2D.rt->DrawEllipse(el,g_D2D.brush(C::BG),1.5f);
    }
    geo->Release();
}

// Jauge de température en arc
static void DrawTempGauge(float cx,float cy,float r,double tempC,double maxT,
                           D2D1_COLOR_F col,const wchar_t* label){
    // Arc de fond (gris)
    // Simplifié : rectangle avec barre colorée
    float bx=cx-r,by=cy;
    float bw=r*2,bh=10;
    FillRoundRect({bx,by,bx+bw,by+bh},4,C::BORDER);
    double pct=(tempC<0)?0:std::clamp(tempC/maxT,0.0,1.0);
    if(pct>0) FillRoundRect({bx,by,bx+(float)(bw*pct),by+bh},4,col);
    wchar_t buf[16];
    if(tempC<0) wcscpy_s(buf,L"N/A");
    else swprintf_s(buf,L"%.0f°C",tempC);
    DText(buf,{cx-r,cy-16,cx+r,cy-2},g_D2D.brush(col),g_D2D.fmtSmall,true);
    DText(label,{cx-r,cy+12,cx+r,cy+24},g_D2D.brush(C::DIMTEXT),g_D2D.fmtTiny,true);
}

struct Card {
    float x,y,w,h;
    void draw(const wchar_t* label,const wchar_t* value,const wchar_t* unit,
              const wchar_t* sub,double barPct,D2D1_COLOR_F accent) const {
        FillRoundRect({x,y,x+w,y+h},8,C::PANEL);
        FillRoundRect({x+1,y,x+w-1,y+3},2,accent);
        float px=x+14,py=y+10;
        DText(label,{px,py,x+w-8,py+16},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        py+=20;
        DText(value,{px,py,x+w-50,py+36},g_D2D.brush(accent),g_D2D.fmtBig);
        DText(unit, {x+w-60,py+14,x+w-6,py+36},g_D2D.brush(C::MUTED),g_D2D.fmtMed);
        py+=42;
        if(barPct>=0) DrawBar(px,py,w-28,6,std::clamp(barPct,0.0,100.0),accent);
        py+=14;
        DText(sub,{px,py,x+w-8,py+20},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);
    }
};

// ─────────────────────────────────────────────────────────────────
//  SCROLL
// ─────────────────────────────────────────────────────────────────
static int  g_ScrollY  = 0;
static int  g_ContentH = 0;
static HWND g_Hwnd     = nullptr;

// ─────────────────────────────────────────────────────────────────
//  RENDER
// ─────────────────────────────────────────────────────────────────
static void Render(HWND hw){
    if(!g_D2D.rt) return;
    g_D2D.rt->BeginDraw();
    g_D2D.rt->Clear(C::BG);

    SensorData s;
    std::deque<double> hist,histCpu,histGpu;
    { std::lock_guard<std::mutex> lk(g_Mtx); s=g_Sensor; hist=g_History; histCpu=g_CpuHistory; histGpu=g_GpuHistory; }

    RECT wrc; GetClientRect(hw,&wrc);
    float W=(float)wrc.right, H=(float)wrc.bottom;
    float oy=(float)-g_ScrollY;
    float pad=12, cw=(W-pad*3)/2;

    // ── HEADER ──────────────────────────────────────────────────
    {
        FillRoundRect({0,oy,W,oy+50},0,{0.035f,0.043f,0.067f,1});
        DText(L"⚡ POWERMON v3.0",{14,oy+8,220,oy+42},g_D2D.brush(C::CYAN),g_D2D.fmtMed);
        SYSTEMTIME st{}; GetLocalTime(&st);
        wchar_t tbuf[32]; swprintf_s(tbuf,L"%02d:%02d:%02d",st.wHour,st.wMinute,st.wSecond);
        DText(tbuf,{W-110,oy+8,W-8,oy+42},g_D2D.brush(C::MUTED),g_D2D.fmtMed);
        wchar_t sbuf[64]; int64_t sec=s.sessionSec;
        swprintf_s(sbuf,L"Session  %02lld:%02lld:%02lld",sec/3600,(sec%3600)/60,sec%60);
        DText(sbuf,{220,oy+14,W-120,oy+36},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        if(s.battPct>=0){ wchar_t bb[32]; swprintf_s(bb,L"%s %d%%",s.charging?L"🔌":L"🔋",(int)s.battPct);
            DText(bb,{W-240,oy+8,W-110,oy+42},g_D2D.brush(s.charging?C::GREEN:C::ORANGE),g_D2D.fmtSmall);
        } else DText(L"🔌 Secteur",{W-240,oy+8,W-110,oy+42},g_D2D.brush(C::GREEN),g_D2D.fmtSmall);
        g_D2D.rt->DrawLine({0,oy+50},{W,oy+50},g_D2D.brush(C::BORDER),1);
    }

    float curY=oy+60;

    // ── PUISSANCE TOTALE ─────────────────────────────────────────
    {
        FillRoundRect({pad,curY,W-pad,curY+100},8,C::PANEL);
        D2D1_COLOR_F col=s.totalWatt>250?C::RED:s.totalWatt>150?C::ORANGE:C::CYAN;
        FillRoundRect({pad+1,curY,W-pad-1,curY+3},2,col);
        DText(L"PUISSANCE TOTALE ESTIMÉE",{pad+14,curY+8,W/2,curY+24},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        wchar_t wbuf[32]; swprintf_s(wbuf,L"%.1f",s.totalWatt);
        DText(wbuf,{pad+14,curY+20,pad+200,curY+80},g_D2D.brush(col),g_D2D.fmtBig);
        DText(L"W",{pad+180,curY+50,pad+220,curY+80},g_D2D.brush(C::MUTED),g_D2D.fmtMed);
        double mn=hist.empty()?0:*std::min_element(hist.begin(),hist.end());
        double mx=hist.empty()?0:*std::max_element(hist.begin(),hist.end());
        double av=hist.empty()?s.totalWatt:std::accumulate(hist.begin(),hist.end(),0.0)/hist.size();
        wchar_t sbb[128]; swprintf_s(sbb,L"Min %.0fW   Moy %.0fW   Max %.0fW",mn,av,mx);
        DText(sbb,{pad+14,curY+78,W/2,curY+96},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);

        // Répartition détaillée droite
        float rx=W/2+20,ry=curY+8,rw=W/2-pad-30;
        auto drawSeg=[&](const wchar_t* lbl,double watt,D2D1_COLOR_F c,float& yy){
            DText(lbl,{rx,yy,rx+140,yy+13},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
            wchar_t tmp[16]; swprintf_s(tmp,L"%.1fW",watt);
            DText(tmp,{rx+rw-50,yy,rx+rw,yy+13},g_D2D.brush(c),g_D2D.fmtSmall);
            DrawBar(rx,yy+14,rw,3,s.totalWatt>0?watt/s.totalWatt*100:0,c);
            yy+=22;
        };
        float yy=ry;
        drawSeg(L"CPU",       s.cpuWatt,      C::ORANGE, yy);
        drawSeg(L"GPU",       s.gpuWatt,      C::CYAN,   yy);
        drawSeg(L"RAM",       s.ramWatt,      C::GREEN,  yy);
        drawSeg(L"Stockage",  s.diskTotalWatt,C::YELLOW, yy);
        drawSeg(L"Réseau",    s.netTotalWatt, C::PURPLE, yy);
    }
    curY+=108;

    // ── GRAPHE TOTAL + CPU + GPU ─────────────────────────────────
    {
        float gh=100;
        // Graphe total (fond)
        DrawGraph(pad,curY,W-pad*2,gh,hist,std::max(400.0,s.totalWatt*1.3),C::CYAN);
        // Overlay CPU
        // (simplifié : on draw un mini graphe CPU à droite)
        wchar_t xl[32]; swprintf_s(xl,L"← %d s",HIST);
        DText(xl,{pad+6,curY+gh-14,pad+80,curY+gh-2},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        DText(L"maintenant →",{W-pad-90,curY+gh-14,W-pad,curY+gh-2},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        curY+=gh+6;
    }

    // ── GRAPHES CPU & GPU côte à côte ────────────────────────────
    {
        float gh=70;
        float hw2=(W-pad*3)/2;
        DrawGraph(pad,curY,hw2,gh,histCpu,std::max(g_CpuTdpMax*1.1,s.cpuWatt*1.2),C::ORANGE);
        DText(L"CPU W",{pad+8,curY+4,pad+80,curY+16},g_D2D.brush(C::ORANGE),g_D2D.fmtSmall);
        DrawGraph(pad*2+hw2,curY,hw2,gh,histGpu,std::max(g_GpuTdpMax*1.1,s.gpuWatt*1.2),C::CYAN);
        DText(L"GPU W",{pad*2+hw2+8,curY+4,pad*2+hw2+80,curY+16},g_D2D.brush(C::CYAN),g_D2D.fmtSmall);
        curY+=gh+10;
    }

    // ── SECTION TEMPÉRATURES ─────────────────────────────────────
    {
        float th=80;
        FillRoundRect({pad,curY,W-pad,curY+th},8,C::PANEL2);
        FillRoundRect({pad+1,curY,W-pad-1,curY+3},2,C::RED);
        DText(L"TEMPÉRATURES",{pad+14,curY+6,W/2,curY+22},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);

        float gw=(W-pad*2-28)/5.0f;
        float gx=pad+14,gy=curY+22;

        DrawTempGauge(gx+gw*0.5f,gy,gw*0.45f,s.cpuTempC,100,C::tempColor(s.cpuTempC),L"CPU");
        DrawTempGauge(gx+gw*1.5f,gy,gw*0.45f,s.gpuTjC,   110,C::tempColor(s.gpuTjC),  L"GPU Tj");

        // Température disque NVMe (premier disque)
        double nvmeTmp=-1;
        if (!s.disks.empty()) nvmeTmp=s.disks[0].tempC;
        DrawTempGauge(gx+gw*2.5f,gy,gw*0.45f,nvmeTmp,75,C::tempColor(nvmeTmp),L"NVMe");

        // VRM estimé (pas de capteur direct sans WinRing0, on affiche la puissance)
        wchar_t vrmBuf[16]; swprintf_s(vrmBuf,L"%.0fW",s.vrmWatt);
        float vx=gx+gw*3.0f;
        DText(L"VRM",  {vx,gy-16,vx+gw,gy-4}, g_D2D.brush(C::DIMTEXT),g_D2D.fmtTiny, true);
        DText(vrmBuf,  {vx,gy,    vx+gw,gy+14},g_D2D.brush(C::TEAL),   g_D2D.fmtSmall,true);
        DText(L"estimé",{vx,gy+14,vx+gw,gy+24},g_D2D.brush(C::MUTED),  g_D2D.fmtTiny, true);

        // Chipset
        wchar_t chBuf[16]; swprintf_s(chBuf,L"%.0fW",s.chipsetWatt);
        float cx2=gx+gw*4.0f;
        DText(L"Chipset",{cx2,gy-16,cx2+gw,gy-4},g_D2D.brush(C::DIMTEXT),g_D2D.fmtTiny, true);
        DText(chBuf,     {cx2,gy,    cx2+gw,gy+14},g_D2D.brush(C::LBLUE), g_D2D.fmtSmall,true);
        DText(L"estimé", {cx2,gy+14, cx2+gw,gy+24},g_D2D.brush(C::MUTED), g_D2D.fmtTiny, true);

        curY+=th+8;
    }

    // ── CPU ──────────────────────────────────────────────────────
    {
        Card c={pad,curY,cw,115};
        wchar_t v[16],sub[160];
        swprintf_s(v,L"%.1f",s.cpuWatt);
        // Tension si dispo
        std::wstring voltStr=L"";
        if(s.voltages.vcore>0){wchar_t vv[24];swprintf_s(vv,L"  %.3fV",s.voltages.vcore);voltStr=vv;}
        swprintf_s(sub,L"%.0f%% charge  |  %.0f MHz  |  %d cœurs%s",
                   s.cpuLoad,s.cpuFreqMhz,s.cpuCores,voltStr.c_str());
        c.draw(s.cpuName.empty()?L"PROCESSEUR":s.cpuName.c_str(),v,L"W",sub,s.cpuLoad,C::ORANGE);
        // Temp mini
        if(s.cpuTempC>0){
            wchar_t tb[20]; swprintf_s(tb,L"%.0f°C",s.cpuTempC);
            DText(tb,{pad+cw-55,curY+18,pad+cw-8,curY+38},g_D2D.brush(C::tempColor(s.cpuTempC)),g_D2D.fmtSmall);
        }
    }

    // ── GPU ──────────────────────────────────────────────────────
    {
        Card c={pad*2+cw,curY,cw,115};
        wchar_t v[16],sub[160];
        swprintf_s(v,L"%.1f",s.gpuWatt);
        double vramGB=(double)s.gpuVramBytes/(1<<30);
        const wchar_t* src=s.gpuSrc==SensorData::GpuSrc::ADL?L"ADL réel":
                           s.gpuSrc==SensorData::GpuSrc::NVAPI?L"NvAPI réel":L"estim. TDP";
        swprintf_s(sub,L"%.0f%% charge  |  %.1f Go VRAM  |  %s",s.gpuLoad,vramGB,src);
        c.draw(s.gpuName.empty()?L"GPU":s.gpuName.c_str(),v,L"W",sub,s.gpuLoad,C::CYAN);
        // Tj mini (FIX : vraie Tj, pas EDGE x2)
        if(s.gpuTjC>0){
            wchar_t tb[20]; swprintf_s(tb,L"Tj %.0f°C",s.gpuTjC);
            DText(tb,{pad*2+cw*1+cw-70,curY+18,pad*2+cw*2-8,curY+38},
                  g_D2D.brush(C::tempColor(s.gpuTjC)),g_D2D.fmtSmall);
        }
    }
    curY+=122;

    // ── RAM ──────────────────────────────────────────────────────
    {
        Card c={pad,curY,cw,115};
        wchar_t v[16],sub[128];
        swprintf_s(v,L"%.1f",s.ramWatt);
        swprintf_s(sub,L"%.1f / %.1f Go  (%.0f%%)",s.ramUsedGB,s.ramTotalGB,s.ramPct);
        c.draw(L"MÉMOIRE RAM",v,L"W",sub,s.ramPct,C::GREEN);
    }

    // ── VENTILATEURS ─────────────────────────────────────────────
    {
        Card c={pad*2+cw,curY,cw,115};
        wchar_t v[16],sub[160];
        swprintf_s(v,L"%.2f",s.fanTotalWatt);
        if(s.fans.empty()){
            swprintf_s(sub,L"Aucun ventilateur détecté via WMI");
        } else {
            std::wstring fanStr;
            for(int i=0;i<(int)s.fans.size()&&i<3;i++){
                wchar_t tmp[32]; swprintf_s(tmp,L"%d tr/min",s.fans[i].rpm);
                if(!fanStr.empty()) fanStr+=L"  |  ";
                fanStr+=tmp;
            }
            swprintf_s(sub,L"%d ventilos  |  %s",(int)s.fans.size(),fanStr.c_str());
        }
        c.draw(L"VENTILATEURS",v,L"W",sub,(double)s.fanCount*20.0,C::TEAL);
    }
    curY+=122;

    // ── STOCKAGE DÉTAILLÉ ─────────────────────────────────────────
    {
        float rowH=60.0f;
        float sh=std::max(115.0f,32.0f+(float)std::max(1,(int)s.disks.size())*rowH);
        FillRoundRect({pad,curY,W-pad,curY+sh},8,C::PANEL);
        FillRoundRect({pad+1,curY,W-pad-1,curY+3},2,C::YELLOW);
        wchar_t hdr[64]; swprintf_s(hdr,L"STOCKAGE — %.1f W total",s.diskTotalWatt);
        DText(hdr,{pad+14,curY+8,W-20,curY+24},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);

        if(s.disks.empty()){
            DText(L"Aucun disque fixe détecté",{pad+20,curY+32,W-20,curY+48},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);
        } else {
            float dx=pad+14,dy=curY+28;
            float dcolW=(W-pad*2-28)/(float)s.disks.size();
            for(int i=0;i<(int)s.disks.size();i++){
                auto& d=s.disks[i];
                float fx=dx+i*dcolW;
                // Type badge
                D2D1_COLOR_F tc=d.type==L"NVMe"?C::CYAN:d.type==L"SSD"?C::GREEN:C::YELLOW;
                DText(d.type.c_str(),{fx,dy,fx+50,dy+13},g_D2D.brush(tc),g_D2D.fmtSmall);
                // Lettre + label
                std::wstring lbl=d.letter;
                if(!d.label.empty()) lbl+=L" "+d.label;
                DText(lbl.c_str(),{fx+55,dy,fx+dcolW-8,dy+13},g_D2D.brush(C::WHITE),g_D2D.fmtSmall);
                // Taille
                wchar_t sz[32]; swprintf_s(sz,L"%.0f/%.0f Go",d.usedGB,d.totalGB);
                DText(sz,{fx,dy+14,fx+dcolW-8,dy+26},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);
                // Barre usage
                double usePct=d.totalGB>0?(d.usedGB/d.totalGB*100):0;
                DrawBar(fx,dy+28,dcolW-12,5,usePct,
                        usePct>90?C::RED:usePct>70?C::ORANGE:C::GREEN);
                // Température NVMe
                wchar_t tw[20];
                if(d.tempC>0) swprintf_s(tw,L"%.0f°C",d.tempC);
                else wcscpy_s(tw,L"T: N/A");
                DText(tw,{fx,dy+36,fx+60,dy+50},g_D2D.brush(d.tempC>0?C::tempColor(d.tempC):C::DIMTEXT),g_D2D.fmtSmall);
                // Watt
                wchar_t ww[16]; swprintf_s(ww,L"%.1fW",d.estWatt);
                DText(ww,{fx+62,dy+36,fx+120,dy+50},g_D2D.brush(C::YELLOW),g_D2D.fmtSmall);
            }
        }
        curY+=sh+8;
    }

    // ── RÉSEAU DÉTAILLÉ ───────────────────────────────────────────
    {
        float rowH=50.0f;
        float nh=std::max(90.0f,32.0f+(float)std::max(1,(int)s.netAdapters.size())*rowH);
        FillRoundRect({pad,curY,W-pad,curY+nh},8,C::PANEL);
        FillRoundRect({pad+1,curY,W-pad-1,curY+3},2,C::PURPLE);
        wchar_t hdr[80]; swprintf_s(hdr,L"RÉSEAU — ↑ %.0f Ko/s  ↓ %.0f Ko/s  |  %.1f W",
                                    s.netTotalUpKBs,s.netTotalDownKBs,s.netTotalWatt);
        DText(hdr,{pad+14,curY+8,W-20,curY+24},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);

        if(s.netAdapters.empty()){
            DText(L"Aucun adaptateur actif",{pad+20,curY+30,W-20,curY+46},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);
        } else {
            float nx=pad+14,ny=curY+26;
            float ncolW=(W-pad*2-28)/(float)s.netAdapters.size();
            for(int i=0;i<(int)s.netAdapters.size();i++){
                auto& a=s.netAdapters[i];
                float fx=nx+i*ncolW;
                D2D1_COLOR_F tc=a.wifi?C::LBLUE:C::PURPLE;
                DText(a.wifi?L"WiFi":L"ETH",{fx,ny,fx+36,ny+13},g_D2D.brush(tc),g_D2D.fmtSmall);
                // Nom (tronqué)
                std::wstring nm=a.name; if(nm.size()>22) nm=nm.substr(0,20)+L"…";
                DText(nm.c_str(),{fx+40,ny,fx+ncolW-8,ny+13},g_D2D.brush(C::WHITE),g_D2D.fmtSmall);
                if(!a.ipv4.empty())
                    DText(a.ipv4.c_str(),{fx,ny+14,fx+ncolW-8,ny+26},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);
                wchar_t trf[48]; swprintf_s(trf,L"↑%.0f  ↓%.0f Ko/s  |  %.1fW",a.upKBs,a.downKBs,a.estWatt);
                DText(trf,{fx,ny+28,fx+ncolW-8,ny+40},g_D2D.brush(tc),g_D2D.fmtSmall);
            }
        }
        curY+=nh+8;
    }

    // ── AUDIO + BLUETOOTH (demi-largeur chacun) ──────────────────
    {
        // Audio
        {
            FillRoundRect({pad,curY,pad+cw,curY+70},8,C::PANEL);
            FillRoundRect({pad+1,curY,pad+cw-1,curY+3},2,C::PINK);
            DText(L"AUDIO",{pad+14,curY+8,pad+cw-8,curY+22},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
            if(s.audio.present){
                std::wstring nm=s.audio.name; if(nm.size()>28) nm=nm.substr(0,26)+L"…";
                DText(nm.c_str(),{pad+14,curY+22,pad+cw-8,curY+38},g_D2D.brush(C::WHITE),g_D2D.fmtSmall);
                wchar_t wb[32]; swprintf_s(wb,L"~%.1f W (estimé)",s.audio.estWatt);
                DText(wb,{pad+14,curY+42,pad+cw-8,curY+58},g_D2D.brush(C::PINK),g_D2D.fmtSmall);
            } else {
                DText(L"Aucun périphérique audio",{pad+14,curY+28,pad+cw-8,curY+44},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);
            }
        }
        // Bluetooth
        {
            float bx=pad*2+cw;
            FillRoundRect({bx,curY,bx+cw,curY+70},8,C::PANEL);
            FillRoundRect({bx+1,curY,bx+cw-1,curY+3},2,C::LBLUE);
            DText(L"BLUETOOTH",{bx+14,curY+8,bx+cw-8,curY+22},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
            if(s.btPresent){
                DText(L"Contrôleur détecté",{bx+14,curY+22,bx+cw-8,curY+38},g_D2D.brush(C::WHITE),g_D2D.fmtSmall);
                DText(L"~0.5 W",{bx+14,curY+42,bx+cw-8,curY+58},g_D2D.brush(C::LBLUE),g_D2D.fmtSmall);
            } else {
                DText(L"Non détecté",{bx+14,curY+28,bx+cw-8,curY+44},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);
            }
        }
        curY+=78;
    }

    // ── ÉCRANS ───────────────────────────────────────────────────
    {
        float rowH=78.0f;
        float mh=std::max(115.0f,30.0f+(float)s.monitors.size()*rowH);
        FillRoundRect({pad,curY,W-pad,curY+mh},8,C::PANEL);
        FillRoundRect({pad+1,curY,W-pad-1,curY+3},2,C::YELLOW);
        wchar_t hdr[64]; swprintf_s(hdr,L"AFFICHAGE — %d écran(s)  ~%.0f W",s.monitorCount,s.monitorWatt);
        DText(hdr,{pad+14,curY+8,W-20,curY+26},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        float my=curY+32, colW=(W-pad*2-28)/std::max(1,(int)s.monitors.size());
        for(int i=0;i<(int)s.monitors.size();i++){
            auto& m=s.monitors[i]; float mx=pad+14+i*colW;
            FillRoundRect({mx-4,my-2,mx+colW-8,my+rowH-4},4,{0.035f,0.043f,0.067f,1});
            if(m.primary) DrawRoundRect({mx-4,my-2,mx+colW-8,my+rowH-4},4,C::YELLOW,0.8f);
            if(!m.edidMfr.empty()) DText(m.edidMfr.c_str(),{mx,my,mx+colW-10,my+14},
                g_D2D.brush(m.edidMfr==L"Koorui"?C::CYAN:C::GREEN),g_D2D.fmtSmall);
            std::wstring name=m.friendlyName.empty()?m.deviceName:m.friendlyName;
            if(!m.edidMfr.empty()){std::wstring lo=name,mfr=m.edidMfr;for(auto&c:lo)c=towlower(c);for(auto&c:mfr)c=towlower(c);if(lo.find(mfr)!=std::wstring::npos)name=name.substr(m.edidMfr.size());while(!name.empty()&&(name[0]==L' '||name[0]==L'-'))name=name.substr(1);}
            if(name.size()>26) name=name.substr(0,24)+L"…";
            DText(name.c_str(),{mx,my+(m.edidMfr.empty()?0:14),mx+colW-10,my+28},
                  g_D2D.brush(m.primary?C::YELLOW:C::WHITE),g_D2D.fmtSmall);
            wchar_t rl[48];
            if(m.refreshHz>0) swprintf_s(rl,L"%d×%d  @%dHz",m.widthPx,m.heightPx,m.refreshHz);
            else swprintf_s(rl,L"%d×%d",m.widthPx,m.heightPx);
            DText(rl,{mx,my+28,mx+colW-10,my+41},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);
            wchar_t sl[48];
            if(m.diagInch>0) swprintf_s(sl,L"%.1f\"  %dmm×%dmm",m.diagInch,m.widthMm,m.heightMm);
            else wcscpy_s(sl,L"Taille EDID inconnue");
            DText(sl,{mx,my+42,mx+colW-10,my+55},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
            bool isK=(m.edidMfr==L"Koorui"||m.edidMfr==L"Koorui/RTK");
            double kwCheck=kooruiWattFromModel(m.friendlyName);
            bool hasExact=isK&&kwCheck>0;
            wchar_t pw[48]; swprintf_s(pw,L"~%.0f W  %s%s",m.estWattW,
                hasExact?L"[datasheet]":L"[estimé]",m.primary?L"  ★":L"");
            DText(pw,{mx,my+56,mx+colW-10,my+68},g_D2D.brush(C::YELLOW),g_D2D.fmtSmall);
        }
        curY+=mh+8;
    }

    // ── USB ──────────────────────────────────────────────────────
    {
        float uh=std::max(90.0f,32.0f+s.usbDevices.size()*18.0f);
        FillRoundRect({pad,curY,W-pad,curY+uh},8,C::PANEL);
        FillRoundRect({pad+1,curY,W-pad-1,curY+3},2,C::PURPLE);
        wchar_t lbl[64]; swprintf_s(lbl,L"USB (%d périphériques — ~%.1fW)",s.usbCount,s.usbWatt);
        DText(lbl,{pad+14,curY+8,W-20,curY+24},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        float dy=curY+26,col2=W/2;
        for(int i=0;i<(int)s.usbDevices.size();i++){
            auto& d=s.usbDevices[i];
            float fx=(i%2==0)?pad+20:col2+10;
            float fy=dy+(i/2)*18.0f;
            std::wstring nm=d.first; if(nm.size()>32) nm=nm.substr(0,30)+L"…";
            DText(nm.c_str(),{fx,fy,fx+280,fy+16},g_D2D.brush(C::WHITE),g_D2D.fmtSmall);
            wchar_t wb[16]; swprintf_s(wb,L"~%.1fW",d.second);
            DText(wb,{fx+285,fy,fx+340,fy+16},g_D2D.brush(C::PURPLE),g_D2D.fmtSmall);
        }
        curY+=uh+8;
    }

    // ── TENSIONS CPU ─────────────────────────────────────────────
    {
        float vh=60;
        FillRoundRect({pad,curY,W-pad,curY+vh},8,C::PANEL);
        FillRoundRect({pad+1,curY,W-pad-1,curY+3},2,C::TEAL);
        DText(L"TENSIONS (WMI — précision limitée)",{pad+14,curY+6,W/2,curY+20},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        float vx=pad+14,vy=curY+20;
        auto showV=[&](const wchar_t* lbl,double v,float& xx){
            DText(lbl,{xx,vy,xx+80,vy+13},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
            wchar_t buf[16];
            if(v>0) swprintf_s(buf,L"%.3f V",v);
            else    wcscpy_s(buf,L"N/A");
            DText(buf,{xx,vy+14,xx+80,vy+28},g_D2D.brush(v>0?C::TEAL:C::MUTED),g_D2D.fmtSmall);
            xx+=100;
        };
        showV(L"Vcore", s.voltages.vcore, vx);
        showV(L"+12V",  s.voltages.v12,   vx);
        showV(L"+5V",   s.voltages.v5,    vx);
        showV(L"+3.3V", s.voltages.v3_3,  vx);
        curY+=vh+8;
    }

    // ── BILAN ÉNERGIE ────────────────────────────────────────────
    {
        float bh=120;
        FillRoundRect({pad,curY,W-pad,curY+bh},8,C::PANEL);
        FillRoundRect({pad+1,curY,W-pad-1,curY+3},2,C::GREEN);
        DText(L"BILAN ÉNERGÉTIQUE",{pad+14,curY+8,W/2,curY+24},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        double kwh=s.cumWhSession/1000.0, cost=kwh*g_TarifKwh;
        double avg=hist.empty()?s.totalWatt:std::accumulate(hist.begin(),hist.end(),0.0)/hist.size();
        double c24=avg*24.0/1000.0*g_TarifKwh, cMo=c24*30.0;
        wchar_t tmp[80]; float cy2=curY+24;
        swprintf_s(tmp,L"Cumulé : %.4f Wh  (%.8f kWh)",s.cumWhSession,kwh);
        DText(tmp,{pad+14,cy2,W*0.6f,cy2+16},g_D2D.brush(C::WHITE),g_D2D.fmtSmall); cy2+=20;
        swprintf_s(tmp,L"Tarif  : %.4f €/kWh",g_TarifKwh);
        DText(tmp,{pad+14,cy2,W*0.6f,cy2+16},g_D2D.brush(C::MUTED),g_D2D.fmtSmall); cy2+=20;
        swprintf_s(tmp,L"Proj. 24h : %.3f €    Mois : %.2f €",c24,cMo);
        DText(tmp,{pad+14,cy2,W*0.6f,cy2+16},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);
        wchar_t cbuf[32]; swprintf_s(cbuf,L"%.5f €",cost);
        DText(L"COÛT SESSION",{W*0.65f,curY+24,W-20,curY+40},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        DText(cbuf,{W*0.65f,curY+40,W-20,curY+90},g_D2D.brush(C::ORANGE),g_D2D.fmtBig);
        curY+=bh+8;
    }

    // ── FOOTER ───────────────────────────────────────────────────
    {
        FillRoundRect({0,curY,W,curY+28},0,{0.035f,0.043f,0.067f,1});
        DText(L"⚠ Estimation TDP×usage (±15W) | Tj AMD : ADL PMLog index 80 (Hotspot) | PowerMon v3.0 | Win32+D2D",
              {14,curY+6,W-14,curY+22},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        curY+=28;
    }

    g_ContentH=(int)(curY+g_ScrollY+20);
    g_D2D.rt->EndDraw();
}

// ─────────────────────────────────────────────────────────────────
//  SCROLLBAR
// ─────────────────────────────────────────────────────────────────
static void UpdateScrollbar(HWND hw){
    RECT rc; GetClientRect(hw,&rc);
    SCROLLINFO si{}; si.cbSize=sizeof(si); si.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
    si.nMin=0; si.nMax=g_ContentH; si.nPage=(UINT)rc.bottom; si.nPos=g_ScrollY;
    SetScrollInfo(hw,SB_VERT,&si,TRUE);
}

// ─────────────────────────────────────────────────────────────────
//  RÉGLAGES
// ─────────────────────────────────────────────────────────────────
static HWND g_HwndSettings=nullptr;
#define ID_CPU_TDP  200
#define ID_GPU_TDP  201
#define ID_SYS_BASE 202
#define ID_TARIF    203
#define ID_SAVE     204

static LRESULT CALLBACK SettingsProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        auto lbl=[&](const wchar_t* t,int x,int y){CreateWindowW(L"STATIC",t,WS_CHILD|WS_VISIBLE,x,y,180,20,hw,0,0,0);};
        auto edit=[&](int id,double v,int x,int y){wchar_t buf[32];swprintf_s(buf,L"%.2f",v);CreateWindowW(L"EDIT",buf,WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER,x,y,80,22,hw,(HMENU)(INT_PTR)id,0,0);};
        lbl(L"TDP CPU max (W) :",10,14);  edit(ID_CPU_TDP, g_CpuTdpMax,200,12);
        lbl(L"TDP GPU max (W) :",10,44);  edit(ID_GPU_TDP, g_GpuTdpMax,200,42);
        lbl(L"Système base (W):",10,74);  edit(ID_SYS_BASE,g_SysBase,  200,72);
        lbl(L"Tarif €/kWh :",    10,104); edit(ID_TARIF,   g_TarifKwh, 200,102);
        CreateWindowW(L"BUTTON",L"Enregistrer",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,80,140,140,28,hw,(HMENU)ID_SAVE,0,0);
        break;}
    case WM_COMMAND:
        if(LOWORD(wp)==ID_SAVE){
            auto g=[&](int id)->double{wchar_t b[32]={};GetDlgItemTextW(hw,id,b,31);return _wtof(b);};
            g_CpuTdpMax=g(ID_CPU_TDP); g_GpuTdpMax=g(ID_GPU_TDP);
            g_SysBase=g(ID_SYS_BASE);  g_TarifKwh=g(ID_TARIF);
            DestroyWindow(hw); g_HwndSettings=nullptr;}
        break;
    case WM_DESTROY: g_HwndSettings=nullptr; break;
    }
    return DefWindowProcW(hw,msg,wp,lp);
}
static void OpenSettings(HWND p){
    if(g_HwndSettings){SetForegroundWindow(g_HwndSettings);return;}
    static bool reg=false;
    if(!reg){WNDCLASSW wc{};wc.lpfnWndProc=SettingsProc;wc.hInstance=GetModuleHandleW(nullptr);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);wc.lpszClassName=L"PowerMonSettings";RegisterClassW(&wc);reg=true;}
    g_HwndSettings=CreateWindowW(L"PowerMonSettings",L"Réglages PowerMon v3",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,310,210,p,nullptr,GetModuleHandleW(nullptr),nullptr);
    ShowWindow(g_HwndSettings,SW_SHOW);
}

// ─────────────────────────────────────────────────────────────────
//  WNDPROC
// ─────────────────────────────────────────────────────────────────
#define ID_TIMER 1
static LRESULT CALLBACK WndProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE: g_Hwnd=hw; SetTimer(hw,ID_TIMER,1000,nullptr); g_D2D.init(hw); break;
    case WM_TIMER:  InvalidateRect(hw,nullptr,FALSE); UpdateScrollbar(hw); break;
    case WM_PAINT:{ PAINTSTRUCT ps; BeginPaint(hw,&ps); Render(hw); EndPaint(hw,&ps); break;}
    case WM_SIZE:   g_D2D.resize(hw); InvalidateRect(hw,nullptr,FALSE); break;
    case WM_VSCROLL:{
        SCROLLINFO si{}; si.cbSize=sizeof(si); si.fMask=SIF_ALL;
        GetScrollInfo(hw,SB_VERT,&si);
        RECT rc; GetClientRect(hw,&rc);
        { int viewH=(int)rc.bottom;
          switch(LOWORD(wp)){
          case SB_TOP:        g_ScrollY=0; break;
          case SB_BOTTOM:     g_ScrollY=g_ContentH-viewH; break;
          case SB_LINEUP:     g_ScrollY-=30; break;
          case SB_LINEDOWN:   g_ScrollY+=30; break;
          case SB_PAGEUP:     g_ScrollY-=viewH; break;
          case SB_PAGEDOWN:   g_ScrollY+=viewH; break;
          case SB_THUMBTRACK: g_ScrollY=(int)si.nTrackPos; break;
          }
          int top=g_ContentH-viewH; g_ScrollY=g_ScrollY<0?0:(g_ScrollY>top&&top>0?top:g_ScrollY);
        }
        UpdateScrollbar(hw); InvalidateRect(hw,nullptr,FALSE); break;}
    case WM_MOUSEWHEEL:{
        int delta=GET_WHEEL_DELTA_WPARAM(wp);
        RECT rc; GetClientRect(hw,&rc);
        { int viewH=(int)rc.bottom; int top=g_ContentH-viewH; int npos=g_ScrollY-delta/2; g_ScrollY=npos<0?0:(npos>top&&top>0?top:npos); }
        UpdateScrollbar(hw); InvalidateRect(hw,nullptr,FALSE); break;}
    case WM_RBUTTONUP:{
        HMENU m=CreatePopupMenu();
        AppendMenuW(m,MF_STRING,1,L"⚙ Réglages TDP / Tarif…");
        AppendMenuW(m,MF_SEPARATOR,0,nullptr);
        AppendMenuW(m,MF_STRING,2,L"Quitter");
        POINT pt; GetCursorPos(&pt);
        int cmd=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,pt.x,pt.y,0,hw,nullptr);
        DestroyMenu(m);
        if(cmd==1) OpenSettings(hw);
        if(cmd==2) PostQuitMessage(0);
        break;}
    case WM_KEYDOWN: if(wp==VK_F5) OpenSettings(hw); break;
    case WM_DESTROY: g_Running=false; KillTimer(hw,ID_TIMER); PostQuitMessage(0); break;
    }
    return DefWindowProcW(hw,msg,wp,lp);
}

// ─────────────────────────────────────────────────────────────────
//  WINMAIN
// ─────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hi,HINSTANCE,LPSTR,int nShow){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_WIN95_CLASSES};
    InitCommonControlsEx(&ic);

    WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=WndProc;
    wc.hInstance=hi; wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName=L"PowerMonV3"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.style=CS_HREDRAW|CS_VREDRAW;
    RegisterClassExW(&wc);

    HWND hw=CreateWindowExW(0,L"PowerMonV3",
        L"⚡ PowerMon v3.0 — Moniteur de consommation",
        WS_OVERLAPPEDWINDOW|WS_VSCROLL,
        CW_USEDEFAULT,CW_USEDEFAULT,860,960,
        nullptr,nullptr,hi,nullptr);
    ShowWindow(hw,nShow); UpdateWindow(hw);

    std::thread st(sensorThread); st.detach();

    MSG m{};
    while(GetMessage(&m,nullptr,0,0)){ TranslateMessage(&m); DispatchMessage(&m); }
    g_Running=false; Sleep(200);
    return 0;
}
