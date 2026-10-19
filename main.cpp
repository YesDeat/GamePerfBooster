// GamePerfBooster.cpp —— 游戏性能优化器
// 功能：提升游戏进程优先级 / 绑定到性能核 / 降级后台进程
// 说明：不会修改任何反作弊进程，只做系统级优先级与亲和性调整

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <tchar.h>
#include <stdlib.h>
#include <stddef.h>
#include <mutex>
#include <string>
#include <vector>
#include <map>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

#include "resource.h"

#define APP_TITLE        _T("GamePerfBooster")
#define WM_TRAYICON      (WM_USER + 1)
#define WM_APP_UPDATETIP (WM_APP + 1)

#define ID_TRAY_PAUSE_RESUME 1001
#define ID_TRAY_RELOAD       1002
#define ID_TRAY_OPENCONFIG   1003
#define ID_TRAY_EXIT         1004

#define CONFIG_FILE _T("config.ini")

// ------------------------------ 配置结构 ------------------------------

enum PriorityAction { PRIO_HIGH, PRIO_ABOVE, PRIO_NORMAL, PRIO_BELOW, PRIO_IDLE };

struct Rule {
    std::wstring  name;   // 进程名（小写）
    PriorityAction action;
};

struct Config {
    std::vector<Rule> boost;                 // 需要提升的进程（游戏主进程）
    std::vector<Rule> limit;                 // 需要降级的进程（后台）
    int  intervalMs            = 3000;
    bool onlyWhenGameRunning   = true;
    bool bindBoostToPerfCores  = true;
    bool perfClassIsMax        = true;   // EfficiencyClass 取最大值作为性能核
};

struct CpuTopology {
    DWORD_PTR allMask   = 0;
    DWORD_PTR perfMask  = 0;
    int totalThreads    = 0;
    int perfThreads     = 0;
    bool hybrid         = false;
};

// ------------------------------ 全局状态 ------------------------------

static bool          g_running = true;
static bool          g_paused  = false;
static std::mutex    g_mutex;
static HWND          g_hWnd    = NULL;
static NOTIFYICONDATA g_nid    = { 0 };
static Config        g_config;
static CpuTopology   g_cpu;
static int           g_boosted = 0;
static int           g_limited = 0;
static bool          g_gameRunning = false;
static DWORD         g_selfPid = 0;

// ------------------------------ 工具函数 ------------------------------

static std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= L'A' && out[i] <= L'Z') out[i] = out[i] - L'A' + L'a';
    }
    return out;
}

