/*
 * PowerMon GUI — Moniteur de consommation électrique Windows 11
 * GUI Win32 natif + Direct2D pour les graphes
 * Capteurs : PDH (CPU/GPU%), WMI (noms/températures), DXGI (GPU),
 *            GlobalMemoryStatusEx (RAM), SetupAPI (USB),
 *            GetSystemPowerStatus (batterie), EnumDisplayDevices (écrans)
 *
 * Compile : MSVC 2019/2022  x64  (voir CMakeLists.txt)
 * Aucune dépendance externe — Win32 API pure + D2D/DWrite
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
//  CONFIGURATION TDP (ajustable dans l'UI plus tard)
// ─────────────────────────────────────────────────────────────────
static double g_CpuTdpMax   = 95.0;
static double g_CpuTdpIdle  =  8.0;
static double g_GpuTdpMax   = 185.0;
static double g_GpuTdpIdle  =  12.0;
static double g_SysBase     =  45.0;   // carte mère, stockage, ventilos
static double g_TarifKwh    =  0.2516;
static const int HIST = 120;           // secondes affichées dans le graphe

// ─────────────────────────────────────────────────────────────────
//  COULEURS (Dark theme)
// ─────────────────────────────────────────────────────────────────
namespace C {
    static const D2D1_COLOR_F BG       = {0.027f,0.031f,0.051f,1};
    static const D2D1_COLOR_F PANEL    = {0.051f,0.059f,0.094f,1};
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
}

// ─────────────────────────────────────────────────────────────────
//  STRUCTURES DE DONNÉES CAPTEURS
// ─────────────────────────────────────────────────────────────────
struct SensorData {
    // CPU
    double cpuLoad     = 0;   // %
    double cpuWatt     = 0;
    double cpuTempC    = 0;
    double cpuFreqMhz  = 0;
    int    cpuCores    = 0;
    std::wstring cpuName;

    // GPU
    double gpuLoad     = 0;   // %
    double gpuWatt     = 0;
    double gpuTempC    = 0;
    double gpuMemUsedMB= 0;
    double gpuMemTotalMB= 0;
    double gpuCoreMhz  = 0;
    std::wstring gpuName;
    size_t gpuVramBytes = 0;
    bool   gpuWattReal  = false;  // true = valeur ADL réelle, false = estimation TDP

    // RAM
    double ramUsedGB   = 0;
    double ramTotalGB  = 0;
    double ramPct      = 0;
    double ramWatt     = 0;
    double ramFreqMhz  = 0;

    // Réseau
    double netUpKBs    = 0;
    double netDownKBs  = 0;

    // Batterie / secteur
    bool   onBattery   = false;
    int    battPct     = -1;   // -1 = pas de batterie
    bool   charging    = false;

    // USB
    int    usbCount    = 0;
    double usbWatt     = 0;
    std::vector<std::pair<std::wstring,double>> usbDevices;

    // Écrans
    struct MonitorInfo {
        std::wstring deviceName;    // \\.\DISPLAY1
        std::wstring friendlyName;  // "Dell U2722D" depuis EDID/registre
        std::wstring edidMfr;       // code fabricant 3 lettres (DEL, SAM, LGD…)
        int    widthPx  = 0;
        int    heightPx = 0;
        int    widthMm  = 0;        // taille physique depuis EDID
        int    heightMm = 0;
        double diagInch = 0;        // diagonale calculée
        int    refreshHz= 0;
        bool   primary  = false;
        double estWattW = 0;        // estimation W selon taille
    };
    int    monitorCount = 0;
    double monitorWatt  = 0;
    std::vector<MonitorInfo> monitors;

    // Disk
    double diskReadMBs  = 0;
    double diskWriteMBs = 0;

    // Total
    double totalWatt   = 0;
    double cumWhSession= 0;
    int64_t sessionSec = 0;
};

static SensorData g_Sensor;
static std::mutex g_Mtx;
static std::deque<double> g_History;   // W au fil du temps
static std::atomic<bool>  g_Running{true};

// ─────────────────────────────────────────────────────────────────
//  THREAD CAPTEURS
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
        PdhAddEnglishCounterW(q, L"\\Processor(_Total)\\% Processor Time", 0, &cCpuLoad);
        PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &cGpuEngine);
        PdhAddEnglishCounterW(q, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",  0, &cDiskRead);
        PdhAddEnglishCounterW(q, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &cDiskWrite);
        PdhAddEnglishCounterW(q, L"\\Network Interface(*)\\Bytes Sent/sec",     0, &cNetSent);
        PdhAddEnglishCounterW(q, L"\\Network Interface(*)\\Bytes Received/sec", 0, &cNetRecv);
        PdhCollectQueryData(q);
        ok = true;
        return true;
    }

    double readScalar(PDH_HCOUNTER c) {
        if (!c) return 0;
        PDH_FMT_COUNTERVALUE v{};
        PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v);
        return std::max(0.0, v.doubleValue);
    }

    // GPU engine: somme toutes les instances
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

    // Réseau: somme toutes les interfaces
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

// WMI helper
static IWbemServices* g_WmiSvc = nullptr;
static bool initWmi() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoInitializeSecurity(nullptr,-1,nullptr,nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT,RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,EOAC_NONE,nullptr);
    IWbemLocator* loc=nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator,0,CLSCTX_INPROC_SERVER,
        IID_IWbemLocator,(void**)&loc))) return false;
    bool r = SUCCEEDED(loc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"),
        nullptr,nullptr,0,0,0,0,&g_WmiSvc));
    if (r) CoSetProxyBlanket(g_WmiSvc,RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,
        nullptr,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,EOAC_NONE);
    loc->Release();
    return r;
}

static std::wstring wmiGetStr(const wchar_t* cls, const wchar_t* prop) {
    if (!g_WmiSvc) return {};
    std::wstring q = std::wstring(L"SELECT ") + prop + L" FROM " + cls;
    IEnumWbemClassObject* en=nullptr;
    if (FAILED(g_WmiSvc->ExecQuery(_bstr_t(L"WQL"),_bstr_t(q.c_str()),
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

// Fréquence CPU via WMI (MHz)
static double wmiGetCpuFreq() {
    if (!g_WmiSvc) return 0;
    IEnumWbemClassObject* en=nullptr;
    g_WmiSvc->ExecQuery(_bstr_t(L"WQL"),
        _bstr_t(L"SELECT CurrentClockSpeed FROM Win32_Processor"),
        WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&en);
    if (!en) return 0;
    double f=0;
    IWbemClassObject* obj=nullptr; ULONG ret=0;
    if (en->Next(WBEM_INFINITE,1,&obj,&ret)==S_OK) {
        VARIANT v; VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"CurrentClockSpeed",0,&v,0,0)))
            f = (v.vt==VT_I4)?(double)v.intVal:(v.vt==VT_UI4)?(double)v.uintVal:0;
        VariantClear(&v); obj->Release();
    }
    en->Release();
    return f;
}

// GPU VRAM via DXGI
static void dxgiGetGpuInfo(std::wstring& name, size_t& vramBytes) {
    IDXGIFactory* fac=nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory),(void**)&fac))) return;
    IDXGIAdapter* ada=nullptr;
    if (SUCCEEDED(fac->EnumAdapters(0,&ada))) {
        DXGI_ADAPTER_DESC d{};
        if (SUCCEEDED(ada->GetDesc(&d))) {
            name = d.Description;
            vramBytes = d.DedicatedVideoMemory;
        }
        ada->Release();
    }
    fac->Release();
}

// USB enumération
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
        // Estimation par type
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
//  MONITEURS — EnumDisplayDevices + DEVMODE + EDID registre
// ─────────────────────────────────────────────────────────────────

// Décode le code fabricant EDID (3 lettres encodées sur 2 octets big-endian)
// ─────────────────────────────────────────────────────────────────
//  TABLE PnP FABRICANTS (codes EDID 3 lettres → nom lisible)
//  Sources : uefi.org/PNP_ID_List + hwids.com
// ─────────────────────────────────────────────────────────────────
struct MfrEntry { const wchar_t* code; const wchar_t* name; };
static const MfrEntry k_MfrTable[] = {
    // ── Koorui / Realtek OEM ──────────────────────────────────
    {L"KOR", L"Koorui"},      // code PnP propre Koorui (récent)
    {L"RTK", L"Koorui/RTK"},  // Realtek — utilisé par certains Koorui OEM
    {L"KUI", L"Koorui"},      // variante observée sur firmware 2023+
    // ── Grands fabricants ─────────────────────────────────────
    {L"DEL", L"Dell"},
    {L"SAM", L"Samsung"},
    {L"LGD", L"LG"},
    {L"LEN", L"Lenovo"},
    {L"AUO", L"AU Optronics"},
    {L"CMN", L"Chimei Innolux"},
    {L"BOE", L"BOE"},
    {L"SDC", L"Samsung Display"},
    {L"SHP", L"Sharp"},
    {L"HWP", L"HP"},
    {L"ACR", L"Acer"},
    {L"VSC", L"ViewSonic"},
    {L"BNQ", L"BenQ"},
    {L"ASU", L"ASUS"},
    {L"MSI", L"MSI"},
    {L"GSM", L"LG"},
    {L"PHL", L"Philips"},
    {L"IOD", L"I-O Data"},
    {L"NEC", L"NEC"},
    {L"ENC", L"Eizo"},
    {L"AIC", L"AG Neovo"},
    {L"IVM", L"Iiyama"},
    {L"HSD", L"Hannstar"},
    {L"CHR", L"Chicony"},
    {L"CPT", L"CPT"},
    {L"IQT", L"Iquo"},
};

static std::wstring mfrNameFromCode(const std::wstring& code) {
    for (auto& e : k_MfrTable)
        if (code == e.code) return e.name;
    return code; // code inconnu → retourne le code brut
}

// ─────────────────────────────────────────────────────────────────
//  TABLE MODÈLES KOORUI → PUISSANCE MESURÉE (datasheet + tests)
//  Modèles courants 2022-2024
// ─────────────────────────────────────────────────────────────────
struct ModelPower { const wchar_t* model; double watt; };
static const ModelPower k_KooruiModels[] = {
    // 24" FHD 100Hz (24E4 / 24N5A / 24N5B)
    {L"24E4",  22.0}, {L"24N5A", 22.0}, {L"24N5B", 22.0},
    {L"24E3",  20.0}, {L"24N4",  20.0},
    // 27" QHD 100/144Hz (27E6QA / 27N5A / 27N5C)
    {L"27E6",  28.0}, {L"27E6Q", 28.0}, {L"27E6QA",28.0},
    {L"27N5A", 26.0}, {L"27N5C", 26.0}, {L"27N4",  25.0},
    // 27" FHD
    {L"27E3",  24.0}, {L"27E4",  24.0},
    // 32" QHD (32N5A)
    {L"32N5A", 38.0}, {L"32E6",  40.0},
    // 34" ultrawide
    {L"34N5A", 55.0},
};

static double kooruiWattFromModel(const std::wstring& modelName) {
    for (auto& m : k_KooruiModels) {
        // Cherche le code modèle dans le nom (insensible à la casse)
        std::wstring lo = modelName;
        std::wstring pat = m.model;
        for (auto& c : lo)  c = towupper(c);
        for (auto& c : pat) c = towupper(c);
        if (lo.find(pat) != std::wstring::npos) return m.watt;
    }
    return -1.0; // inconnu
}

// ─────────────────────────────────────────────────────────────────
//  EDID : décodage fabricant (octets 8-9, encodage PnP 5 bits)
// ─────────────────────────────────────────────────────────────────
static std::wstring edidMfrCode(const BYTE* edid) {
    WORD w = (WORD)((edid[8] << 8) | edid[9]);
    // Vérifie que les bits 15 sont à 0 (champ valide)
    if (w == 0x0000 || w == 0xFFFF) return {};
    wchar_t mfr[4] = {
        (wchar_t)(L'A' + ((w >> 10) & 0x1F) - 1),
        (wchar_t)(L'A' + ((w >>  5) & 0x1F) - 1),
        (wchar_t)(L'A' + ((w >>  0) & 0x1F) - 1),
        L'\0'
    };
    // Sanity : lettres A-Z uniquement
    for (int i=0;i<3;i++)
        if (mfr[i]<L'A'||mfr[i]>L'Z') return {};
    return mfr;
}

// ─────────────────────────────────────────────────────────────────
//  EDID : nom modèle depuis descripteur 0xFC (Monitor Name)
//         + fallback descripteur 0xFE (ASCII string non standard)
// ─────────────────────────────────────────────────────────────────
static std::wstring edidFriendlyName(const BYTE* edid, int len) {
    if (len < 128) return {};
    std::wstring best;
    for (int i = 54; i <= 108; i += 18) {
        // Octets i+0..i+1 = 0x00 0x00 → descriptor étendu
        if (edid[i] != 0x00 || edid[i+1] != 0x00) continue;
        BYTE dtype = edid[i+3];
        if (dtype != 0xFC && dtype != 0xFE) continue; // 0xFC=name, 0xFE=string

        char name[14] = {};
        memcpy(name, edid + i + 5, 13);
        // Trim \n, \r, espaces finaux
        for (int j=12;j>=0;j--) {
            if (name[j]=='\n'||name[j]=='\r'||name[j]==' '||name[j]=='\0')
                name[j]='\0';
            else break;
        }
        if (name[0] == '\0') continue;

        int sz = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
        std::wstring ws(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name, -1, ws.data(), sz);
        while (!ws.empty() && ws.back()==L'\0') ws.pop_back();

        if (dtype == 0xFC) return ws;   // Monitor Name descriptor → priorité max
        if (best.empty())  best = ws;   // String descriptor → fallback
    }
    return best;
}

// ─────────────────────────────────────────────────────────────────
//  EDID : Product Code (octets 10-11, little-endian) → hex string
//  Utile pour identifier le modèle exact quand le nom EDID est vide
// ─────────────────────────────────────────────────────────────────
static std::wstring edidProductCode(const BYTE* edid) {
    WORD pc = (WORD)(edid[10] | (edid[11] << 8));
    wchar_t buf[8]; swprintf_s(buf, L"%04X", pc);
    return buf;
}

// ─────────────────────────────────────────────────────────────────
//  ESTIMATION PUISSANCE
//  1. Modèle Koorui connu → valeur mesurée
//  2. Table générique par marque + diagonale
// ─────────────────────────────────────────────────────────────────
static double estimateMonitorWatt(const std::wstring& mfrCode,
                                  const std::wstring& modelName,
                                  double diagInch)
{
    // 1. Koorui exact
    bool isKoorui = (mfrCode==L"KOR"||mfrCode==L"RTK"||mfrCode==L"KUI");
    if (isKoorui && !modelName.empty()) {
        double w = kooruiWattFromModel(modelName);
        if (w > 0) return w;
    }

    // 2. Diagonale connue
    if (diagInch > 0) {
        // Koorui est généralement 15-20% plus économique que les grandes marques
        double base;
        if      (diagInch < 22) base = 16.0;
        else if (diagInch < 24) base = 20.0;
        else if (diagInch < 25) base = 22.0;
        else if (diagInch < 27) base = 25.0;
        else if (diagInch < 28) base = 28.0;
        else if (diagInch < 32) base = 32.0;
        else if (diagInch < 35) base = 40.0;
        else                    base = 55.0;
        return isKoorui ? base * 0.85 : base;
    }

    // 3. Fallback absolu
    return 25.0;
}

using MonitorInfo = SensorData::MonitorInfo;

// Callback EnumDisplayMonitors
struct EnumMonCtx {
    std::vector<MonitorInfo>* list;
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumMonCtx*>(lp);

    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;

    MonitorInfo info;
    info.deviceName = mi.szDevice;
    info.primary    = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    info.widthPx    = mi.rcMonitor.right  - mi.rcMonitor.left;
    info.heightPx   = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // Résolution et taux de rafraîchissement via DEVMODE
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        info.widthPx   = dm.dmPelsWidth;
        info.heightPx  = dm.dmPelsHeight;
        info.refreshHz = dm.dmDisplayFrequency;
    }

    // Taille physique via EnumDisplayDevices → clé registre → EDID
    DISPLAY_DEVICEW dd{}; dd.cb = sizeof(dd);
    // Trouve le device ID (ex: "\\.\DISPLAY1\Monitor0")
    for (DWORD i = 0; EnumDisplayDevicesW(mi.szDevice, i, &dd, 0); i++) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;

        // dd.DeviceID contient le chemin PnP comme
        // "MONITOR\DEL4277\{...}\0001" → on extrait le préfixe pour SetupAPI
        std::wstring devId = dd.DeviceID;

        // Essai direct EDID via SetupAPI classe Monitor
        {
            GUID monGuid = {0x4d36e96e,0xe325,0x11ce,
                            {0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
            HDEVINFO hDev = SetupDiGetClassDevsW(&monGuid, nullptr, nullptr,
                                                  DIGCF_PRESENT);
            if (hDev != INVALID_HANDLE_VALUE) {
                SP_DEVINFO_DATA did2{}; did2.cbSize = sizeof(did2);
                for (DWORD j = 0; SetupDiEnumDeviceInfo(hDev, j, &did2); j++) {
                    wchar_t instId[512]{};
                    CM_Get_Device_IDW(did2.DevInst, instId, 511, 0);
                    // Vérifie que l'instance match notre moniteur
                    std::wstring inst(instId);
                    // Extrait le modèle depuis devId (ex: "MONITOR\DEL4277\...")
                    bool match = false;
                    if (devId.size() > 8) {
                        // compare le segment modèle (après MONITOR\)
                        size_t p = devId.find(L'\\');
                        if (p != std::wstring::npos) {
                            std::wstring model = devId.substr(p+1);
                            // inst contient quelque chose comme DISPLAY\DEL4277\...
                            match = (inst.find(model.substr(0,
                                     model.find(L'\\'))) != std::wstring::npos);
                        }
                    }
                    if (!match && inst.find(L"DISPLAY") == std::wstring::npos)
                        continue;
                    if (!match) {
                        // heuristique : prend le premier Monitor dispo
                        // si on n'a pas encore de nom, c'est acceptable
                    }

                    HKEY hKey = SetupDiOpenDevRegKey(hDev, &did2,
                        DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
                    if (hKey == INVALID_HANDLE_VALUE) continue;

                    DWORD type2=0, sz2=0;
                    if (RegQueryValueExW(hKey,L"EDID",nullptr,&type2,
                                        nullptr,&sz2)==ERROR_SUCCESS && sz2>=128) {
                        std::vector<BYTE> edid(sz2);
                        RegQueryValueExW(hKey,L"EDID",nullptr,&type2,
                                        edid.data(),&sz2);
                        RegCloseKey(hKey);

                        // Taille physique : octets 21 (H mm) et 22 (V mm)
                        info.widthMm  = edid[21];
                        info.heightMm = edid[22];

                        // Diagonale en pouces
                        if (info.widthMm > 0 && info.heightMm > 0) {
                            double diagMm = sqrt((double)info.widthMm*info.widthMm
                                               + (double)info.heightMm*info.heightMm);
                            info.diagInch = diagMm / 25.4;
                        }

                        // Fabricant PnP + nom modèle depuis EDID
                        info.edidMfr      = edidMfrCode(edid.data());
                        info.friendlyName = edidFriendlyName(edid.data(),(int)sz2);

                        // Si le descripteur 0xFC est vide (certains Koorui OEM
                        // n'encodent pas le nom modèle) → construire depuis
                        // nom fabricant + product code
                        if (info.friendlyName.empty() && !info.edidMfr.empty()) {
                            std::wstring brand = mfrNameFromCode(info.edidMfr);
                            std::wstring pc    = edidProductCode(edid.data());
                            info.friendlyName  = brand + L" " + pc;
                        }
                        break;
                    }
                    RegCloseKey(hKey);
                }
                SetupDiDestroyDeviceInfoList(hDev);
            }
        }
        break; // on a traité le premier device actif
    }

    // Fallback nom si EDID toujours vide
    if (info.friendlyName.empty()) {
        DISPLAY_DEVICEW dd2{}; dd2.cb = sizeof(dd2);
        if (EnumDisplayDevicesW(mi.szDevice, 0, &dd2, 0) && dd2.DeviceString[0]) {
            info.friendlyName = dd2.DeviceString;
            // Détecte Koorui dans le nom Windows même sans EDID
            std::wstring lo = info.friendlyName;
            for (auto& c : lo) c = towlower(c);
            if (lo.find(L"koorui") != std::wstring::npos && info.edidMfr.empty())
                info.edidMfr = L"KOR";
        } else {
            info.friendlyName = mi.szDevice;
        }
    }

    // Résout le nom lisible du fabricant
    if (!info.edidMfr.empty())
        info.edidMfr = mfrNameFromCode(info.edidMfr); // remplace le code par le nom

    info.estWattW = estimateMonitorWatt(info.edidMfr, info.friendlyName, info.diagInch);
    ctx->list->push_back(info);
    return TRUE;
}

static void enumMonitors(std::vector<MonitorInfo>& monitors, int& cnt, double& totalW) {
    monitors.clear();
    EnumMonCtx ctx{&monitors};
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, (LPARAM)&ctx);
    cnt    = (int)monitors.size();
    totalW = 0;
    for (auto& m : monitors) totalW += m.estWattW;
}

// ─────────────────────────────────────────────────────────────────
//  ADL2 — Lecture puissance GPU AMD réelle
//  Chargement dynamique de atiadlxx.dll (drivers AMD requis)
//  Aucune dépendance au compile — fallback silencieux si absent
// ─────────────────────────────────────────────────────────────────

// Structures ADL minimales (subset de adl_sdk.h)
typedef void* ADL_CONTEXT_HANDLE;

struct AdlAdapterInfo {
    int  iSize;
    int  iAdapterIndex;
    char strUDID[256];
    int  iBusNumber;
    int  iDeviceNumber;
    int  iFunctionNumber;
    int  iVendorID;
    char strAdapterName[256];
    char strDisplayName[256];
    int  iPresent;
    int  iExist;
    char strDriverPath[256];
    char strDriverPathExt[256];
    char strPNPString[256];
    int  iOSDisplayIndex;
};

// Identifiants de capteurs ADL2_New_QueryPMLogData
// (ADL_PMLOG_ASIC_POWER = 1 dans adl_defines.h)
#define ADL_PMLOG_ASIC_POWER      1
#define ADL_PMLOG_CLK_GFXCLK     13
#define ADL_PMLOG_TEMPERATURE_EDGE 7
#define ADL_PMLOG_MAX_SENSORS     256

struct AdlPmLogData {
    unsigned int ulVersion;
    unsigned int ulActiveSampleRate;
    long long    ulLastUpdated;
    unsigned int ulValues[ADL_PMLOG_MAX_SENSORS][2]; // [0]=valeur, [1]=support
};

struct AdlPmLogSupportList {
    unsigned int uSupported[ADL_PMLOG_MAX_SENSORS]; // bitmask
};

// Pointeurs de fonctions ADL2
typedef int  (__stdcall *ADL2_Main_Control_Create_t)
    (void* (*)(int), int, ADL_CONTEXT_HANDLE*);
typedef int  (__stdcall *ADL2_Main_Control_Destroy_t)
    (ADL_CONTEXT_HANDLE);
typedef int  (__stdcall *ADL2_Adapter_NumberOfAdapters_Get_t)
    (ADL_CONTEXT_HANDLE, int*);
typedef int  (__stdcall *ADL2_Adapter_AdapterInfo_Get_t)
    (ADL_CONTEXT_HANDLE, AdlAdapterInfo*, int);
typedef int  (__stdcall *ADL2_Adapter_Active_Get_t)
    (ADL_CONTEXT_HANDLE, int, int*);
typedef int  (__stdcall *ADL2_New_QueryPMLogData_Get_t)
    (ADL_CONTEXT_HANDLE, int, AdlPmLogData*);
// API plus ancienne (Overdrive6) — fallback si la nouvelle échoue
typedef int  (__stdcall *ADL2_Overdrive6_CurrentPower_Get_t)
    (ADL_CONTEXT_HANDLE, int, int, int*);

// Allocateur demandé par ADL (ADL_Main_Memory_Alloc)
static void* __stdcall AdlAlloc(int size) { return malloc(size); }

struct AdlCtx {
    HMODULE   dll      = nullptr;
    ADL_CONTEXT_HANDLE ctx = nullptr;
    int       adapterIdx = -1;   // premier adaptateur AMD actif
    bool      ready    = false;
    bool      usePmLog = false;  // true = nouvelle API, false = OD6

    // Fonctions chargées
    ADL2_Main_Control_Create_t         pfCreate  = nullptr;
    ADL2_Main_Control_Destroy_t        pfDestroy = nullptr;
    ADL2_Adapter_NumberOfAdapters_Get_t pfNumAdapt= nullptr;
    ADL2_Adapter_AdapterInfo_Get_t     pfAdaptInfo=nullptr;
    ADL2_Adapter_Active_Get_t          pfActive  = nullptr;
    ADL2_New_QueryPMLogData_Get_t      pfPmLog   = nullptr;
    ADL2_Overdrive6_CurrentPower_Get_t pfOD6Pwr  = nullptr;

    bool init() {
        // Cherche la DLL 64-bit dans les emplacements AMD connus
        const wchar_t* paths[] = {
            L"atiadlxx.dll",                                           // PATH
            L"C:\\Windows\\System32\\atiadlxx.dll",
            L"C:\\Program Files\\AMD\\CNext\\CNext\\atiadlxx.dll",
            L"C:\\Program Files (x86)\\AMD\\CNext\\CNext\\atiadlxx.dll",
        };
        for (auto p : paths) {
            dll = LoadLibraryW(p);
            if (dll) break;
        }
        if (!dll) return false;  // drivers AMD non installés

        // Charge les symboles
        pfCreate    = (ADL2_Main_Control_Create_t)
                       GetProcAddress(dll,"ADL2_Main_Control_Create");
        pfDestroy   = (ADL2_Main_Control_Destroy_t)
                       GetProcAddress(dll,"ADL2_Main_Control_Destroy");
        pfNumAdapt  = (ADL2_Adapter_NumberOfAdapters_Get_t)
                       GetProcAddress(dll,"ADL2_Adapter_NumberOfAdapters_Get");
        pfAdaptInfo = (ADL2_Adapter_AdapterInfo_Get_t)
                       GetProcAddress(dll,"ADL2_Adapter_AdapterInfo_Get");
        pfActive    = (ADL2_Adapter_Active_Get_t)
                       GetProcAddress(dll,"ADL2_Adapter_Active_Get");
        pfPmLog     = (ADL2_New_QueryPMLogData_Get_t)
                       GetProcAddress(dll,"ADL2_New_QueryPMLogData_Get");
        pfOD6Pwr    = (ADL2_Overdrive6_CurrentPower_Get_t)
                       GetProcAddress(dll,"ADL2_Overdrive6_CurrentPower_Get");

        if (!pfCreate || !pfDestroy || !pfNumAdapt || !pfAdaptInfo)
            return false;

        // Crée le contexte ADL
        if (pfCreate(AdlAlloc, 1, &ctx) != 0) return false;

        // Trouve le premier adaptateur AMD actif
        int numAdapt = 0;
        pfNumAdapt(ctx, &numAdapt);
        if (numAdapt <= 0) return false;

        std::vector<AdlAdapterInfo> infos(numAdapt);
        infos[0].iSize = sizeof(AdlAdapterInfo);
        pfAdaptInfo(ctx, infos.data(), (int)(sizeof(AdlAdapterInfo)*numAdapt));

        for (int i = 0; i < numAdapt; i++) {
            int active = 0;
            if (pfActive && pfActive(ctx, i, &active) == 0 && active) {
                // Vérifie que c'est AMD (vendor 0x1002)
                if (infos[i].iVendorID == 0x1002) {
                    adapterIdx = i;
                    break;
                }
            }
        }
        if (adapterIdx < 0) {
            // Fallback : prend le premier sans vérifier active
            for (int i = 0; i < numAdapt; i++) {
                if (infos[i].iVendorID == 0x1002) {
                    adapterIdx = i; break;
                }
            }
        }
        if (adapterIdx < 0) return false;

        // Teste quelle API est disponible
        if (pfPmLog) {
            AdlPmLogData tmp{};
            tmp.ulVersion = sizeof(AdlPmLogData);
            int r = pfPmLog(ctx, adapterIdx, &tmp);
            // r==0 → OK, r==-8 → feature non dispo
            usePmLog = (r == 0);
        }

        ready = true;
        return true;
    }

    // Retourne la puissance GPU en W, ou -1 si non disponible
    double readPowerW() {
        if (!ready || adapterIdx < 0) return -1.0;

        if (usePmLog && pfPmLog) {
            AdlPmLogData d{};
            d.ulVersion = sizeof(AdlPmLogData);
            if (pfPmLog(ctx, adapterIdx, &d) == 0) {
                // ulValues[ADL_PMLOG_ASIC_POWER][1] = 1 si la valeur est valide
                if (d.ulValues[ADL_PMLOG_ASIC_POWER][1] == 1) {
                    return (double)d.ulValues[ADL_PMLOG_ASIC_POWER][0];
                }
            }
        }

        // Fallback : Overdrive6 — valeur en milliwatts × 100
        // ADL_OD6_POWER_TYPE_ASIC = 0
        if (pfOD6Pwr) {
            int val = 0;
            if (pfOD6Pwr(ctx, adapterIdx, 0, &val) == 0 && val > 0) {
                return val / 100.0;  // milliwatts/100 → watts
            }
        }

        return -1.0;  // non disponible → l'appelant utilise l'estimation TDP
    }

    ~AdlCtx() {
        if (ctx && pfDestroy) pfDestroy(ctx);
        if (dll) FreeLibrary(dll);
    }
};

static AdlCtx g_Adl;

// Thread capteurs — tourne à 1 Hz
static void sensorThread() {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);

    PdhCtx pdh;
    pdh.init();
    Sleep(1000); // premier intervalle PDH

    // Init static info
    bool wmiOk = initWmi();
    std::wstring cpuName = wmiGetStr(L"Win32_Processor",L"Name");
    // Trim espaces multiples
    {
        std::wstring tmp; bool sp=false;
        for(wchar_t c:cpuName){if(c==L' '){if(!sp){tmp+=c;sp=true;}}else{tmp+=c;sp=false;}}
        while(!tmp.empty()&&tmp.back()==L' ') tmp.pop_back();
        cpuName=tmp;
    }

    SYSTEM_INFO si{}; GetSystemInfo(&si);
    int cores = (int)si.dwNumberOfProcessors;

    std::wstring gpuNameDxgi; size_t vramBytes=0;
    dxgiGetGpuInfo(gpuNameDxgi,vramBytes);
    std::wstring gpuNameWmi = wmiGetStr(L"Win32_VideoController",L"Name");
    std::wstring gpuName = gpuNameDxgi.empty() ? gpuNameWmi : gpuNameDxgi;

    // Initialisation ADL — lecture puissance réelle AMD
    bool adlOk = g_Adl.init();

    double cumWh=0;
    auto t0=std::chrono::steady_clock::now();
    int usbRefresh=0;
    std::vector<std::pair<std::wstring,double>> usbDevs;
    enumUsb(usbDevs);

    while(g_Running) {
        PdhCollectQueryData(pdh.q);

        double cpuLoad  = pdh.readScalar(pdh.cCpuLoad);
        double gpuLoad  = pdh.readGpuLoad();
        double diskRd   = pdh.readScalar(pdh.cDiskRead)  / (1024.0*1024.0); // MB/s
        double diskWr   = pdh.readScalar(pdh.cDiskWrite) / (1024.0*1024.0);
        double netUp    = pdh.readNetSum(pdh.cNetSent)   / 1024.0; // KB/s
        double netDn    = pdh.readNetSum(pdh.cNetRecv)   / 1024.0;

        double cpuFreq  = wmiGetCpuFreq();

        // RAM
        MEMORYSTATUSEX ms{}; ms.dwLength=sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        double ramTotal = (double)ms.ullTotalPhys/(1<<30);
        double ramUsed  = ramTotal - (double)ms.ullAvailPhys/(1<<30);
        double ramPct   = ms.dwMemoryLoad;
        // DDR4 : ~3.5W par 8 Go
        double ramWatt  = (ramTotal/8.0)*3.5;

        // Batterie
        SYSTEM_POWER_STATUS pwr{};
        GetSystemPowerStatus(&pwr);
        bool onBat  = (pwr.ACLineStatus==0);
        int battPct = (pwr.BatteryLifePercent==255)?-1:(int)pwr.BatteryLifePercent;
        bool charg  = (pwr.BatteryFlag & 0x08)!=0;

        // CPU — estimation TDP × usage (RAPL non exposé en user-mode sans WinRing0)
        double cpuW = g_CpuTdpIdle+(g_CpuTdpMax-g_CpuTdpIdle)*(cpuLoad/100.0);

        // GPU — lecture ADL réelle (drivers AMD), fallback TDP×% si indispo
        double adlW = g_Adl.readPowerW();
        double gpuW = (adlW >= 0.0) ? adlW
                    : g_GpuTdpIdle+(g_GpuTdpMax-g_GpuTdpIdle)*(gpuLoad/100.0);

        // USB
        if (++usbRefresh>=15) { enumUsb(usbDevs); usbRefresh=0; }
        double usbW=0; for(auto&d:usbDevs) usbW+=d.second;

        // Écrans
        int monCnt=0; double monW=0;
        std::vector<MonitorInfo> monList;
        enumMonitors(monList,monCnt,monW);

        // Total
        double total = cpuW+gpuW+ramWatt+usbW+monW+g_SysBase;
        cumWh += total/3600.0;

        auto now=std::chrono::steady_clock::now();
        int64_t sec=std::chrono::duration_cast<std::chrono::seconds>(now-t0).count();

        {
            std::lock_guard<std::mutex> lk(g_Mtx);
            g_Sensor.cpuLoad   = cpuLoad;
            g_Sensor.cpuWatt   = cpuW;
            g_Sensor.cpuFreqMhz= cpuFreq;
            g_Sensor.cpuName   = cpuName;
            g_Sensor.cpuCores  = cores;
            g_Sensor.gpuLoad     = gpuLoad;
            g_Sensor.gpuWatt     = gpuW;
            g_Sensor.gpuWattReal = (adlW >= 0.0);
            g_Sensor.gpuName     = gpuName;
            g_Sensor.gpuVramBytes=vramBytes;
            g_Sensor.gpuMemTotalMB=(double)vramBytes/(1024*1024);
            g_Sensor.ramUsedGB = ramUsed;
            g_Sensor.ramTotalGB= ramTotal;
            g_Sensor.ramPct    = ramPct;
            g_Sensor.ramWatt   = ramWatt;
            g_Sensor.diskReadMBs  = diskRd;
            g_Sensor.diskWriteMBs = diskWr;
            g_Sensor.netUpKBs  = netUp;
            g_Sensor.netDownKBs= netDn;
            g_Sensor.onBattery = onBat;
            g_Sensor.battPct   = battPct;
            g_Sensor.charging  = charg;
            g_Sensor.usbDevices= usbDevs;
            g_Sensor.usbCount  = (int)usbDevs.size();
            g_Sensor.usbWatt   = usbW;
            g_Sensor.monitorCount = monCnt;
            g_Sensor.monitorWatt  = monW;
            g_Sensor.monitors     = monList;
            g_Sensor.totalWatt = total;
            g_Sensor.cumWhSession = cumWh;
            g_Sensor.sessionSec= sec;

            g_History.push_back(total);
            if((int)g_History.size()>HIST) g_History.pop_front();
        }

        Sleep(1000);
    }
}

// ─────────────────────────────────────────────────────────────────
//  RENDU DIRECT2D
// ─────────────────────────────────────────────────────────────────
struct D2DCtx {
    ID2D1Factory*          factory   = nullptr;
    ID2D1HwndRenderTarget* rt        = nullptr;
    IDWriteFactory*        dwrite    = nullptr;
    IDWriteTextFormat*     fmtBig    = nullptr;  // valeur principale
    IDWriteTextFormat*     fmtMed    = nullptr;  // label
    IDWriteTextFormat*     fmtSmall  = nullptr;  // petits labels
    IDWriteTextFormat*     fmtMono   = nullptr;  // mono

    // Brushes (recréés à chaque frame si nécessaire → on les garde)
    std::map<UINT32,ID2D1SolidColorBrush*> brushes;

    bool init(HWND hw) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&factory);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),(IUnknown**)&dwrite);

        RECT rc; GetClientRect(hw,&rc);
        D2D1_SIZE_U sz = D2D1::SizeU(rc.right,rc.bottom);
        D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties();
        D2D1_HWND_RENDER_TARGET_PROPERTIES hrtp = D2D1::HwndRenderTargetProperties(hw,sz);
        factory->CreateHwndRenderTarget(rtp,hrtp,&rt);

        dwrite->CreateTextFormat(L"Segoe UI",nullptr,
            DWRITE_FONT_WEIGHT_BOLD,DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,28,L"fr-FR",&fmtBig);
        dwrite->CreateTextFormat(L"Segoe UI",nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,13,L"fr-FR",&fmtMed);
        dwrite->CreateTextFormat(L"Segoe UI",nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,11,L"fr-FR",&fmtSmall);
        dwrite->CreateTextFormat(L"Consolas",nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,11,L"fr-FR",&fmtMono);

        return rt != nullptr;
    }

    ID2D1SolidColorBrush* brush(D2D1_COLOR_F c) {
        UINT32 key = (UINT32)(c.r*255)<<16|(UINT32)(c.g*255)<<8|(UINT32)(c.b*255);
        auto it=brushes.find(key);
        if(it!=brushes.end()) return it->second;
        ID2D1SolidColorBrush* b=nullptr;
        rt->CreateSolidColorBrush(c,&b);
        brushes[key]=b;
        return b;
    }

    void resize(HWND hw) {
        RECT rc; GetClientRect(hw,&rc);
        if(rt) rt->Resize(D2D1::SizeU(rc.right,rc.bottom));
    }

    ~D2DCtx() {
        for(auto&p:brushes) if(p.second) p.second->Release();
        if(fmtMono) fmtMono->Release();
        if(fmtSmall)fmtSmall->Release();
        if(fmtMed)  fmtMed->Release();
        if(fmtBig)  fmtBig->Release();
        if(dwrite)  dwrite->Release();
        if(rt)      rt->Release();
        if(factory) factory->Release();
    }
};

static D2DCtx g_D2D;

// Helper : dessiner texte
static void DText(const wchar_t* t, D2D1_RECT_F r, ID2D1SolidColorBrush* b,
                  IDWriteTextFormat* f, bool center=false) {
    if(!t||!b||!f) return;
    if(center) {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    g_D2D.rt->DrawText(t,(UINT32)wcslen(t),f,r,b);
}

// Helper : rectangle arrondi rempli
static void FillRoundRect(D2D1_RECT_F r, float rad, D2D1_COLOR_F col) {
    auto* b = g_D2D.brush(col);
    g_D2D.rt->FillRoundedRectangle(D2D1::RoundedRect(r,rad,rad),b);
}
static void DrawRoundRect(D2D1_RECT_F r, float rad, D2D1_COLOR_F col, float sw=1.0f) {
    auto* b = g_D2D.brush(col);
    g_D2D.rt->DrawRoundedRectangle(D2D1::RoundedRect(r,rad,rad),b,sw);
}

// ─────────────────────────────────────────────────────────────────
//  WIDGETS DE BASE
// ─────────────────────────────────────────────────────────────────

// Barre de progression horizontale
static void DrawBar(float x,float y,float w,float h,
                    double pct,D2D1_COLOR_F fillCol) {
    D2D1_RECT_F bg={x,y,x+w,y+h};
    D2D1_RECT_F fg={x,y,x+(float)(w*pct/100.0),y+h};
    FillRoundRect(bg,3,C::BORDER);
    if(pct>0) FillRoundRect(fg,3,fillCol);
}

// Courbe de puissance
static void DrawGraph(float x,float y,float w,float h,
                      const std::deque<double>& hist,
                      double maxW,D2D1_COLOR_F lineCol) {
    // Fond
    D2D1_RECT_F bg={x,y,x+w,y+h};
    FillRoundRect(bg,6,{0.035f,0.043f,0.067f,1});

    if(hist.size()<2) return;

    // Grilles horizontales
    auto bMuted=g_D2D.brush(C::BORDER);
    for(int i=1;i<4;i++){
        float gy=y+h*(1.0f-i/4.0f);
        g_D2D.rt->DrawLine({x+8,gy},{x+w-4,gy},bMuted,0.5f);
        // Label watt
        wchar_t buf[16]; swprintf_s(buf,L"%.0f",maxW*i/4.0);
        DText(buf,{x+4,gy-8,x+40,gy+8},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
    }

    int n=(int)hist.size();
    float stepX = w / (float)(HIST-1);
    float offX  = x + (HIST-n)*stepX;

    // Remplissage sous la courbe
    ID2D1PathGeometry* geo=nullptr;
    g_D2D.factory->CreatePathGeometry(&geo);
    ID2D1GeometrySink* sink=nullptr;
    geo->Open(&sink);
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);

    float fy0=y+h-(float)std::clamp(hist[0]/maxW,0.0,1.0)*(h-4)-2;
    sink->BeginFigure({offX,fy0},D2D1_FIGURE_BEGIN_FILLED);
    for(int i=1;i<n;i++){
        float px=offX+i*stepX;
        float py=y+h-(float)std::clamp(hist[i]/maxW,0.0,1.0)*(h-4)-2;
        sink->AddLine({px,py});
    }
    sink->AddLine({offX+(n-1)*stepX,y+h});
    sink->AddLine({offX,y+h});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close(); sink->Release();

    // Gradient fill
    D2D1_COLOR_F fillA={lineCol.r,lineCol.g,lineCol.b,0.18f};
    g_D2D.rt->FillGeometry(geo,g_D2D.brush(fillA));
    // Ligne
    g_D2D.rt->DrawGeometry(geo,g_D2D.brush(lineCol),1.8f);
    geo->Release();

    // Point courant
    if(n>0){
        float px=offX+(n-1)*stepX;
        float py=y+h-(float)std::clamp(hist[n-1]/maxW,0.0,1.0)*(h-4)-2;
        D2D1_ELLIPSE el=D2D1::Ellipse({px,py},4,4);
        g_D2D.rt->FillEllipse(el,g_D2D.brush(lineCol));
        g_D2D.rt->DrawEllipse(el,g_D2D.brush(C::BG),1.5f);
    }
}

// Carte métrique : label + grosse valeur + barre + sous-info
struct Card {
    float x,y,w,h;

    void draw(const wchar_t* label,
              const wchar_t* value,
              const wchar_t* unit,
              const wchar_t* sub,
              double barPct,
              D2D1_COLOR_F accent) const
    {
        // Fond + bordure
        D2D1_RECT_F r={x,y,x+w,y+h};
        FillRoundRect(r,8,C::PANEL);
        // Trait coloré en haut
        D2D1_RECT_F top={x+1,y,x+w-1,y+3};
        FillRoundRect(top,2,accent);

        float px=x+14, py=y+10;
        // Label
        DText(label,{px,py,x+w-8,py+16},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        py+=20;
        // Valeur principale
        DText(value,{px,py,x+w-50,py+36},g_D2D.brush(accent),g_D2D.fmtBig);
        // Unité
        DText(unit,{x+w-60,py+14,x+w-6,py+36},g_D2D.brush(C::MUTED),g_D2D.fmtMed);
        py+=42;
        // Barre
        if(barPct>=0)
            DrawBar(px,py,w-28,6,std::clamp(barPct,0.0,100.0),accent);
        py+=14;
        // Sous-info
        DText(sub,{px,py,x+w-8,py+20},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);
    }
};

// ─────────────────────────────────────────────────────────────────
//  SCROLL Y & layout
// ─────────────────────────────────────────────────────────────────
static int  g_ScrollY  = 0;
static int  g_ContentH = 0;
static HWND g_Hwnd     = nullptr;

// ─────────────────────────────────────────────────────────────────
//  RENDU COMPLET
// ─────────────────────────────────────────────────────────────────
static void Render(HWND hw) {
    if(!g_D2D.rt) return;

    g_D2D.rt->BeginDraw();
    g_D2D.rt->Clear(C::BG);

    // Snapshot thread-safe
    SensorData s;
    std::deque<double> hist;
    {
        std::lock_guard<std::mutex> lk(g_Mtx);
        s=g_Sensor; hist=g_History;
    }

    RECT wrc; GetClientRect(hw,&wrc);
    float W=(float)wrc.right, H=(float)wrc.bottom;
    float oy=(float)-g_ScrollY;   // offset scroll

    // ── HEADER ──────────────────────────────────────────────────
    {
        D2D1_RECT_F hdr={0,oy,W,oy+50};
        FillRoundRect(hdr,0,{0.035f,0.043f,0.067f,1});

        // Titre
        DText(L"⚡ POWERMON",{14,oy+8,200,oy+42},
              g_D2D.brush(C::CYAN),g_D2D.fmtMed);

        // Heure
        SYSTEMTIME st{}; GetLocalTime(&st);
        wchar_t tbuf[32];
        swprintf_s(tbuf,L"%02d:%02d:%02d",st.wHour,st.wMinute,st.wSecond);
        DText(tbuf,{W-110,oy+8,W-8,oy+42},g_D2D.brush(C::MUTED),g_D2D.fmtMed);

        // Session
        wchar_t sbuf[64];
        int64_t sec=s.sessionSec;
        swprintf_s(sbuf,L"Session  %02lld:%02lld:%02lld",sec/3600,(sec%3600)/60,sec%60);
        DText(sbuf,{200,oy+14,W-120,oy+36},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);

        // Batterie ou secteur
        if(s.battPct>=0){
            wchar_t bb[32];
            swprintf_s(bb,L"%s %d%%",s.charging?L"🔌":L"🔋",(int)s.battPct);
            DText(bb,{W-240,oy+8,W-110,oy+42},g_D2D.brush(s.charging?C::GREEN:C::ORANGE),g_D2D.fmtSmall);
        } else {
            DText(L"🔌 Secteur",{W-240,oy+8,W-110,oy+42},
                  g_D2D.brush(C::GREEN),g_D2D.fmtSmall);
        }

        // Ligne séparatrice
        g_D2D.rt->DrawLine({0,oy+50},{W,oy+50},g_D2D.brush(C::BORDER),1);
    }

    float curY=oy+60;
    float pad=12, cw=(W-pad*3)/2;

    // ── PUISSANCE TOTALE (pleine largeur) ───────────────────────
    {
        D2D1_RECT_F r={pad,curY,W-pad,curY+100};
        FillRoundRect(r,8,C::PANEL);
        D2D1_RECT_F top={pad+1,curY,W-pad-1,curY+3};
        // Gradient couleur selon charge
        D2D1_COLOR_F col = s.totalWatt>250?C::RED:s.totalWatt>150?C::ORANGE:C::CYAN;
        FillRoundRect(top,2,col);

        DText(L"PUISSANCE TOTALE ESTIMÉE",{pad+14,curY+8,W/2,curY+24},
              g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);

        wchar_t wbuf[32]; swprintf_s(wbuf,L"%.1f",s.totalWatt);
        DText(wbuf,{pad+14,curY+20,pad+200,curY+80},g_D2D.brush(col),g_D2D.fmtBig);
        DText(L"W",{pad+180,curY+50,pad+220,curY+80},g_D2D.brush(C::MUTED),g_D2D.fmtMed);

        // Stats mini
        double mn=hist.empty()?0:*std::min_element(hist.begin(),hist.end());
        double mx=hist.empty()?0:*std::max_element(hist.begin(),hist.end());
        double av=hist.empty()?0:std::accumulate(hist.begin(),hist.end(),0.0)/hist.size();
        wchar_t sbb[128];
        swprintf_s(sbb,L"Min %.0fW   Moy %.0fW   Max %.0fW",mn,av,mx);
        DText(sbb,{pad+14,curY+78,W/2,curY+96},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);

        // Répartition droite
        float rx=W/2+20, ry=curY+12, rw=W/2-pad-30;
        auto drawSeg=[&](const wchar_t* lbl,double watt,D2D1_COLOR_F c,float& yy){
            DText(lbl,{rx,yy,rx+120,yy+14},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
            wchar_t tmp[16]; swprintf_s(tmp,L"%.1fW",watt);
            DText(tmp,{rx+rw-50,yy,rx+rw,yy+14},g_D2D.brush(c),g_D2D.fmtSmall);
            DrawBar(rx,yy+16,rw,4,s.totalWatt>0?watt/s.totalWatt*100:0,c);
            yy+=26;
        };
        float yy=ry;
        drawSeg(L"CPU",        s.cpuWatt,  C::ORANGE,yy);
        drawSeg(L"GPU",        s.gpuWatt,  C::CYAN,  yy);
        drawSeg(L"RAM",        s.ramWatt,  C::GREEN, yy);
        drawSeg(L"USB",        s.usbWatt,  C::PURPLE,yy);
        drawSeg(L"Écrans",     s.monitorWatt,C::YELLOW,yy);
    }
    curY+=108;

    // ── GRAPHE PUISSANCE ────────────────────────────────────────
    {
        float gh=110;
        DrawGraph(pad,curY,W-pad*2,gh,hist,
                  std::max(400.0,s.totalWatt*1.3),C::CYAN);
        // Label axe X
        wchar_t xl[32]; swprintf_s(xl,L"← %d s",HIST);
        DText(xl,{pad+6,curY+gh-14,pad+80,curY+gh-2},
              g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        DText(L"maintenant →",{W-pad-90,curY+gh-14,W-pad,curY+gh-2},
              g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        curY+=gh+10;
    }

    // ── CPU ─────────────────────────────────────────────────────
    {
        Card c={pad,curY,cw,115};
        wchar_t v[16],sub[128];
        swprintf_s(v,L"%.1f",s.cpuWatt);
        swprintf_s(sub,L"%.0f%% charge  |  %.0f MHz  |  %d cœurs",
                   s.cpuLoad,s.cpuFreqMhz,s.cpuCores);
        c.draw(s.cpuName.empty()?L"PROCESSEUR":s.cpuName.c_str(),
               v,L"W",sub,s.cpuLoad,C::ORANGE);
        curY+=0; // on reste sur la même ligne pour GPU
    }

    // ── GPU ─────────────────────────────────────────────────────
    {
        Card c={pad*2+cw,curY,cw,115};
        wchar_t v[16],sub[128];
        swprintf_s(v,L"%.1f",s.gpuWatt);
        double vramGB=(double)s.gpuVramBytes/(1<<30);
        // Indique la source : ADL réel ou estimation TDP
        const wchar_t* src = s.gpuWattReal ? L"ADL réel" : L"estim. TDP";
        swprintf_s(sub,L"%.0f%% charge  |  %.1f Go VRAM  |  %s",
                   s.gpuLoad, vramGB, src);
        c.draw(s.gpuName.empty()?L"GPU":s.gpuName.c_str(),
               v,L"W",sub,s.gpuLoad,C::CYAN);
    }
    curY+=122;

    // ── RAM ─────────────────────────────────────────────────────
    {
        Card c={pad,curY,cw,115};
        wchar_t v[16],sub[128];
        swprintf_s(v,L"%.1f",s.ramWatt);
        swprintf_s(sub,L"%.1f / %.1f Go  (%.0f%%)  |  ~%.1fW",
                   s.ramUsedGB,s.ramTotalGB,s.ramPct,s.ramWatt);
        c.draw(L"MÉMOIRE RAM",v,L"W",sub,s.ramPct,C::GREEN);
    }

    // ── DISQUE ──────────────────────────────────────────────────
    {
        Card c={pad*2+cw,curY,cw,115};
        wchar_t v[16],sub[128];
        double diskWatt=0.5+s.diskReadMBs*0.01+s.diskWriteMBs*0.01; // estimation NVMe
        swprintf_s(v,L"%.1f",diskWatt);
        swprintf_s(sub,L"Lect %.1f Mo/s  |  Écrit %.1f Mo/s",
                   s.diskReadMBs,s.diskWriteMBs);
        c.draw(L"STOCKAGE (NVMe/SSD)",v,L"W",sub,
               std::clamp((s.diskReadMBs+s.diskWriteMBs)/20.0*100,0.0,100.0),C::YELLOW);
    }
    curY+=122;

    // ── RÉSEAU ──────────────────────────────────────────────────
    {
        Card c={pad,curY,cw,115};
        wchar_t v[16],sub[128];
        swprintf_s(v,L"2.5"); // carte réseau ~2.5W
        swprintf_s(sub,L"↑ %.0f Ko/s  |  ↓ %.0f Ko/s",s.netUpKBs,s.netDownKBs);
        c.draw(L"RÉSEAU",v,L"W",sub,
               std::clamp((s.netUpKBs+s.netDownKBs)/5000.0*100,0.0,100.0),C::PURPLE);
    }

    // ── ÉCRANS ──────────────────────────────────────────────────
    {
        // Hauteur dynamique : header 30 + 78px par écran (5 lignes), min 115
        float rowH = 78.0f;
        float mh   = std::max(115.0f, 30.0f + (float)s.monitors.size() * rowH);

        // Panneau pleine largeur
        D2D1_RECT_F r={pad,curY,W-pad,curY+mh};
        FillRoundRect(r,8,C::PANEL);
        D2D1_RECT_F top={pad+1,curY,W-pad-1,curY+3};
        FillRoundRect(top,2,C::YELLOW);

        wchar_t hdr[64];
        swprintf_s(hdr,L"AFFICHAGE  —  %d écran(s)  ~%.0f W total",
                   s.monitorCount,s.monitorWatt);
        DText(hdr,{pad+14,curY+8,W-20,curY+26},
              g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);

        float my = curY+32;
        float colW = (W - pad*2 - 28) / std::max(1,(int)s.monitors.size());

        for (int i=0;i<(int)s.monitors.size();i++) {
            auto& m = s.monitors[i];
            float mx = pad+14 + i*colW;

            // Cadre par écran — bordure jaune si primaire
            D2D1_RECT_F mr={mx-4, my-2, mx+colW-8, my+rowH-4};
            FillRoundRect(mr,4,{0.035f,0.043f,0.067f,1});
            if(m.primary) DrawRoundRect(mr,4,C::YELLOW,0.8f);

            // ── Ligne 1 : Marque  (si connue)
            if(!m.edidMfr.empty()) {
                std::wstring brand = m.edidMfr;
                if(brand.size()>18) brand=brand.substr(0,16)+L"…";
                // Badge marque coloré
                bool isKoorui=(m.edidMfr==L"Koorui"||m.edidMfr==L"Koorui/RTK");
                D2D1_COLOR_F bc = isKoorui ? C::CYAN : C::GREEN;
                DText(brand.c_str(),{mx,my,mx+colW-10,my+14},
                      g_D2D.brush(bc),g_D2D.fmtSmall);
            }

            // ── Ligne 2 : Nom modèle (EDID 0xFC ou fallback)
            {
                std::wstring name = m.friendlyName.empty()
                                  ? m.deviceName : m.friendlyName;
                // Enlève le préfixe marque si déjà affiché ligne 1
                if(!m.edidMfr.empty()) {
                    std::wstring lo=name, mfr=m.edidMfr;
                    for(auto&c:lo)  c=towlower(c);
                    for(auto&c:mfr) c=towlower(c);
                    if(lo.find(mfr)!=std::wstring::npos)
                        name=name.substr(m.edidMfr.size());
                    while(!name.empty()&&(name[0]==L' '||name[0]==L'-'))
                        name=name.substr(1);
                }
                if(name.size()>26) name=name.substr(0,24)+L"…";
                float ly = m.edidMfr.empty() ? my : my+14;
                DText(name.c_str(),{mx,ly,mx+colW-10,ly+14},
                      g_D2D.brush(m.primary?C::YELLOW:C::WHITE),g_D2D.fmtSmall);
            }

            // ── Ligne 3 : Résolution + fréquence
            {
                wchar_t rl[48];
                if(m.refreshHz>0)
                    swprintf_s(rl,L"%d×%d  @%dHz",m.widthPx,m.heightPx,m.refreshHz);
                else
                    swprintf_s(rl,L"%d×%d",m.widthPx,m.heightPx);
                DText(rl,{mx,my+28,mx+colW-10,my+41},
                      g_D2D.brush(C::MUTED),g_D2D.fmtSmall);
            }

            // ── Ligne 4 : Taille physique + diagonale
            {
                wchar_t sl[48];
                if(m.diagInch>0)
                    swprintf_s(sl,L"%.1f\"  %dmm × %dmm",
                               m.diagInch,m.widthMm,m.heightMm);
                else
                    swprintf_s(sl,L"Taille EDID inconnue");
                DText(sl,{mx,my+42,mx+colW-10,my+55},
                      g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
            }

            // ── Ligne 5 : Puissance + source (mesurée ou estimée)
            {
                // Détermine si on a une valeur Koorui connue ou estimée
                bool isKoorui=(m.edidMfr==L"Koorui"||m.edidMfr==L"Koorui/RTK");
                double kwCheck = kooruiWattFromModel(m.friendlyName);
                bool hasExact  = isKoorui && kwCheck > 0;
                wchar_t pw[48];
                swprintf_s(pw, L"~%.0f W  %s%s",
                           m.estWattW,
                           hasExact ? L"[datasheet]" : L"[estimé]",
                           m.primary ? L"  ★ Principal" : L"");
                DText(pw,{mx,my+56,mx+colW-10,my+68},
                      g_D2D.brush(C::YELLOW),g_D2D.fmtSmall);
            }
        }
        curY+=mh+10;
    }

    // ── USB ─────────────────────────────────────────────────────
    {
        float uh=std::max(115.0f,32.0f+s.usbDevices.size()*18.0f);
        D2D1_RECT_F r={pad,curY,W-pad,curY+uh};
        FillRoundRect(r,8,C::PANEL);
        D2D1_RECT_F top={pad+1,curY,W-pad-1,curY+3};
        FillRoundRect(top,2,C::PURPLE);

        wchar_t lbl[64]; swprintf_s(lbl,L"USB (%d périphériques — ~%.1fW)",
                                     s.usbCount,s.usbWatt);
        DText(lbl,{pad+14,curY+8,W-20,curY+24},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);

        float dy=curY+26;
        float col2=W/2;
        for(int i=0;i<(int)s.usbDevices.size();i++){
            auto& d=s.usbDevices[i];
            float fx=(i%2==0)?pad+20:col2+10;
            float fy=dy+(i/2)*18.0f;
            // Nom (tronqué)
            std::wstring nm=d.first;
            if(nm.size()>32) nm=nm.substr(0,30)+L"…";
            DText(nm.c_str(),{fx,fy,fx+280,fy+16},
                  g_D2D.brush(C::WHITE),g_D2D.fmtSmall);
            wchar_t wb[16]; swprintf_s(wb,L"~%.1fW",d.second);
            DText(wb,{fx+285,fy,fx+340,fy+16},g_D2D.brush(C::PURPLE),g_D2D.fmtSmall);
        }
        curY+=uh+10;
    }

    // ── BILAN ÉNERGIE / COÛT ────────────────────────────────────
    {
        float bh=120;
        D2D1_RECT_F r={pad,curY,W-pad,curY+bh};
        FillRoundRect(r,8,C::PANEL);
        D2D1_RECT_F top={pad+1,curY,W-pad-1,curY+3};
        FillRoundRect(top,2,C::GREEN);
        DText(L"BILAN ÉNERGÉTIQUE",{pad+14,curY+8,W/2,curY+24},
              g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);

        double kwh=s.cumWhSession/1000.0;
        double cost=kwh*g_TarifKwh;
        double avg=hist.empty()?s.totalWatt:
            std::accumulate(hist.begin(),hist.end(),0.0)/hist.size();
        double c24=avg*24.0/1000.0*g_TarifKwh;
        double cMo=c24*30.0;

        // Col 1 : énergie
        wchar_t tmp[64];
        float cy=curY+24;
        swprintf_s(tmp,L"Cumulé : %.4f Wh  (%.8f kWh)",s.cumWhSession,kwh);
        DText(tmp,{pad+14,cy,W*0.6f,cy+16},g_D2D.brush(C::WHITE),g_D2D.fmtSmall); cy+=20;
        swprintf_s(tmp,L"Tarif  : %.4f €/kWh",g_TarifKwh);
        DText(tmp,{pad+14,cy,W*0.6f,cy+16},g_D2D.brush(C::MUTED),g_D2D.fmtSmall); cy+=20;
        swprintf_s(tmp,L"Proj. 24h : %.3f €    Mois : %.2f €",c24,cMo);
        DText(tmp,{pad+14,cy,W*0.6f,cy+16},g_D2D.brush(C::MUTED),g_D2D.fmtSmall);

        // Col 2 : coût
        wchar_t cbuf[32]; swprintf_s(cbuf,L"%.5f €",cost);
        DText(L"COÛT SESSION",{W*0.65f,curY+24,W-20,curY+40},
              g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        DText(cbuf,{W*0.65f,curY+40,W-20,curY+90},
              g_D2D.brush(C::ORANGE),g_D2D.fmtBig);

        curY+=bh+10;
    }

    // ── FOOTER ──────────────────────────────────────────────────
    {
        D2D1_RECT_F r={0,curY,W,curY+28};
        FillRoundRect(r,0,{0.035f,0.043f,0.067f,1});
        DText(L"⚠ Estimation TDP×usage — précision ±15W   |   PowerMon v2.0   |   Win32 + Direct2D",
              {14,curY+6,W-14,curY+22},g_D2D.brush(C::DIMTEXT),g_D2D.fmtSmall);
        curY+=28;
    }

    g_ContentH=(int)(curY+g_ScrollY+20);

    g_D2D.rt->EndDraw();
}

// ─────────────────────────────────────────────────────────────────
//  SCROLLBAR
// ─────────────────────────────────────────────────────────────────
static void UpdateScrollbar(HWND hw) {
    RECT rc; GetClientRect(hw,&rc);
    int viewH=(int)rc.bottom;
    SCROLLINFO si{};
    si.cbSize=sizeof(si);
    si.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
    si.nMin=0; si.nMax=g_ContentH;
    si.nPage=(UINT)viewH; si.nPos=g_ScrollY;
    SetScrollInfo(hw,SB_VERT,&si,TRUE);
}

// ─────────────────────────────────────────────────────────────────
//  FENÊTRE RÉGLAGES (TDP)
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
        auto lbl=[&](const wchar_t* t,int x,int y){
            CreateWindowW(L"STATIC",t,WS_CHILD|WS_VISIBLE,x,y,180,20,hw,0,0,0);
        };
        auto edit=[&](int id,double v,int x,int y){
            wchar_t buf[32]; swprintf_s(buf,L"%.2f",v);
            CreateWindowW(L"EDIT",buf,WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER,
                          x,y,80,22,hw,(HMENU)(INT_PTR)id,0,0);
        };
        lbl(L"TDP CPU max (W) :",10,14); edit(ID_CPU_TDP,g_CpuTdpMax,200,12);
        lbl(L"TDP GPU max (W) :",10,44); edit(ID_GPU_TDP,g_GpuTdpMax,200,42);
        lbl(L"Système base (W):",10,74); edit(ID_SYS_BASE,g_SysBase,200,72);
        lbl(L"Tarif €/kWh :",    10,104);edit(ID_TARIF,g_TarifKwh,200,102);
        CreateWindowW(L"BUTTON",L"Enregistrer",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
                      80,140,140,28,hw,(HMENU)ID_SAVE,0,0);
        break;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==ID_SAVE){
            auto getEdit=[&](int id)->double{
                wchar_t buf[32]={};
                GetDlgItemTextW(hw,id,buf,31);
                return _wtof(buf);
            };
            g_CpuTdpMax=getEdit(ID_CPU_TDP);
            g_GpuTdpMax=getEdit(ID_GPU_TDP);
            g_SysBase  =getEdit(ID_SYS_BASE);
            g_TarifKwh =getEdit(ID_TARIF);
            DestroyWindow(hw); g_HwndSettings=nullptr;
        }
        break;
    case WM_DESTROY: g_HwndSettings=nullptr; break;
    }
    return DefWindowProcW(hw,msg,wp,lp);
}

static void OpenSettings(HWND parent){
    if(g_HwndSettings){ SetForegroundWindow(g_HwndSettings); return; }
    static bool reg=false;
    if(!reg){
        WNDCLASSW wc{}; wc.lpfnWndProc=SettingsProc;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
        wc.lpszClassName=L"PowerMonSettings";
        RegisterClassW(&wc); reg=true;
    }
    g_HwndSettings=CreateWindowW(L"PowerMonSettings",L"Réglages PowerMon",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT,CW_USEDEFAULT,310,210,
        parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    ShowWindow(g_HwndSettings,SW_SHOW);
}

// ─────────────────────────────────────────────────────────────────
//  WNDPROC PRINCIPAL
// ─────────────────────────────────────────────────────────────────
#define ID_TIMER   1
#define WM_SENSOR_UPDATE (WM_USER+1)

static LRESULT CALLBACK WndProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:
        g_Hwnd=hw;
        SetTimer(hw,ID_TIMER,1000,nullptr);
        g_D2D.init(hw);
        break;

    case WM_TIMER:
        InvalidateRect(hw,nullptr,FALSE);
        UpdateScrollbar(hw);
        break;

    case WM_PAINT:{
        PAINTSTRUCT ps; BeginPaint(hw,&ps);
        Render(hw);
        EndPaint(hw,&ps);
        break;
    }
    case WM_SIZE:
        g_D2D.resize(hw);
        InvalidateRect(hw,nullptr,FALSE);
        break;

    case WM_VSCROLL:{
        SCROLLINFO si{}; si.cbSize=sizeof(si); si.fMask=SIF_ALL;
        GetScrollInfo(hw,SB_VERT,&si);
        RECT rc; GetClientRect(hw,&rc);
        switch(LOWORD(wp)){
        case SB_TOP:        g_ScrollY=0; break;
        case SB_BOTTOM:     g_ScrollY=g_ContentH-(int)rc.bottom; break;
        case SB_LINEUP:     g_ScrollY-=30; break;
        case SB_LINEDOWN:   g_ScrollY+=30; break;
        case SB_PAGEUP:     g_ScrollY-=(int)rc.bottom; break;
        case SB_PAGEDOWN:   g_ScrollY+=(int)rc.bottom; break;
        case SB_THUMBTRACK: g_ScrollY=si.nTrackPos; break;
        }
        g_ScrollY=std::clamp(g_ScrollY,0,std::max(0,g_ContentH-(int)rc.bottom));
        UpdateScrollbar(hw);
        InvalidateRect(hw,nullptr,FALSE);
        break;
    }
    case WM_MOUSEWHEEL:{
        int delta=GET_WHEEL_DELTA_WPARAM(wp);
        RECT rc; GetClientRect(hw,&rc);
        g_ScrollY=std::clamp(g_ScrollY-delta/2,0,
                             std::max(0,g_ContentH-(int)rc.bottom));
        UpdateScrollbar(hw);
        InvalidateRect(hw,nullptr,FALSE);
        break;
    }
    // Clic droit → menu contextuel
    case WM_RBUTTONUP:{
        HMENU m=CreatePopupMenu();
        AppendMenuW(m,MF_STRING,1,L"⚙ Réglages TDP / Tarif…");
        AppendMenuW(m,MF_SEPARATOR,0,nullptr);
        AppendMenuW(m,MF_STRING,2,L"Quitter");
        POINT pt; GetCursorPos(&pt);
        int cmd=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,
                               pt.x,pt.y,0,hw,nullptr);
        DestroyMenu(m);
        if(cmd==1) OpenSettings(hw);
        if(cmd==2) PostQuitMessage(0);
        break;
    }
    case WM_KEYDOWN:
        if(wp==VK_F5) OpenSettings(hw);
        break;
    case WM_DESTROY:
        g_Running=false;
        KillTimer(hw,ID_TIMER);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hw,msg,wp,lp);
}

// ─────────────────────────────────────────────────────────────────
//  WINMAIN
// ─────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hi,HINSTANCE,LPSTR,int nShow){
    // DPI awareness
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Activer commctrl
    INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_WIN95_CLASSES};
    InitCommonControlsEx(&ic);

    // Classe fenêtre
    WNDCLASSEXW wc{};
    wc.cbSize       = sizeof(wc);
    wc.lpfnWndProc  = WndProc;
    wc.hInstance    = hi;
    wc.hbrBackground= (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName= L"PowerMonV2";
    wc.hCursor      = LoadCursor(nullptr,IDC_ARROW);
    wc.style        = CS_HREDRAW|CS_VREDRAW;
    RegisterClassExW(&wc);

    HWND hw = CreateWindowExW(0,L"PowerMonV2",
        L"⚡ PowerMon — Moniteur de consommation",
        WS_OVERLAPPEDWINDOW|WS_VSCROLL,
        CW_USEDEFAULT,CW_USEDEFAULT,820,920,
        nullptr,nullptr,hi,nullptr);

    ShowWindow(hw,nShow);
    UpdateWindow(hw);

    // Thread capteurs
    std::thread st(sensorThread);
    st.detach();

    MSG m{};
    while(GetMessage(&m,nullptr,0,0)){
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    g_Running=false;
    Sleep(200);
    return 0;
}
