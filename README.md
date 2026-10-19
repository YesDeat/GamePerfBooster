# GamePerfBooster

一个常驻系统托盘的 **游戏性能优化器**（Win32 / C++）。在不改动任何反作弊进程的前提下，把 CPU 资源尽量让给游戏：

A system-tray-resident **game performance optimizer** (Win32 / C++). It pushes as much CPU headroom as possible to your game without touching any anti-cheat components:

- **提升游戏进程优先级**，并在混合架构（大小核）CPU 上把游戏绑定到**性能核**
  **Boost the game process priority** and, on hybrid (big.LITTLE) CPUs, bind the game to the **performance cores**.
- **降级后台进程**（启动器、浏览器等），且默认只在游戏运行时才生效
  **Throttle background processes** (launcher, browsers, etc.), by default only while a game is running.
- 规则全部写在 `config.ini`，想优化哪个游戏自己填，改完托盘右键"重新加载配置"即可
  All rules live in `config.ini` — add whatever game you want; right-click the tray and "Reload config" to apply.
- 内置系统关键进程保护名单，配置里写了也不会生效
  A built-in protection list of critical system processes that can never be touched, even if listed in config.

> 不会碰任何反作弊组件。它只调用 `SetPriorityClass` / `SetProcessAffinityMask`，
> 不注入、不读写其他进程内存、不拦截任何驱动，因此不存在因"篡改反作弊"带来的封号风险。

> It never touches any anti-cheat component. It only calls `SetPriorityClass` / `SetProcessAffinityMask`,
> performs no injection, reads/writes no other process memory, and hooks no driver — so there is no ban risk
> from "tampering with anti-cheat".

## 编译 / Build

1. Visual Studio 2022（工具集 v143）打开 `GamePerfBooster.sln`
2. 选择 **Release | x64**
3. 生成解决方案

1. Open `GamePerfBooster.sln` with Visual Studio 2022 (toolset v143)
2. Select **Release | x64**
3. Build the solution

程序需要**管理员权限**运行（否则无法调整其他用户的进程）；非管理员启动时会自动申请提权。