static std::wstring Trim(const std::wstring& s) {
    const wchar_t* ws = L" \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

static int BitCount(DWORD_PTR mask) {
    int n = 0;
    while (mask) { n += (int)(mask & 1); mask >>= 1; }
    return n;
}

static DWORD PriorityClassOf(PriorityAction a) {
    switch (a) {
    case PRIO_HIGH:   return HIGH_PRIORITY_CLASS;
    case PRIO_ABOVE:  return ABOVE_NORMAL_PRIORITY_CLASS;
    case PRIO_NORMAL: return NORMAL_PRIORITY_CLASS;
    case PRIO_BELOW:  return BELOW_NORMAL_PRIORITY_CLASS;
    default:          return IDLE_PRIORITY_CLASS;
    }
}

static bool ParseAction(const std::wstring& v, PriorityAction& out) {
    std::wstring s = ToLower(v);
    if (s == L"high" || s == L"h")        { out = PRIO_HIGH;   return true; }
    if (s == L"above" || s == L"a")       { out = PRIO_ABOVE;  return true; }
    if (s == L"normal" || s == L"n")      { out = PRIO_NORMAL; return true; }
    if (s == L"below" || s == L"b")       { out = PRIO_BELOW;  return true; }
    if (s == L"idle" || s == L"i")        { out = PRIO_IDLE;   return true; }
    return false;
}

// 永不动的系统关键进程（配置里写了也无效）
static bool IsProtectedProcess(const std::wstring& name) {
    static const wchar_t* blocked[] = {
        L"system", L"idle", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"winlogon.exe", L"services.exe", L"lsass.exe", L"lsaiso.exe",
        L"svchost.exe", L"dwm.exe", L"explorer.exe", L"fontdrvhost.exe",
        L"audiodg.exe", L"ctfmon.exe"
    };
    for (int i = 0; i < (int)(sizeof(blocked) / sizeof(blocked[0])); ++i) {
        if (name == blocked[i]) return true;
    }
    return false;
}

// ------------------------------ CPU 拓扑（大小核识别） ------------------------------

static CpuTopology DetectTopology(bool perfClassIsMax) {
    CpuTopology t;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (len == 0) len = 4096;

    std::vector<BYTE> buf(len);
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info =
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data());

    struct Entry { KAFFINITY mask; BYTE cls; };
    std::vector<Entry> entries;

    if (GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len)) {
        DWORD offset = 0;
        // 注意：不能用 sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) 做边界，
        // 该结构体按联合体最大成员计算（80 字节），而核心条目实际只有 48 字节，
        // 会导致最后一个核被漏掉，必须按每条目的 Size 字段推进。
        const DWORD kHeaderSize = (DWORD)offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
        while (offset + kHeaderSize + sizeof(DWORD) <= len) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p =
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + offset);
            if (p->Size < kHeaderSize + sizeof(DWORD)) break;
            if (p->Size > len - offset) break;
            if (p->Relationship == RelationProcessorCore) {
                // 确保 GroupMask 数组完整可读
                DWORD need = kHeaderSize + (DWORD)sizeof(PROCESSOR_RELATIONSHIP);
                if (p->Processor.GroupCount > 1)
                    need += (DWORD)(p->Processor.GroupCount - 1) * (DWORD)sizeof(GROUP_AFFINITY);
                if (p->Size < need) { offset += p->Size; continue; }

                for (WORD g = 0; g < p->Processor.GroupCount; ++g) {
                    if (p->Processor.GroupMask[g].Group == 0) {
                        Entry e;
                        e.mask = p->Processor.GroupMask[g].Mask;
                        e.cls  = p->Processor.EfficiencyClass;
                        entries.push_back(e);
                    }
                }
            }
            offset += p->Size;
        }
    }

    if (!entries.empty()) {
        BYTE maxCls = 0, minCls = 255;
        for (size_t i = 0; i < entries.size(); ++i) {
            t.allMask |= entries[i].mask;
            if (entries[i].cls > maxCls) maxCls = entries[i].cls;
            if (entries[i].cls < minCls) minCls = entries[i].cls;
        }
        // EfficiencyClass 越高越强调性能：默认取最高一级作为性能核
        DWORD_PTR highMask = 0, lowMask = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].cls == maxCls) highMask |= entries[i].mask;
            if (entries[i].cls == minCls) lowMask  |= entries[i].mask;
        }
        t.perfMask = perfClassIsMax ? highMask : lowMask;
        t.hybrid = (maxCls > minCls) && (t.perfMask != 0) && (t.perfMask != t.allMask);
    }

    if (t.allMask == 0) {  // 兜底：用系统可用亲和性掩码
        DWORD_PTR procMask = 0, sysMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)) {
            t.allMask = sysMask;
            t.perfMask = sysMask;
        }
    }

    t.totalThreads = BitCount(t.allMask);
    if (!t.hybrid) t.perfMask = t.allMask;   // 非混合架构：不做亲和性限制
    if (t.perfMask == 0) t.perfMask = t.allMask;
    t.perfThreads = BitCount(t.perfMask);
    return t;
}

// ------------------------------ 配置文件 ------------------------------

static std::wstring GetModuleDir() {
    TCHAR path[MAX_PATH] = { 0 };
    GetModuleFileName(NULL, path, MAX_PATH);
    std::wstring s(path);
    size_t pos = s.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"" : s.substr(0, pos + 1);
}

static std::wstring GetConfigPath() {
    return GetModuleDir() + CONFIG_FILE;
}

static bool ReadFileAll(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(h); return false; }
    out.resize(size);
    DWORD read = 0;
    BOOL ok = ReadFile(h, &out[0], size, &read, NULL);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(read);
    return true;
}

static std::wstring DecodeText(const std::string& raw) {
    size_t start = 0;
    if (raw.size() >= 3 &&
        (BYTE)raw[0] == 0xEF && (BYTE)raw[1] == 0xBB && (BYTE)raw[2] == 0xBF) {
        start = 3;
    }
    const char* p = raw.data() + start;
    int len = (int)(raw.size() - start);

    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, len, NULL, 0);
    if (need > 0) {
        std::wstring out((size_t)need, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, p, len, &out[0], need);
        return out;
    }
    need = MultiByteToWideChar(CP_ACP, 0, p, len, NULL, 0);
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(CP_ACP, 0, p, len, &out[0], need);
    return out;
}

