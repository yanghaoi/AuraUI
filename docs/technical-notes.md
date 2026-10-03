# 技术笔记：桌面层 HUD 的 Win32 深坑与最终方案

本文记录 AuraUI 在开发中踩到的 Win32 / DWM / Shell 深坑：现象、定位过程、
实证方法与最终方案。所有结论都在 Windows 10 19045（部分场景经 RDP 会话）上实测验证。

目录：

1. [渲染模式：挂载态 GDI vs 移动态 ULW](#1-渲染模式)
2. [桌面层挂载：WorkerW 拓扑与 0x052C](#2-桌面层挂载workerw-拓扑与-0x052c)
3. [分层子窗口的三种故障模式（实证）](#3-分层子窗口的三种故障模式)
4. [残影（分身）：WorkerW 从不重绘被腾出的区域](#4-残影问题)
5. [移动模式设计](#5-移动模式)
6. [托盘回调：NOTIFYICON_VERSION_4 的真实布局](#6-托盘回调)
7. [ICO 资源：每个条目必须是完整 DIB](#7-ico-资源)
8. [调试方法与工具](#8-调试方法)
9. [文字宽度保护](#9-文字宽度保护自动最小宽--设置护栏)
10. [配置与生命周期的实测教训（2026-10 审计）](#10-配置与生命周期的实测教训2026-10-审计)
11. [已知限制](#11-已知限制)
12. [参考](#12-参考)

---

## 1. 渲染模式

HUD 有两种渲染形态，按窗口所处状态切换：

| | 挂载态（桌面层子窗口） | 移动态（拖动中的顶层弹窗） |
|---|---|---|
| 窗口层级 | 壁纸层 WorkerW 的 `WS_CHILD` | `WS_POPUP` 顶层 + `WS_EX_TOPMOST` |
| `WS_EX_LAYERED` | **无** | 有 |
| `WS_EX_TRANSPARENT` | 有（鼠标穿透） | 无（接收拖动） |
| `WS_EX_NOACTIVATE` | 有 | 无 |
| 内容呈现 | `WM_PAINT` + `BitBlt`：**壁纸垫底** + 半透明面板（不透明表面） | `UpdateLayeredWindow`（逐像素 alpha） |
| 半透明 | 有，经壁纸垫底模拟（见下） | 原生逐像素 alpha |

绘制管线不变：Direct2D（`ID2D1DCRenderTarget`）→ 32bpp 预乘 alpha DIB。
挂载态由 `WM_PAINT` 里 `BitBlt` 上屏（`ulwOk_ = false`）；
移动态由 `UpdateLayeredWindow` 上屏。文字均为 DirectWrite + 灰度抗锯齿。

**挂载态的透明度：壁纸垫底（underlay）。** 普通 GDI 子窗口的表面是不透明的，
无法直接做逐像素透明。方案：把 HUD 后面的那块**真实壁纸**按 shell 的摆放样式
（FILL/STRETCH/FIT/CENTER）裁剪缩放成**面板尺寸的不透明裁片**（WIC
Clipper+Scaler，信箱区域用用户背景色填充）垫在面板下，再以 `bg @ opacity`
画半透明面板——合成结果与"透明面板叠在壁纸上"一致，且表面不透明、`BitBlt` 安全。

关键工程细节：

* **裁片在后台线程生成**（latest-wins 队列 + 完成后 `PostMessage` 回 UI 线程套用），
  大图解码（30-100ms）不卡渲染；且**永不保留全分辨率像素**——面板尺寸裁片
  ~400KB，而非全图的 ~16MB（像素+D2D 位图）。
* 壁纸状态优先走 `IDesktopWallpaper` COM（每显示器路径 + 规范摆放枚举——
  注册表 `WallpaperStyle` 在部分构建上是未文档化值如 10，猜测会错），注册表降级。
  COM 侧的显示器索引用 `GetMonitorRECT` 按矩形几何匹配 `EnumDisplayMonitors`
  序（两个枚举的顺序没有文档保证，见第 10 节）。
* **解码源必须是 shell 实际显示的那层缓存（三层链）**：注册表/COM 解析出的原图
  **不是**桌面显示的内容。实测（PrintWindow 捕获壁纸层合成画面 vs 候选文件，
  mean|diff|/255）：
  1. `Themes\CachedFiles\CachedImage_<W>_<H>_POS*.jpg` —— 已按显示器预裁剪、
     **1:1 直接铺放，误差 0.65** ← DWM 实际显示的就是它；
  2. `Themes\TranscodedWallpaper` —— 第一次重编码（489KB→266KB，若按居中裁切
     数学用误差 ~15）；
  3. 注册表原图（同样 ~15，且多一层编码差）。
  垫底若解码 2/3 层，~80% 不透明的面板主体能掩盖差异，但圆角切除区让裁片直接
  紧贴真实桌面——差异显形为"半透明的角"，圆角越大越明显（该缺陷自项目诞生即
  存在，2026-09-22 定位并修复；第一轮只修到第 2 层，仍差 ~15，第二轮修到第 1
  层后角部误差 0.81/255、BR 角 0.04）。`CurrentWallpaper` 三级解析：按显示器
  WxH 匹配 CachedImage（命中时 style 置 stretch——显示器尺寸图片的 stretch 即
  精确 1:1，解码数学零改动）→ TranscodedWallpaper → 原图；裁片键携带源文件
  `LastWriteTime`（这些缓存会被原地重写：换壁纸、幻灯片轮换、SPI 兜底重放都会
  删除重建 CachedFiles——时间戳机制吸收这种抖动）。
  取证方法论（PrintWindow/BGRX/比值判据等）见第 8 节。
* 裁片键 = 路径+样式+背景色+显示器矩形+面板矩形+源文件时间戳，任一变化才重新调度。
* 刷新触发：挂载 / 位置尺寸变化 / `WM_SETTINGCHANGE(SPI_SETDESKWALLPAPER)`
  （换壁纸立即跟随，不等 60 次渲染的节流周期）/ 会话重连解锁 / 每 60 渲染节流。
  **跟随模式**：SPI 兜底重放后 120 渲染内改为**每渲染重查**（Shell 重建缓存
  6-20s，桌面会在层间流转，慢轮询会留下数秒的层错位——即"半透明边角"）；
  无裁片状态与**上次解码失败**同样每渲染重试——失败路径置失败标志旁路同键
  去重，否则"重试"会被 latest-wins 去重吞掉，一次瞬时失败留成永久黑角
  （2026-10-02 修复，见第 10 节）。
* 无壁纸文件（纯色桌面 / 聚焦无缓存文件）与 TILE / SPAN 样式回退为**纯色垫底**
  （shell 背景色 `WallpaperInfo::bg`）：面板透明度仍然生效，只是垫底不是真实壁纸
  （SPAN 的跨屏几何无法用单屏裁片表达；TILE 留待未来实现真实平铺解码）。

---

## 2. 桌面层挂载：WorkerW 拓扑与 0x052C

`desktop/desktop.cpp` 负责让 HUD 真正"沉"到桌面图标下面。

### 正确的目标：壁纸层 WorkerW（**不是** Progman）

壁纸层建好之后的拓扑：

```
WorkerW  (图标宿主)   <- 内含 SHELLDLL_DefView -> SysListView32
WorkerW  (壁纸层)     <- HUD 挂这里
Progman
```

壁纸层 = **全屏、可见、属于 Explorer、且不含 `SHELLDLL_DefView`** 的 `WorkerW`。
它的位置随 shell 版本变化，所以两处都要找：

| shell 版本 | 壁纸层位置 |
|---|---|
| Windows 10 ~ 11 23H2 | 顶层 WorkerW（在图标宿主之后） |
| Windows 11 24H2+ / Win10 发过 `0x052C` 之后 | **Progman 的直接子窗口** |

（Win11 24H2+ 的 Progman 带 `WS_EX_NOREDIRECTIONBITMAP` 的 "raised desktop"，
壁纸处理另有差异——`IsRaisedDesktop()` 判别，SPI 重放在其上完全跳过。）

### ⚠️ `0x052C` 的参数：wParam 必须是 `0xD`

**这是本项目最大的坑**：参数写错时窗口一切正常但**完全不可见**，且没有任何报错。

壁纸层是**懒加载**的，不存在时必须发 `0x052C` 让 shell 建出来：

```cpp
// ❌ 静默无效：shell 什么都不做，壁纸层保持隐藏
SendMessageTimeout(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);
// ✅ 正确
SendMessageTimeout(progman, 0x052C, 0x0D, 0x1, SMTO_NORMAL, 1000, nullptr);
```

实测（Windows 10 19045）发送前后的全屏 WorkerW：

```
发送前:  [0x14028c  visible=False]                        <- 壁纸层隐藏
发送后:  [0x14028c visible=True, 0x2d02fe visible=True]    <- 出现可见的壁纸层
```

发送 `(0,0)` 时**没有任何变化**。此时若把窗口挂到那个隐藏的 WorkerW 上，
窗口自身的 `IsWindowVisible` 仍是 `True`，但父窗口不可见 -> 屏幕上什么都没有。

### ⚠️ 为什么不能挂 Progman

`SHELLDLL_DefView` 会**抓取父窗口的图像缓冲来画自己的背景**，所以：
放在它下面的兄弟窗口被那层背景盖住，放在它上面又会盖住图标 —— **没有可用的位置**。

这也解释了一个典型症状：**按 Win+D / 点"显示桌面"时窗口闪一下就没了** ——
那是 shell 重绘桌面时背景快照被刷新，窗口短暂露出又被盖回。

### ⚠️ 只按类名找 `WorkerW` 不可靠

**窗口类按进程注册，类名可以重名**，而 `FindWindowExW(..., L"WorkerW", ...)`
只比对类名字符串。实测本机 18 个 `WorkerW` 里有 3 个不属于 Explorer：

```
0x5108e0  WorkBuddyAI.exe     visible=False  Explorer的吗=NO
0x2505ec  Everything.exe      visible=False  Explorer的吗=NO
0x160170  RuntimeBroker.exe   visible=False  Explorer的吗=NO
```

挂到这种窗口上，**父窗口不可见 -> 子窗口也不可见**，窗口直接隐身。所以宿主必须同时满足：

| 规则 | 原因 |
|---|---|
| **可见**（`IsWindowVisible`） | 隐藏宿主会让子窗口一起隐身 |
| **属于 Explorer 进程** | 排除第三方同名类窗口（与 Progman 的 pid 对比） |
| **不含 `SHELLDLL_DefView`** | 那是图标宿主，不是壁纸层 |

### 挂载步骤

1. 按上表找壁纸层；找不到就发 `0x052C(0xD, 0x1)` 后重试（最多 5 次，"找不到 -> 发 -> 再找"）；
2. **先把窗口样式从 `WS_POPUP` 改成 `WS_CHILD`，再 `SetParent`**
   （MSDN 明确要求；否则窗口会处于"半挂载"状态，`GetParent()` 返回 `NULL`）；
3. `SetWindowPos(..., HWND_BOTTOM, ...)` 让它位于壁纸层内部最底；
   若宿主自己带 `SHELLDLL_DefView`，则插到它的正下方 —— `desktop::ZOrderAnchor()`
   把这条规则收敛成一个函数，`EnsureZOrder()`（挂载/自愈）与 `Hud::PlaceWindow()`
   （每次重绘）共用同一个锚点，不会互相打架；
4. 坐标通过 `MapWindowPoints(HWND_DESKTOP, host, ...)` 从屏幕坐标换算成父窗口客户区坐标，
   因此虚拟桌面负坐标、多显示器都能正确处理。

**绝不使用 `HWND_TOPMOST`。**

### Explorer 重启恢复

Explorer 被结束/崩溃/重启时，桌面窗口会被销毁，HUD 作为子窗口也会一起消失。恢复链路：

| 触发 | 处理 |
|---|---|
| `TaskbarCreated` 广播（隐藏的顶层消息窗口接收） | 重建托盘图标 + 重新挂载 HUD |
| 2 秒健康检查定时器 | HUD 窗口不存在 → **重建窗口**；父窗口失效 → 重新 `SetParent`；z 序被抬到图标之上 → 用 `EnsureZOrder()` 推回 `SHELLDLL_DefView` 正下方；显示器布局变化 → 重新排版 |
| `WM_DISPLAYCHANGE` / `WM_DEVICECHANGE` | 重新排版 + 刷新网卡枚举 |

不需要用户手动做任何事。

---

## 3. 分层子窗口的三种故障模式

背景：挂载 = `SetParent` 到 Explorer 的壁纸层 WorkerW（跨进程 graft）。
实验与现场日志证明，**跨进程 reparent 之后，分层子窗口在本机不可靠**，
共观察到三种互相独立的表现：

| # | 故障 | 现象 | 证据 |
|---|---|---|---|
| A | ULW 合成 wedged | `UpdateLayeredWindow` 每秒返回成功，`WS_VISIBLE` 在、父窗口正确，**但屏幕上什么都没有** | 运行日志无任何 ULW 失败记录；窗口结构探针全部正常；用户目视不可见 |
| B | SLWA 状态不合成 | `GetLayeredWindowAttributes` 读回 `ok=TRUE, alpha=已设置`，同样不上屏 | 同上，切换 SLWA/ULW 模式均无效 |
| C | LAYERED 位无法恢复 | 子窗口的 `WS_EX_LAYERED` 被清除后，`SetWindowLongPtr` 加回**静默失败**（读回为空，重试 3 次全丢） | 日志 `WS_EX_LAYERED re-set lost, retrying` ×3；样式位跟踪探针 |

三种模式共同点：**所有窗口属性读数全部正常，唯独 DWM 不合成**。这使得"属性探针"
（样式位、父窗口、z 序、可见位）全部失效，只能靠像素级探针或人眼判定。

**结论**：挂载态（跨进程子窗口）只能使用普通 GDI 重定向路径
（非分层 + `WM_PAINT`/`BitBlt`），它是唯一被长期实证稳定可见的形态。
顶层弹窗（移动态）不受此限制，保留 ULW 逐像素渲染。
另注意：**清除子窗口的 LAYERED 位发生在可见状态下时，会把当前帧"烤"进桌面
合成**，留下永久残影（见下一节）——任何样式位操作都必须在窗口隐藏时进行。

---

## 4. 残影问题

### 机制

DWM 下**每个顶层窗口只有一张 GDI 重定向表面**，子窗口树的所有 GDI 输出
（包括 HUD 的 BitBlt）都写入**WorkerW 顶层窗口自己的表面**。
当子窗口被移动 / 隐藏 / 销毁后，暴露的矩形需要 WorkerW 自己重绘——
但它是 Explorer 的内部窗口，对"寄宿的访客子窗口"从不重绘
（跨进程 graft 树的绘制同步是坏的，参见 Raymond Chen 的 SetParent 系列）。
于是子窗口的最后一帧**永久僵死**在桌面上 = "分身"。

实证：`EnumWindows` 始终只有 1 个活窗口（排除窗口泄漏）；重启 Explorer
重建整个场景后分身消失（表面被重建）。这也解释了"隐藏 HUD 后面板不消失、
数据不更新；再显示后恢复更新"——窗口藏了，但它烤在 WorkerW 表面里的最后一帧还在。

### 修复：`desktop::RefreshWallpaper(const RECT* vacated, bool replay)`

三级策略（`desktop/desktop.cpp`），按代价递增、命中即止：

1. **绘制覆盖（主路径）**：按当前壁纸状态（三级缓存解析 + 摆放数学，
   `DecodeRegionToBGRA`，与 `Hud::DecodeUnderlayCrop` 保持镜像、需同步维护）
   解码腾出区域应有的像素，经跨进程 `GetDC(host)` 直接 `BitBlt` 到壁纸
   WorkerW 的表面盖掉冻结帧。**不经 Shell、零缓存流转、像素级正确**
   （实测：覆盖后旧区残影 mean|d|=0.01/255，缓存文件 mtime 不变）。
2. **精准失效（绘制失败时伴随兜底）**：`RedrawWindow(host, &rect, nullptr,
   RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_NOINTERNALPAINT)`。
   **单独使用不足以清残影**（实测 mean|d|=35.8 vs 干净区 0.12）——桌面类
   窗口只经 `WM_ERASEBKGND` 重绘；**禁止** `RDW_UPDATENOW` / `RDW_ERASENOW`
   （同步标志对其它进程拥有的窗口无效）。
3. **SPI 重放（仅兜底）**：绘制失败（无可解码壁纸）或桌面级刷新
   （`vacated == nullptr`，会话重连——RDP 客户端缓存里的旧帧需要它）时，
   `SystemParametersInfoW(SPI_SETDESKWALLPAPER, 真实路径, SPIF_SENDWININICHANGE)`。
   三条铁律（各有实测背书）：
   * **重放原始路径**（`WallpaperInfo::replayPath`），绝不能是缓存层文件——
     否则 Shell 对已重编码两代的 JPEG 再编码，桌面每重放劣化一代
     （实测 CachedImage 296033→292907 字节）；
   * **不带 `SPIF_UPDATEINIFILE`**——会写用户注册表、打断幻灯片、"每屏
     不同壁纸"场景会把其它屏重置为 0 号屏的图；
   * 重放会让 Shell 重建壁纸缓存 **6-20 秒**（桌面短暂显示在另一缓存层），
     函数返回 true，调用方据此让垫底进入跟随模式（见第 1 节）。
   另：`SPI(NULL)` 会把部分配置刷成纯黑，禁用；24H2 raised desktop
   （`IsRaisedDesktop()`）下完全跳过——会直接毁掉壁纸 WorkerW（Lively
   对同款调用同样门控）。
4. `DwmFlush()`：促使 RDP 客户端及时拾取脏区域（绘制覆盖路径自带）。

**防误伤**：

* **去抖合并**：HUD 的每次"腾出矩形"先进入 App 的脏矩形并集
  （`UnionRect`），250ms 静默后才真正刷新一次——几何滑块拖动每秒触发
  数十次腾出报告，不再逐次刷新。**桌面级请求（`nullptr`）是粘性标志**：
  去抖窗口内后续的矩形报告不得把它降级成单矩形刷新（其余区域会留幽灵帧，
  2026-10-02 修复）。退出前 flush，保证不留尾巴。

**调用点**（凡是 HUD"腾出桌面层矩形"的地方）：

| 路径 | 触发 |
|---|---|
| `Hud::BeginMove` | Detach 脱离桌面层后 |
| `Hud::SetVisible(false)` / `ApplyConfig` 隐藏分支 | 隐藏后 |
| `Hud::Destroy` | 移动重建 / 退出 / Explorer 重启销毁 |
| `Hud::PlaceWindow` | 位置或尺寸**实际变化**时（按 `placedX_/Y_/W/H_` 门控；每秒的快照刷新摆放同一矩形，不会触发） |

### 已知副作用

* 常规移动 / 隐藏只走**绘制覆盖**：零闪动、Shell 零感知（实测缓存 mtime
  不变）。历史上的"每次移动伴随全桌面壁纸重绘"已随绘制覆盖的引入消失。
* SPI 兜底一旦触发（解码失败 / 会话重连），壁纸缓存流转 6-20s：桌面短暂
  显示在另一缓存层，垫底靠跟随模式对齐（120 渲染内每渲染重查裁片键）。
* 残影一旦产生（旧版本遗留），只能靠桌面 F5 / 重启 Explorer / 换壁纸清除；
  新版本不会再产生。

### 调查中的错误路线（记录在此，勿重复）

残影/透明角问题历经多轮误诊，以下是被实测否定或绕弯的路线：

1. **"渲染管线回归"**：同半径下新旧二进制角部像素 A/B 逐位一致——残影
   与渲染代码无关，回退代码作诊断不能定位此类问题。
2. **"解码 TranscodedWallpaper 即可"**：桌面显示的是第三层缓存
   （CachedFiles\CachedImage，1:1 误差 0.65/255），转码层仍差 ~15——
   必须实测"屏幕 vs 候选文件"确定显示源，不能靠推断。
3. **GetDIBits 缓冲按 RGBX 解析**：内存序是 BGRX，通道颠倒曾让所有文件
   比对得出 ~48 的伪误差，推翻过一整轮结论。
4. **测量参数硬编码**：把圆角半径写死为 32（实际配置 22），采样落进面板
   填充区，制造出一次假"回归"——测量参数必须从配置实读。
5. **`FindWindowW` 找挂载后的 HUD**：它是 WorkerW 的子窗口，FindWindowW
   只搜顶层——两次误报"窗口消失"。
6. **"外科式 RedrawWindow 足以清残影、可去掉 SPI 重放"**：实测残影
   35.8 清不掉；重放必须保留为兜底，但改为绘制覆盖为主。
7. **`CopyPixels` 的 cbStride 语义**：它按 cbStride 步长逐行写输出，而
   目标缓冲的真实行距是缓冲宽度×4——面板悬出显示器/图像边缘（信箱钳制
   使 dstW < w）时两者不等，每行错位累积成对角错切（"拖影"）。垫底路径
   自项目之初即带此 bug，被面板主体 80% 不透明掩盖；绘制覆盖把它放大到
   桌面上才暴露。修正：先拷入连续临时缓冲再逐行 memcpy（两处解码同步）。
   **教训：边界条件（区域被钳制缩小）下的数值验证不能只测典型路径。**

---

## 5. 移动模式

托盘菜单"移动 HUD 位置..."的完整生命周期：

1. **进入**（`Hud::BeginMove`）：`desktop::Detach` 脱离桌面层（child → popup，
   坐标需在 Detach 前后手动保持，`SetParent` 会按新父重解坐标）；ex 样式
   去掉 `WS_EX_TRANSPARENT | WS_EX_NOACTIVATE`、加 `WS_EX_TOPMOST | WS_EX_LAYERED`；
   渲染模式切到 ULW；若 HUD 原本隐藏则强制显示。
   若配置里 `HudVisible=0`，`App::BeginHudMove` 会**持久化** `HudVisible=1`
   （用户点"移动"的意图就是看到它、摆好它）。
2. **拖动**：`WM_NCHITTEST` 在移动模式下返回 `HTCAPTION`，整个面板即标题栏，
   走系统原生拖动循环（单个拖动内 Esc 可取消）。移动期间快照刷新照常渲染
   内容，但 **跳过 `PlaceWindow`**（否则每秒被拽回配置坐标）；健康检查
   （重挂 / Z 序 / 显示器变化）也全部跳过。
3. **完成**：`WM_NCLBUTTONDBLCLK` / `WM_NCRBUTTONUP` → `EndMove(true)`；
   记录 `GetWindowRect`，换算为"显示器序号 + 显示器相对像素"（即
   `config.ini` 的 `X/Y/Monitor` 约定），通过回调写回配置。托盘菜单在移动
   模式中再次点击等价于"就放在这里"（手势失效的恢复路径）。
4. **重建**：回调投递 `kMsgRebuildHud`，延迟销毁旧窗口、按新位置重建（不在
   HUD 自己的 WndProc 里同步销毁）。重建走的与启动完全相同的路径。

避免的坑：

* 移动态弹窗**不要**调用 `SetLayeredWindowAttributes` 之后又切回 ULW
  （文档：SLWA 之后 ULW 失效，直到样式位清/置——而子窗口清了就回不来）。
* `WS_EX_TOPMOST` 归窗口管理器管：**清除必须用 `SetWindowPos(HWND_NOTOPMOST)`**，
  直接改样式位无效。
* 移动期间 `App::HealthCheck` 与 `TaskbarCreated` 处理必须旁观
  （`hud_.InMoveMode()` 守卫），否则 2 秒一次的健康检查会把窗口拽回去。

---

## 6. 托盘回调

`NOTIFYICON_VERSION_4`（`NIM_SETVERSION` 设置成功后）回调消息的真实布局：

| | `wParam` | `lParam` |
|---|---|---|
| v4 | 打包的屏幕坐标：`LOWORD`=x，`HIWORD`=y（signed short） | `LOWORD`=事件，`HIWORD`=图标 ID |
| 旧版 | 图标 ID | 鼠标消息（值在 16 位内） |

要点：

* **`LOWORD(lp)` 在两种布局下都是事件**，事件读取与布局无关；只有坐标来源
  依赖 `version4_`（v4 用 `wParam` 里的坐标，legacy 用 `GetCursorPos`）。
  早期版本把两者读反（以为事件在 `wParam`），导致右键/左键**全部无响应**。
* **v4 模式下 shell 对同一次点击同时投递裸鼠标消息和抽象事件**。
  日志实录（一次右键）：`lp=0x10204 → 0x10205 → 0x1007B`，即
  `WM_RBUTTONDOWN → WM_RBUTTONUP → WM_CONTEXTMENU`（高 16 位的 1 是图标 ID）。
  **只处理抽象事件**：`WM_CONTEXTMENU` 弹菜单、`NIN_SELECT`/`NIN_KEYSELECT`/
  `NIN_BALLOONUSERCLICK`（气泡点击）打开设置；裸消息一律忽略。否则每次点击动作
  执行两遍——表现为菜单选中后"闪一下"又弹出来。
* 旧版布局仅在 `NIM_SETVERSION` 被拒绝时出现，由 `version4_` 区分。

菜单弹出的前置条件（各自失效时都表现为"右键没反应/闪一下就没"）：

* **owner 窗口不能是隐藏窗口，也不能是 message-only 窗口**：从未显示过的窗口无法成为前台
  窗口，`TrackPopupMenu` 的菜单会在出现的瞬间被"点击在菜单外"关掉。
  用**屏幕外 1x1、已 `ShowWindow`、带 `WS_EX_TOOLWINDOW`** 的顶层窗口最稳。
* `SetForegroundWindow` 单独调用常失败（shell 持有前台锁），可靠写法是先
  `AttachThreadInput(GetCurrentThreadId(), GetWindowThreadProcessId(GetForegroundWindow()), TRUE)`
  再 `SetForegroundWindow(owner)`，最后 `AttachThreadInput(..., FALSE)`；
  `TrackPopupMenu` 之后 `PostMessage(owner, WM_NULL, 0, 0)`。
* **校验菜单坐标**：坐标离谱时菜单会弹到看不见的地方，落在虚拟屏幕外就
  退回 `GetCursorPos()`。
* 回调日志记录**原始** `wParam`/`lParam`，是定位这类问题最快的手段。

---

## 7. ICO 资源

ICO 的每个图像条目必须是**完整 DIB**：40 字节 `BITMAPINFOHEADER`
（`biSize=40`、`biHeight=2×宽`）+ XOR 像素 + AND 掩码。

图标生成脚本曾漏写头部（条目直接以裸像素开头，`biSize=0`）。
windres 不解析 ICO 内部结构、原样嵌入，于是 exe 里有了**无法解码的图标资源**：
`LoadImageW` 静默失败 → 回退到系统默认图标 → "托盘图标不是自己的"。
`ExtractAssociatedIcon` 同样返回默认图标——它对坏数据的"成功"没有诊断价值。

生成脚本务必在写入像素前补齐头部，并用
`biSize==40 && biHeight==2*宽` 断言自检。

---

## 8. 调试方法

| 手段 | 适用 | 注意 |
|---|---|---|
| 结构探针（EnumWindows + 样式/父窗口/rect） | 窗口是否存在、挂载、可见位 | **无法判定渲染内容**；找挂载后的 HUD 要走窗口树（`FindWindowW` 只搜顶层，见第 4 节教训 5） |
| `GetLayeredWindowAttributes` 读回 | SLWA 状态是否设置成功 | 设置成功 ≠ DWM 合成（故障模式 B） |
| `DwmGetWindowAttribute(DWMWA_CLOAKED)` | 是否被 DWM 遮蔽 | 本例全为 0，排除用 |
| `PrintWindow(PW_RENDERFULLCONTENT)` | **壁纸 WorkerW 的合成画面**（含 HUD 子窗口） | 本项目像素取证的主力：BitBlt 整段黑帧的 RDP 会话里依然可用。注意它对 ULW **子窗口直接调用**是盲的——要对宿主 WorkerW 调用；解析 GetDIBits 缓冲按 **BGRX**（按 RGBX 解析 = 通道颠倒伪误差） |
| `CopyFromScreen` / `GetPixel` | 合成后像素 | RDP 会话可能整体失效（返回黑/`CLR_INVALID`）；`GetPixel` 对更高完整性级别的窗口返回无效 |
| 运行日志（`Tray callback` / `Hud:` / `App:` 行） | 事件序列、布局判定 | 裸鼠标消息别刷屏（按 `WM_MOUSEMOVE` 过滤）；`DBG` 行需 `--verbose` 启动 |

像素级比对的三个纪律（多轮误诊换来的）：

1. **屏幕 vs 候选文件用比值而非差值**：角部裁片与桌面同源时
   `mean(screen)/mean(file) ≈ 1.000`，受壁纸明暗影响小，是最灵敏的判据。
2. **单次截屏内自洽对比**：用远离面板的干净区拟合"屏幕壁纸映射"，再预测
   角部——同源同捕获，对全局色调/残影免疫。
3. **测量参数从配置实读**（圆角半径、面板坐标），不要凭记忆硬编码。

排查"窗口属性全部正常却看不见"时的判定顺序：

1. 排除窗口泄漏（`EnumWindows` 计数）。
2. 排除 cloaked / 隐藏 / 父链不可见 / 位置离屏。
3. 排除配置状态（`HudVisible`、`Paused`）。
4. 剩下的就是合成层问题——分层子窗口的故障模式 A/B/C 之一。

以上判定顺序在开发中由一组端到端验证脚本逐项复核（托盘回调全链路、
移动模式端到端：脱离 / 样式 / 落点落盘 / 重挂、宽度护栏弹窗、
配置持久化语义、窗口树 / Progman-WorkerW 层级探针等）；
这些脚本属于本地工具，不随仓库发布。

---

## 9. 文字宽度保护：自动最小宽 + 设置护栏

右对齐的值文本以 `NO_WRAP` + `CLIP` 绘制，一旦比预留区域宽，溢出部分向左伸出
被裁掉的是**开头字符**（VRAM 行两次复发：共享 56px 值区把 "x.x GB / y.y GB" 裁成
".0GB"；VRAM 专属 140px 固定值区在 12GB 卡 + 缩放显示上把 "612.4 MB / 12.0 GB"
裁成 ".4 MB / 12.0 GB"）。标签区（CPU 品牌行等）同样受宽度约束。魔法数追不胜追，
最终方案是三层测量保护（2026-09-24）：

1. **值区域实测宽**（`RenderToTarget` 的 `valueZoneW`）：
   `max(行下限×s, 实测宽 + pad + 2s)`。区域矩形是 `[width-w, width-pad]`，
   因此 `w` 必须覆盖文本**加** pad——第一版修复漏加 pad 仍被边际裁切。
2. **内容最小宽**（`Hud::ContentMinWidth`）：逐行算
   `pad + labelW + gap + valueW + pad`（Title/NET 行按整行文本），面板宽 =
   `max(80, 配置宽, widthFloor_)`。`widthFloor_` **增长即时、收缩需连续 5 帧保持
   更小**——数值逐秒变长变短，无迟滞的地板会让面板呼吸并反复重裁壁纸垫底；
   `ApplyConfig` 重置地板。`DumpToBmp` 一次性渲染，直接应用无迟滞。
3. **设置护栏**（`SettingsWindow::EnsureMinWidth`）：`Hud::RequiredPanelWidth`
   用与渲染端同一套 `BuildItemsFor` + `ContentMinWidth` + 字体构造（`MakeTextFormat`），
   按草稿配置、**目标显示器的实际 DPI**（`MonitorIndexDpi`，`GetDpiForMonitor`
   MDT_EFFECTIVE）计算最小宽；应用/确定 时若草稿宽度不足，自动抬高（编辑框同步）
   并弹窗说明。渲染端的地板仍然兜底（实时预览/INI 手改路径）。

LAN 行（Text kind）的值区域下限与 Meter 行统一（56px×s），不再用半面板宽，
测量数学对所有行一致。

**测量一致性陷阱**：比较字符串宽度必须在**同一 DPI 感知环境**下进行。独立探针
进程若不声明 DPI 感知，`LOGPIXELSX`/DWrite 都被虚拟化到 96，与应用进程
（Per-Monitor-V2，缩放显示器上真实 120）测出的宽度差一个缩放系数——这不是应用
的 bug，探针要加 `SetProcessDpiAwareness`。应用内测量与渲染共用同一 TextFormat
与缩放，天然一致。

**跨进程 UI 自动化坑**：`GetWindowTextW` 对**其他进程**的控件返回空串（它不发
`WM_GETTEXT`，只读本进程缓存的窗口标题）；驱动设置窗口做验证要用
`SendMessage(WM_GETTEXT)` 直取，`WM_SETTEXT` 则无此限制。

验证：`--dump` 墨迹扫描（默认配置 320→360 自适应——默认字号下 CPU 品牌行原本
就在被裁；窄宽 120 + 字号 17 → 面板 463、全行无裁切）；
设置窗口的宽度护栏弹窗端到端（含负向：宽度充足不弹窗）。

---

## 10. 配置与生命周期的实测教训（2026-10 审计）

2026-10 全量代码审计修复（v0.2.1）中与 API 行为假设相关的部分，单独成节。

### INI 编码：profile API 不认 UTF-16 BOM（实测）

为修"西文系统上把宋体存成 `??`"，曾把 config.ini 改为 UTF-16LE+BOM 写盘
（"`GetPrivateProfileStringW` 认 BOM"是流传很广的说法）。用 15 行最小程序在
Win10 19045 上实测：**profile API 按原始 ANSI 字节读文件，完全找不到节，
全部返回默认值**——若上线会直接毁掉配置读取，冒烟测试都没拦住（`--dump`
读的是旧 ANSI 文件，不触发保存路径）。结论：

* INI 编码锁定 **CP_ACP**，这是硬约束不是偏好（`config.cpp` 顶部注释）；
* 编码问题的修法放在 UI 端：字体下拉过滤掉 CP_ACP 不可表示的名字
  （`EncodableInAcp`，`WideCharToMultiByte` + `WC_NO_BEST_FIT_CHARS`），
  不把存不下的值提供给用户。

**教训：跨 API 的行为假设先写最小程序实测，且冒烟测试要覆盖"写回再读入"的环路。**

### WM_ENDSESSION 与 GWLP_USERDATA 的顺序

`Shutdown()` 先清 `GWLP_USERDATA` 再 `DestroyWindow`，`WM_DESTROY` 在静态
`WndProc` 里取不到 `self`，走到 `DefWindowProc`——`PostQuitMessage` 永远不执行，
消息循环不退出：每次注销/关机应用都拖到系统强杀超时。修复：`WM_ENDSESSION`
处理完直接 `PostQuitMessage(0)`，不依赖 `WM_DESTROY`。

**教训："经 userdata 分发"的静态窗口回调，销毁路径要专门走查一遍
（成员清理与消息派发的先后顺序）。**

### latest-wins 去重会吞掉失败重试

垫底解码失败后注释写着"渲染节流会重试"，但重试入口 `ScheduleUnderlay` 被
`key == underlayKey_` 去重拦下（失败不改键），"重试"落空。修复：worker 失败置
`underlayReqFailed_`，调度端见标志放行同键重排。

**教训："稍后会重试"的注释必须指出具体调用路径；latest-wins 去重的队列
必须有失败旁路。**

### 两个显示器枚举的顺序没有文档保证

`Config::monitorIndex` 是 `EnumDisplayMonitors` 序，曾直接当
`IDesktopWallpaper::GetMonitorDevicePathAt` 的下标用。两者顺序不被任何文档
保证一致，多屏不同壁纸时会解码另一台显示器的图。修复：`GetMonitorRECT`
逐一取矩形按**几何相等**匹配（显示器矩形天然唯一），失配回退原下标。

### 自启动只加不删

启动路径 `if (autoStart) SetAutoStart(true)`：手改 INI `AutoStart=0` 永远清不掉
已有的 Run 键。改为无条件 `SetAutoStart(cfg_.autoStart)` 双向对齐
（`SetAutoStart(false)` 对不存在的键容错）。

### 其他小修（一句话备查）

* `RawSMBIOSData` 表区 = 8 字节头 + `Length` 字节：扫描终点是
  `data + 8 + Length`（少加 8 会漏掉表尾），`Length` 需按 API 返回值夹取。
* `GetPrivateProfileStringW` 最多返回 `nSize-2` 字符：缓冲增长判据是
  `n == size-2`（用 `n < size-1` 时增长分支是死代码，长值静默截断）。
* 网络速率基线要携带适配器 LUID：切换网卡时重新起步，不跨卡相减
  （否则一个 tick 的 TB/s 级假尖峰）；`GetIfTable2` 失败同样重置基线。
* NVML 失联（驱动重置/TDR）后句柄作废但 `nvmlUp_` 仍为 true：核心调用
  连续失败 ~5 秒后卸载重初始化；`getCount()==0` 闩锁避免无卡系统空转。
* 日志轮转 `MoveFileW` 会因外部程序占用失败（无 `FILE_SHARE_DELETE`）：
  失败时截断兜底，512 KB 上限继续生效；轮转在锁内，告警只能走
  `OutputDebugString`（`log::Warn` 会递归死锁）。
* 被后台线程 `PostMessage` 的 HWND 要在销毁路径上提前解绑（窗口句柄可能
  被复用）；共享状态（此处 `notifyWnd_`）用同一把互斥量保护读写。

---

## 11. 已知限制

* 挂载态（桌面层）面板**不透明**：这是规避跨进程分层子窗口缺陷的代价；
  移动态拖动过程仍为逐像素半透明。视觉半透明始终生效：半透明面板叠加在
  垫底之上（真实壁纸裁片，或无壁纸文件 / TILE / SPAN 时的纯色背景垫底），
  不透明度滑块对挂载态与移动态都有效。
* 常规移动 / 隐藏经绘制覆盖清残影，不重放壁纸；仅解码失败 / 会话重连走
  SPI 兜底（6-20s 缓存流转，垫底自动跟随）。"每显示器不同壁纸" / 幻灯片
  场景下兜底重放的行为需关注。
* INI 为系统代码页存储（第 10 节）：代码页不可表示的字体名不在设置下拉中
  提供；手改这类值在保存时仍会变成 `??`。
* RDP 会话中 `CopyFromScreen` / `GetPixel` 可能整体失效；用
  `PrintWindow(壁纸 WorkerW, PW_RENDERFULLCONTENT)` 替代（见第 8 节）。

---

## 12. 参考

* Lively Wallpaper `RefreshDesktop()`（同款问题的同款修复）：
  [WinDesktopCore.cs](https://github.com/rocksdanister/lively/blob/core-separation/src/Lively/Lively/Core/WinDesktopCore.cs) ·
  [SetupDesktop.cs（旧分支）](https://github.com/rocksdanister/lively/blob/master/src/livelywpf/livelywpf/wp_lib/SetupDesktop.cs)
* Raymond Chen，跨进程 SetParent 的坑：
  [Is it legal to have a cross-process parent/child window relationship?](https://devblogs.microsoft.com/oldnewthing/20130412-00/?p=4683) ·
  [What happens if I don't paint when I get WM_PAINT?](https://devblogs.microsoft.com/oldnewthing/20141203-00/?p=43483) ·
  [窗口 cloaking](https://devblogs.microsoft.com/oldnewthing/20200302-00/?p=103507)
* MSDN：[SetParent](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setparent)（重绘注记、DPI 强制重置表） ·
  [RedrawWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-redrawwindow)（桌面只走 `RDW_ERASE`、跨进程同步标志无效） ·
  [UpdateLayeredWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-updatelayeredwindow) ·
  [Window Features（分层窗口 / SLWA 后 ULW 失效）](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features) ·
  [NOTIFYICON_VERSION_4 消息布局](https://learn.microsoft.com/en-us/windows/win32/shell/notification-area)
* WorkerW / `0x052C` 拓扑：[dynamicwallpaper.readthedocs.io](https://dynamicwallpaper.readthedocs.io/en/docs/dev/make-wallpaper.html) ·
  [AutoHotkey "Always On Bottom"](https://www.autohotkey.com/boards/viewtopic.php?f=76&t=80343)
