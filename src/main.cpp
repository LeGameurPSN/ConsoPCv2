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
    int    monitorCount = 0;
    double monitorWatt  = 0;

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

// Nombre d'écrans + estimation W
static void enumMonitors(int& cnt, double& watt) {
    cnt = GetSystemMetrics(SM_CMONITORS);
    // Estimation : ~25W par écran standard
    watt = cnt * 25.0;
}

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

        // Puissances estimées
        double cpuW = g_CpuTdpIdle+(g_CpuTdpMax-g_CpuTdpIdle)*(cpuLoad/100.0);
        double gpuW = g_GpuTdpIdle+(g_GpuTdpMax-g_GpuTdpIdle)*(gpuLoad/100.0);

        // USB
        if (++usbRefresh>=15) { enumUsb(usbDevs); usbRefresh=0; }
        double usbW=0; for(auto&d:usbDevs) usbW+=d.second;

        // Écrans
        int monCnt=0; double monW=0;
        enumMonitors(monCnt,monW);

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
            g_Sensor.gpuLoad   = gpuLoad;
            g_Sensor.gpuWatt   = gpuW;
            g_Sensor.gpuName   = gpuName;
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
        swprintf_s(sub,L"%.0f%% charge  |  TDP max %.0fW  |  %.1f Go VRAM",
                   s.gpuLoad,g_GpuTdpMax,vramGB);
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
        Card c={pad*2+cw,curY,cw,115};
        wchar_t v[16],sub[128];
        swprintf_s(v,L"%.0f",s.monitorWatt);
        swprintf_s(sub,L"%d écran(s) détecté(s)  |  ~25W par écran",s.monitorCount);
        c.draw(L"AFFICHAGE",v,L"W",sub,
               std::clamp(s.monitorWatt/150.0*100,0.0,100.0),C::YELLOW);
    }
    curY+=122;

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
    int viewH=rc.bottom;
    SCROLLINFO si{};
    si.cbSize=sizeof(si);
    si.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
    si.nMin=0; si.nMax=g_ContentH;
    si.nPage=viewH; si.nPos=g_ScrollY;
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
        case SB_BOTTOM:     g_ScrollY=g_ContentH-rc.bottom; break;
        case SB_LINEUP:     g_ScrollY-=30; break;
        case SB_LINEDOWN:   g_ScrollY+=30; break;
        case SB_PAGEUP:     g_ScrollY-=rc.bottom; break;
        case SB_PAGEDOWN:   g_ScrollY+=rc.bottom; break;
        case SB_THUMBTRACK: g_ScrollY=si.nTrackPos; break;
        }
        g_ScrollY=std::clamp(g_ScrollY,0,std::max(0,g_ContentH-rc.bottom));
        UpdateScrollbar(hw);
        InvalidateRect(hw,nullptr,FALSE);
        break;
    }
    case WM_MOUSEWHEEL:{
        int delta=GET_WHEEL_DELTA_WPARAM(wp);
        RECT rc; GetClientRect(hw,&rc);
        g_ScrollY=std::clamp(g_ScrollY-delta/2,0,
                             std::max(0,g_ContentH-rc.bottom));
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