static bool ParseBool(const std::wstring& v) {
    return (v == L"1" || v == L"true" || v == L"yes" || v == L"on");
}

static bool LoadConfig(const std::wstring& path, Config& cfg) {
    std::string raw;
    if (!ReadFileAll(path, raw)) return false;

    std::wstring text = DecodeText(raw);
    std::wstring section;
    size_t i = 0;

    while (i < text.size()) {
        size_t j = text.find(L'\n', i);
        std::wstring line = (j == std::wstring::npos) ? text.substr(i) : text.substr(i, j - i);
        i = (j == std::wstring::npos) ? text.size() : j + 1;

        line = Trim(line);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;

        if (line[0] == L'[') {
            size_t e = line.find(L']');
            if (e != std::wstring::npos) section = ToLower(Trim(line.substr(1, e - 1)));
            continue;
        }

        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Trim(line.substr(0, eq));
        std::wstring val = ToLower(Trim(line.substr(eq + 1)));

        if (section == L"boost" || section == L"limit") {
            PriorityAction a;
            if (!ParseAction(val, a)) continue;
            Rule r;
            r.name   = ToLower(key);
            r.action = a;
            if (IsProtectedProcess(r.name)) continue;
            if (section == L"boost") cfg.boost.push_back(r);
            else                     cfg.limit.push_back(r);
        }
        else if (section == L"options") {
            std::wstring k = ToLower(key);
            if (k == L"intervalms") {
                int v = _wtoi(val.c_str());
                if (v >= 500 && v <= 60000) cfg.intervalMs = v;
            }
            else if (k == L"onlywhengamerunning")  cfg.onlyWhenGameRunning  = ParseBool(val);
            else if (k == L"bindboosttoperfcores") cfg.bindBoostToPerfCores = ParseBool(val);
            else if (k == L"perfcoreclassismax")   cfg.perfClassIsMax       = ParseBool(val);
        }
    }
    return true;
}

static Config DefaultConfig() {
    Config c;
    c.boost.clear();
    c.limit.clear();

    Rule g; g.name = L"valorant-win64-shipping.exe"; g.action = PRIO_ABOVE;
    c.boost.push_back(g);

    const wchar_t* bg[] = { L"riotclientservices.exe", L"chrome.exe", L"msedge.exe", L"firefox.exe" };
    for (int i = 0; i < 4; ++i) {
        Rule r;
        r.name = bg[i];
        // 默认用 below（温和）：降到 idle 效果更强，但切出去看网页会明显卡顿
        r.action = PRIO_BELOW;
        c.limit.push_back(r);
    }

    c.intervalMs           = 3000;
    c.onlyWhenGameRunning  = true;
    c.bindBoostToPerfCores = true;
    c.perfClassIsMax       = true;
    return c;
}

static const wchar_t* kDefaultConfigText =
L"# GamePerfBooster 配置文件\r\n"
L"# 格式：进程名 = 优先级动作（high / above / normal / below / idle）\r\n"
L"# 修改后托盘右键 -> 重新加载配置 即可生效\r\n"
L"\r\n"
L"[Boost]\r\n"
L"# 游戏主进程：提升优先级，并绑定到性能核（混合架构 CPU）\r\n"
L"VALORANT-Win64-Shipping.exe = above\r\n"
L"\r\n"
L"[Limit]\r\n"
L"# 后台进程：仅在游戏运行时降级\r\n"
L"# below = 低于正常（温和，推荐）；idle = 最低（省资源更多，但切出去会很卡）\r\n"
L"RiotClientServices.exe = below\r\n"
L"chrome.exe = below\r\n"
L"msedge.exe = below\r\n"
L"firefox.exe = below\r\n"
L"\r\n"
L"[Options]\r\n"
L"# 检测间隔（毫秒，500-60000）\r\n"
L"IntervalMs = 3000\r\n"
L"# 仅在游戏运行时才降级后台进程（1=是 / 0=否）\r\n"
L"OnlyWhenGameRunning = 1\r\n"
L"# 把 [Boost] 里的进程绑定到性能核（1=是 / 0=否）\r\n"
L"BindBoostToPerfCores = 1\r\n"
L"# 性能核判定：1=EfficiencyClass 最大的一级是性能核（默认）\r\n"
L"# 若绑核后帧数反而下降，改成 0（取最小的一级），或直接关掉上一项\r\n"
L"PerfCoreClassIsMax = 1\r\n";