The program requires **administrator privileges** (otherwise it cannot adjust other users' processes); it will auto-request elevation when launched without them.

## 使用 / Usage

1. 把生成的 `GamePerfBooster.exe` 放到任意目录运行
   Place the built `GamePerfBooster.exe` in any folder and run it.
2. 首次运行会在 exe 同目录自动生成 `config.ini`
   On first run a `config.ini` is auto-generated next to the exe.
3. 托盘右键菜单 / Right-click tray menu:

| 菜单项 / Menu item | 说明 / Description |
| --- | --- |
| 暂停/恢复优化 / Pause/Resume | 临时停止所有调整 / Temporarily stop all adjustments |
| 重新加载配置 / Reload config | 修改 `config.ini` 后点这个即时生效 / Apply `config.ini` changes instantly |
| 打开配置文件 / Open config | 用默认编辑器打开 `config.ini` / Open `config.ini` in the default editor |
| 退出 / Exit | 退出程序（已生效的优先级会随进程结束自动恢复）/ Quit (applied priorities recover when processes end) |

托盘悬停提示会显示：是否检测到游戏、性能核数量、已优化/已降级的进程数。

The tray tooltip shows: whether a game is detected, the performance-core count, and how many processes were boosted / throttled.

## 配置说明 / Configuration

```ini
[Boost]
# 游戏主进程：提升优先级，并绑定到性能核
VALORANT-Win64-Shipping.exe = above

[Limit]
# 后台进程：仅在游戏运行时降级
RiotClientServices.exe = below
chrome.exe = below
msedge.exe = below
firefox.exe = below

[Options]
IntervalMs = 3000
OnlyWhenGameRunning = 1
BindBoostToPerfCores = 1
PerfCoreClassIsMax = 1
```

```ini
[Boost]
# Game main process: raise priority and bind to performance cores
VALORANT-Win64-Shipping.exe = above

[Limit]
# Background processes: throttled only while a game is running
RiotClientServices.exe = below
chrome.exe = below
msedge.exe = below
firefox.exe = below

[Options]
IntervalMs = 3000
OnlyWhenGameRunning = 1
BindBoostToPerfCores = 1
PerfCoreClassIsMax = 1
```

### 优先级动作 / Priority actions

| 值 / Value | 含义 / Meaning |
| --- | --- |
| `high` | 高（效果最强，可能导致系统其他操作卡顿）/ High (strongest, may stutter the rest of the system) |
| `above` | 高于正常（推荐）/ Above Normal (recommended) |
| `normal` | 正常 / Normal |
| `below` | 低于正常（后台进程推荐）/ Below Normal (recommended for background) |
| `idle` | 最低（省资源更多，但切出去会很卡）/ Idle (frees most resources, but switching out will be very laggy) |

### 选项 / Options

| 选项 / Option | 说明 / Description |
| --- | --- |
| `IntervalMs` | 检测间隔，500–60000，默认 3000 / Scan interval, 500–60000, default 3000 |
| `OnlyWhenGameRunning` | `1`=只有检测到 `[Boost]` 里的进程时才降级后台进程；`0`=一直降级 / `1` = throttle background only when a `[Boost]` process is seen; `0` = always |
| `BindBoostToPerfCores` | `1`=把游戏绑到性能核；`0`=只调优先级，不动亲和性 / `1` = bind game to performance cores; `0` = priority only, no affinity |
| `PerfCoreClassIsMax` | 性能核判定方式，见下方说明 / Performance-core detection method, see below |

### 常见游戏进程名 / Common game process names

| 游戏 / Game | 主进程 / Main process |
| --- | --- |
| VALORANT（含港服）/ VALORANT (incl. HK/TW server) | `VALORANT-Win64-Shipping.exe` |
| 英雄联盟 / League of Legends | `League of Legends.exe` |
| Apex 英雄 / Apex Legends | `r5apex.exe` |
| CS2 | `cs2.exe` |
| 绝地求生 / PUBG | `TslGame.exe` |
| 原神 / Genshin Impact | `YuanShen.exe` / `GenshinImpact.exe` |
| 三角洲行动 / Delta Force | `DeltaForceClient-Win64-Shipping.exe` |

## 关于"绑性能核" / About "binding to performance cores"

- 只有**混合架构** CPU（如 12 代及以后的 Intel 大小核）才会生效；普通 CPU 上程序会自动跳过绑核，只调优先级。
  Only **hybrid** CPUs (e.g. Intel 12th-gen+ big.LITTLE) are affected; on normal CPUs the program automatically skips affinity binding and only adjusts priority.
- 用 `GetLogicalProcessorInformationEx` 读 `EfficiencyClass` 判定性能核。若绑核后帧数反而下降，
  把 `PerfCoreClassIsMax` 改成 `0`（取另一类核心），或直接把 `BindBoostToPerfCores` 设为 `0`。
  Performance cores are detected via `GetLogicalProcessorInformationEx` reading `EfficiencyClass`. If FPS drops after binding,
  set `PerfCoreClassIsMax` to `0` (use the other core class) or set `BindBoostToPerfCores` to `0`.
- 线程数很多的 3A 大作不建议绑核（可能反而限制调度），绑核更适合 VALORANT 这类轻线程游戏。
  Thread-heavy AAA titles are not recommended for binding (it may restrict scheduling); binding suits light-thread games like VALORANT.

## 替换图标为你自己的 Logo / Replace the icon with your own logo

仓库里的 `001.ico` 只是通用占位图标，请替换成你自己的 Logo：

The `001.ico` shipped in the repository is only a generic placeholder; please replace it with your own logo:

1. 把你的图标做成包含 16/32/48/256 尺寸的 `.ico`（可用在线 PNG 转 ICO 工具）
   Make your icon an `.ico` containing 16/32/48/256 sizes (use an online PNG-to-ICO tool).
2. 命名为 `001.ico`，覆盖仓库根目录同名文件
   Name it `001.ico` and overwrite the file in the repository root.
3. 若改用其他文件名，需同步修改 `GamePerfBooster.rc` 中的 `IDI_ICON1 ICON "001.ico"`
   以及 `GamePerfBooster.vcxproj` / `.filters` 中的 `<Image Include="001.ico" />`
   If you use a different file name, also update `IDI_ICON1 ICON "001.ico"` in `GamePerfBooster.rc`
   and `<Image Include="001.ico" />` in `GamePerfBooster.vcxproj` / `.filters`.

图标在代码中通过 `LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1))` 加载为托盘图标（`main.cpp` 的 `WM_CREATE` 分支）。

The icon is loaded as the tray icon via `LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1))` (the `WM_CREATE` branch in `main.cpp`).

## 文件说明 / Files

| 文件 / File | 说明 / Description |
| --- | --- |
| `main.cpp` | 全部逻辑（进程扫描、优先级/亲和性调整、CPU 拓扑识别、托盘界面）/ All logic (process scan, priority/affinity, CPU topology, tray UI) |
| `resource.h`、`GamePerfBooster.rc` | 资源定义与资源脚本（图标）/ Resource definitions and resource script (icon) |
| `GamePerfBooster.sln`、`.vcxproj`、`.filters` | Visual Studio 工程文件 / Visual Studio project files |

## 已知限制 / Known limitations

- 受保护进程（含部分反作弊组件）无法调整，程序会静默跳过，不会报错。
  Protected processes (including some anti-cheat components) cannot be adjusted; the program silently skips them without error.
- 只处理 CPU 处理器组 0，超 64 线程的多组服务器环境不在支持范围。
  Only CPU processor group 0 is handled; multi-group servers with more than 64 threads are out of scope.