static void WriteDefaultConfig(const std::wstring& path) {
    int need = WideCharToMultiByte(CP_UTF8, 0, kDefaultConfigText, -1, NULL, 0, NULL, NULL);
    if (need <= 0) return;
    std::string data((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, kDefaultConfigText, -1, &data[0], need, NULL, NULL);
    data.resize((size_t)need - 1);  // 去掉结尾 \0

    std::string out;
    out.push_back((char)0xEF); out.push_back((char)0xBB); out.push_back((char)0xBF);
    out += data;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &written, NULL);
    CloseHandle(h);
}

static void ReloadConfig() {
    std::wstring path = GetConfigPath();
    Config c;
    bool ok = LoadConfig(path, c);
    if (!ok) {
        WriteDefaultConfig(path);
        c = DefaultConfig();
    }
    if (c.boost.empty() && c.limit.empty()) c = DefaultConfig();

    CpuTopology cpu = DetectTopology(c.perfClassIsMax);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_config = c;
        g_cpu    = cpu;
    }
}

// ------------------------------ 应用优化 ------------------------------

static bool ApplyToProcess(DWORD pid, PriorityAction action, DWORD_PTR affinity) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (h == NULL) h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (h == NULL) return false;   // 受保护进程 / 权限不足：静默跳过

    bool ok = false;
    if (SetPriorityClass(h, PriorityClassOf(action))) ok = true;
    if (affinity != 0) SetProcessAffinityMask(h, affinity);
    CloseHandle(h);
    return ok;
}

static void Tick() {
    Config cfg;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        cfg = g_config;
    }

    std::map<std::wstring, std::vector<DWORD>> procs;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe = { 0 };
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == g_selfPid) continue;
            std::wstring name = ToLower(pe.szExeFile);
            if (IsProtectedProcess(name)) continue;
            procs[name].push_back(pe.th32ProcessID);
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);

    bool gameRunning = false;
    int boosted = 0, limited = 0;

    CpuTopology cpu;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        cpu = g_cpu;
    }
    DWORD_PTR boostAff = (cfg.bindBoostToPerfCores && cpu.hybrid) ? cpu.perfMask : 0;

    for (size_t i = 0; i < cfg.boost.size(); ++i) {
        std::map<std::wstring, std::vector<DWORD>>::iterator it = procs.find(cfg.boost[i].name);
        if (it == procs.end()) continue;
        gameRunning = true;
        for (size_t k = 0; k < it->second.size(); ++k) {
            if (ApplyToProcess(it->second[k], cfg.boost[i].action, boostAff)) boosted++;
        }
    }

    if (!cfg.onlyWhenGameRunning || gameRunning) {
        for (size_t i = 0; i < cfg.limit.size(); ++i) {
            std::map<std::wstring, std::vector<DWORD>>::iterator it = procs.find(cfg.limit[i].name);
            if (it == procs.end()) continue;
            for (size_t k = 0; k < it->second.size(); ++k) {
                if (ApplyToProcess(it->second[k], cfg.limit[i].action, 0)) limited++;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_gameRunning = gameRunning;
        g_boosted = boosted;
        g_limited = limited;
    }
    if (g_hWnd) PostMessage(g_hWnd, WM_APP_UPDATETIP, 0, 0);
}

static DWORD WINAPI MonitorThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    while (g_running) {
        bool paused;
        int interval;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            paused = g_paused;
            interval = g_config.intervalMs;
        }
        if (paused) {
            Sleep(500);
            continue;
        }
        Tick();
        Sleep(interval);
    }
    return 0;
}

// ------------------------------ 界面 ------------------------------

static void UpdateTrayTip() {
    bool paused, gameRunning;
    int boosted, limited;
    CpuTopology cpu;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        paused = g_paused;
        gameRunning = g_gameRunning;
        boosted = g_boosted;
        limited = g_limited;
        cpu = g_cpu;
    }

    TCHAR tip[128] = { 0 };
    if (cpu.hybrid) {
        _sntprintf_s(tip, _countof(tip), _TRUNCATE,
            _T("%s（%s）\n性能核 %d/%d | 优化 %d | 降级 %d"),
            APP_TITLE, paused ? _T("已暂停") : (gameRunning ? _T("游戏运行中") : _T("待机")),
            cpu.perfThreads, cpu.totalThreads, boosted, limited);
    }
    else {
        _sntprintf_s(tip, _countof(tip), _TRUNCATE,
            _T("%s（%s）\n线程 %d | 优化 %d | 降级 %d"),
            APP_TITLE, paused ? _T("已暂停") : (gameRunning ? _T("游戏运行中") : _T("待机")),
            cpu.totalThreads, boosted, limited);
    }

    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    _tcscpy_s(g_nid.szTip, tip);
    g_nid.szTip[_countof(g_nid.szTip) - 1] = 0;
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
}

static void ShowTrayMenu() {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();

    bool paused, gameRunning;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        paused = g_paused;
        gameRunning = g_gameRunning;
    }

    TCHAR title[64] = { 0 };
    _sntprintf_s(title, _countof(title), _TRUNCATE,
        _T("GamePerfBooster | %s"),
        paused ? _T("已暂停") : (gameRunning ? _T("游戏运行中") : _T("待机中")));
    AppendMenu(hMenu, MF_STRING | MF_GRAYED, 0, title);
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_TRAY_PAUSE_RESUME, paused ? _T("恢复优化") : _T("暂停优化"));
    AppendMenu(hMenu, MF_STRING, ID_TRAY_RELOAD, _T("重新加载配置"));
    AppendMenu(hMenu, MF_STRING, ID_TRAY_OPENCONFIG, _T("打开配置文件"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, _T("退出"));

    SetForegroundWindow(g_hWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, g_hWnd, NULL);
    DestroyMenu(hMenu);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_nid.cbSize = sizeof(NOTIFYICONDATA);
        g_nid.hWnd = hWnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1));
        if (g_nid.hIcon == NULL) g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        _tcscpy_s(g_nid.szTip, APP_TITLE);
        Shell_NotifyIcon(NIM_ADD, &g_nid);

        CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
        break;
    }

    case WM_TRAYICON: {
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) ShowTrayMenu();
        break;
    }

    case WM_APP_UPDATETIP:
        UpdateTrayTip();
        break;

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_TRAY_PAUSE_RESUME: {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_paused = !g_paused;
            break;
        }
        case ID_TRAY_RELOAD:
            ReloadConfig();
            MessageBox(hWnd, _T("配置已重新加载。"), APP_TITLE, MB_ICONINFORMATION);
            break;
        case ID_TRAY_OPENCONFIG: {
            std::wstring path = GetConfigPath();
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                WriteDefaultConfig(path);
            ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOW);
            break;
        }
        case ID_TRAY_EXIT: {
            g_running = false;
            Shell_NotifyIcon(NIM_DELETE, &g_nid);
            DestroyWindow(hWnd);
            PostQuitMessage(0);
            break;
        }
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ------------------------------ 权限 ------------------------------

static bool CheckAdminPrivilege() {
    BOOL isAdmin = FALSE;
    PSID pAdminSid = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (!AllocateAndInitializeSid(&ntAuthority, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &pAdminSid)) return false;

    if (!CheckTokenMembership(NULL, pAdminSid, &isAdmin)) {
        FreeSid(pAdminSid);
        return false;
    }
    FreeSid(pAdminSid);
    return isAdmin == TRUE;
}

static void ElevateToAdmin() {
    TCHAR szPath[MAX_PATH] = { 0 };
    GetModuleFileName(NULL, szPath, MAX_PATH);

    SHELLEXECUTEINFO sei = { 0 };
    sei.cbSize = sizeof(sei);
    sei.lpVerb = _T("runas");
    sei.lpFile = szPath;
    sei.nShow  = SW_SHOWNORMAL;

    if (!ShellExecuteEx(&sei)) {
        MessageBox(NULL, _T("请以管理员身份运行程序！"), APP_TITLE, MB_ICONWARNING);
    }
}

// ------------------------------ 入口 ------------------------------

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    g_selfPid = GetCurrentProcessId();

    if (!CheckAdminPrivilege()) {
        ElevateToAdmin();
        return 0;
    }

    ReloadConfig();

    WNDCLASSEX wc = { 0 };
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = _T("GamePerfBoosterClass");
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);

    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, _T("窗口类注册失败！"), APP_TITLE, MB_ICONERROR);
        return 1;
    }

    g_hWnd = CreateWindowEx(0, _T("GamePerfBoosterClass"), APP_TITLE,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, NULL);

    if (g_hWnd == NULL) {
        MessageBox(NULL, _T("窗口创建失败！"), APP_TITLE, MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
    return (int)msg.wParam;
}
