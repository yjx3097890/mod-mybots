# mod-mybots 寻路原理说明

本文解释当前模块「怎么走路」：同图贴地走、远距离坐鸟、跨大陆换图。重点把代码里的英文名翻译成中文，并说明为什么这么设计。

相关代码：`src/MyBotsNav.*`（同图）、`src/MyBotsTravel.*`（跨图）、`src/MyBotsExecutor.cpp` 里的 `IssueMove` / `MoveTo`。

---

## 1. 一句话结论

| 场景 | 实际怎么走 | 绝不做什么 |
|---|---|---|
| **同一张地图** | 用 **navmesh（导航网格）** 绕障碍贴地走 | 不接受「穿山穿墙的直线」当合法路 |
| **同图很远** | 先找 **taxi（飞行点）** 登机，落地后再走网格 | 不假装飞到空中（必须靠近真正的飞行管理员） |
| **同图隔海 / 爬树** | 船（如 Moonspray）或 **树下粉门**（Rut'theran↔达纳苏斯） | **绝不沿树干爬上去**；有门就走 `transfer_portal` |
| **另一张地图** | 规则：炉石 → 船/飞艇/传送门 → 否则失败 | **绝不在错误地图上朝目标坐标直线冲** |

早期看到的「直来直去、穿模、橡皮筋」，多半不是「没开寻路」，而是：跨图目标却在本图乱走，或引擎给出了假路径（直线）却被当成真路径执行了。

---

## 2. 分层：谁负责哪一段

```
作业（move_to / travel_to / complete_quest…）
        │
        ▼
┌───────────────────┐
│ MyBotsExecutor    │  每 tick 推进一步：要不要跨图？要不要坐鸟？发不发走路命令？
└─────────┬─────────┘
          │
     ┌────┴────┐
     ▼         ▼
MyBotsTravel   MyBotsNav + IssueMove
（跨图规则）    （同图：算可走点 → 让角色沿网格走）
     │              │
     │              ▼
     │         AzerothCore 引擎
     │         PathGenerator / MotionMaster / MovePoint
     │              │
     │              ▼
     │         磁盘上的 mmaps（导航网格数据）
     ▼
playerbots_travelnode（船/门）+ TravelMgr（坏格子、多跳飞行图）
```

- **本模块**：决定「去哪、用什么交通方式、假路径拒不执行」。
- **引擎**：真正算折线、驱动角色移动。
- **playerbots**：坏格子 / 多跳飞行只读 `TravelMgr`。船和传送门改从 `playerbots_travelnode` 自己装，因为现版 playerbots 启动时不再 `loadMapTransfers`。

---

## 3. 术语表（英文 → 中文 + 含义）

### 3.1 地图与坐标

| 英文 | 读法/简称 | 含义 |
|---|---|---|
| **map / mapId** | 地图编号 | 一张独立世界。`0`=东部王国，`1`=卡利姆多，`530`=外域，`571`=诺森德。**不同 map 上的 xyz 互不相通**。 |
| **x, y, z** | 坐标 | 位置。z 是高度。目标点「在楼顶还是楼底」全靠 z。 |
| **zone** | 区域 | 更大的地理分区（如艾尔文森林），主要用于显示/任务，不是寻路主键。 |
| **WMO** | 世界模型物体 | 城里的房子、酒馆等多层建筑。寻路若选错楼层，角色会「掉到楼下」。 |

### 3.2 导航网格（同图走路的核心）

| 英文 | 含义 |
|---|---|
| **navmesh**（navigation mesh） | **导航网格**：把可行走地面切成许多多边形。角色只能在这些多边形上走，自然绕开山、墙、悬崖。 |
| **mmap / mmaps** | AzerothCore 把导航网格存成的数据文件（movement maps）。没加载对应格子时，引擎可能退回「画直线」。 |
| **tile** | 网格瓦片：大地图按格子分块加载。远处目标的 tile 可能还没加载。 |
| **polygon / poly** | 网格上的一块可行走多边形。角色「脚下没有 poly」≈ 掉穿地板（off-mesh）。 |
| **PathGenerator** | 引擎类：从当前位置到目标点算一条折线路径。 |
| **CalculatePath** | 调用寻路计算。参数里的 `forceDest` 若乱开，容易强行终点而穿障碍。 |
| **GetPathType / PATHFIND_*** | 路径质量标记，见下一小节。 |
| **PrepareWalkTarget** | **本模块**的函数：在真正下令走路前，把目标修成「脚能踩到、且有可信网格路径」的点；修不了就返回失败。 |
| **IssueMove** | **本模块**发移动：先 `PrepareWalkTarget`，再 `MovePoint(..., generatePath=true)`。 |
| **MotionMaster** | 引擎移动调度器：管角色当前是站立、走路、飞行等。 |
| **MovePoint** | 「走到某一点」的指令。本模块开启 **generatePath**，让引擎再沿 navmesh 生成路径，而不是直线位移。 |
| **POINT_MOTION_TYPE** | 当前正在执行「走到点」这类运动。 |
| **spline** | 客户端/服务器上的平滑移动曲线。坐鸟、走路都可能是一条 spline。 |

### 3.3 路径类型（`PATHFIND_*`）——假路径从哪来

| 标志 | 含义 | 本模块态度 |
|---|---|---|
| **PATHFIND_NORMAL** | 正常完整路径 | 优先接受 |
| **PATHFIND_INCOMPLETE** | 只能走到中途（远点 tile 未加载等） | 可接受，边走边让后续 tick 再算 |
| **PATHFIND_NOPATH** | 算不出路 | 拒绝 |
| **PATHFIND_NOT_USING_PATH** | **没用网格**，往往是直线 | 有网格的地图上 **拒绝**（这是「直来直去穿山」的主因之一） |
| **PATHFIND_SHORTCUT** | 抄近路/捷径类结果 | 有网格时拒绝 |
| **PATHFIND_SHORT** | 过短/不可靠 | 有网格时拒绝 |
| **PATHFIND_FARFROMPOLY_START** | 起点离网格很远 | 视为可能穿地，触发纠正 |

本模块用 **`IsCredibleMeshPath`（可信网格路径）** 过滤：长距离却只有 2 个点（起点+终点）的「乌鸦直线」（crow-flies）也会被拒绝。

### 3.4 卡住与补救

| 英文 | 含义 |
|---|---|
| **stuck** | 卡住：一段时间几乎没位移。 |
| **detour** | 绕路：卡死后向侧面偏一点再试。 |
| **bad point / MarkBadPoint** | 把某个失败坐标记为「坏点」，短时间内别再往那儿冲。 |
| **isBadMmap** | playerbots 已标记的坏网格格子；本模块可读，避免重复踩坑。 |
| **CorrectIfUnderground / off-mesh** | 若判定角色掉到网格下，把他抬回脚下地面（有冷却，避免每帧传送造成橡皮筋）。 |
| **ApproachWaypoint** | 终点太远或 tile 未加载时：先朝目标方向走一段「已有真路径」的中间点，边走边加载。 |
| **GetMapHeight** | 取某 (x,y) 附近地面高度。必须结合「靠近请求的 z」选楼层，不能从天上乱扫（否则会吸到洞穴底）。 |
| **rubber-band（橡皮筋）** | 服务器改位置/重算路径过猛，客户端看起来猛拽一下。常与每 tick `Clear()`、反复 `MovePoint`、频繁抬高度有关。 |

### 3.5 飞行点（同图远距离）

| 英文 | 含义 |
|---|---|
| **taxi** | 飞行点交通（狮鹫/双足飞龙等）。 |
| **TaxiNode / flight path** | 飞行点节点与航线。 |
| **GetNearestTaxiNode** | 找离某坐标最近的飞行点。 |
| **GetTaxiPath** | 查两点之间是否有 **直达** DBC 航线。 |
| **FindTaxiPath** | playerbots 的 **多跳** 飞行图（BFS）：例如 A→B→C。 |
| **FlightMaster** | 飞行管理员 NPC。本模块要求必须走近真人 NPC 再 `ActivateTaxiPathTo`，禁止空传登机（曾导致「突然出现在金郡天上」）。 |
| **ActivateTaxiPathTo** | 引擎 API：开始一段飞行。 |
| **in_flight / UNIT_FLAG_TAXI_FLIGHT** | 正在坐鸟；期间不要 `Clear()` 走路，否则会登机→掉落→再登机死循环。 |

### 3.6 跨图旅行

| 英文 | 含义 |
|---|---|
| **cross-map** | 目标在另一张 `mapId`。 |
| **MyBotsTravel / AdvanceCrossMap** | 跨图推进逻辑。 |
| **hearthstone / homebind** | 炉石；角色绑定点所在地图。绑定图=目标图时可炉石过去。 |
| **TravelMgr** | playerbots 旅行管理器：任务点、坏 mmap、跨图边等。 |
| **mapTransfer** | 一张「从 A 图某点 → B 图某点」的交通边（船、飞艇、区域触发传送门等）。 |
| **continent transfers** | 启动时从 `playerbots_travelnode` + `_link` 只读 **type 2/3（门/船）**、地图 `0/1/530/571` 的边；按阵营过滤飞艇/联盟船。不灌 walk path。 |
| **transfer_approach** | 走向出发码头。上船后改为 `transfer_aboard`，**禁止再 MovePoint**（否则船一开就会在船里往回走）。 |
| **transfer_portal** | 同图走入 AreaTrigger（如泰达希尔树下粉门）。到点后继续朝触发体积挪，靠坐标突变判断到达，**不是** `GetTransport()`。 |
| **transfer_aboard** | 已在船/飞艇上，停步等换图或到岸。 |
| **transfer_disembark** | 到目标图后走到干码头，再继续同图任务。 |
| **transfer_waiting** | 已到点，等船/触发器把人换到另一张图。 |
| **AdvanceLocalTransfer** | 同图船/门状态机：坐鸟落地后若 Z 落差大（达纳苏斯），优先树下粉门。 |
| **cross_map_unreachable** | 炉石和 transfer 都用不上 → **安全失败**，原地停，不乱走。 |
| **travel_to** | 作业/LLM 操作：带 `map` 的跨图移动意图（可带 xyz 或 NPC entry）。 |
| **MAP_UNSPECIFIED** | 「payload 没写 map」。注意：**东部王国是 map `0`，`0` 是合法目标图**，不能当成「没指定」。 |

### 3.7 作业侧常见 result 字符串

| 字符串 | 意思 |
|---|---|
| `moving` | 同图正在走 |
| `arrived` | 到了 |
| `unreachable` / `stuck` | 同图走不通或卡死超限 |
| `taxi_approach` / `taxi_boarded` / `taxi_boarded_multi` / `taxi_no_flightmaster` | 走向飞行点 / 已登机 / 多跳登机 / 在节点等飞行管理员（不会因此改去走路穿海） |
| `hearth_cast` / `hearth_pending` | 正在炉石 / 等炉石完成 |
| `transfer_approach` / `transfer_approach;via=` / `transfer_portal` / `transfer_waiting` / `transfer_aboard` / `transfer_disembark` | 走向码头 / 树下粉门 / 等船 / 已在船上别乱动 / 到岸后走到干码头 |
| `local_portal_arrived` / `local_boat_arrived` | 同图门/船腿完成，继续朝任务点走 |
| `cross_map_unreachable;from=;to=;hearth=;transfers=` | 跨图无可用规则（当前图/目标图/炉石图/本阵营可用边数） |
| `cross_map_combat` | 跨图时先在打架，暂缓 |

---

## 4. 同图走路：原理（逐步）

目标：从 A 走到同图的 B，看起来像真人绕路，而不是穿模。

1. **作业**给出目标 `(x,y,z)`（可选 `dist` 到达半径）。
2. **MoveTo** 发现「当前 map == 目标 map」（或未指定 map）→ 走同图分支。
3. （可选）距离够远且配置开了 taxi → **TryTaxi**：走近 FlightMaster → 登机 → 落地后再走。
4. （可选）同图船/门：`LocalBoatHelps` → **AdvanceLocalTransfer**。达纳苏斯目标且已在鲁瑟兰附近时走 **树下粉门**（`transfer_portal`），不爬树。
5. **IssueMove**：
   - 若已在朝同一目标走，且未到重算间隔 → **不再** `Clear`/`MovePoint`（减少橡皮筋）。
   - 偶尔检查是否掉穿地并抬回。
   - 调用 **PrepareWalkTarget**：
     - 在目标附近用 **GetMapHeight** 找合理楼层（优先接近请求的 z，避免吸到楼下）。
     - 用 **PathGenerator** 试算；只接受 **可信网格路径**。
     - 太远则 **ApproachWaypoint** 先走中间可达点。
   - 成功则 **MovePoint(..., generatePath=true)**：引擎沿 navmesh 生成折线并驱动移动。
6. 每 tick 看距离是否 ≤ `dist` → `arrived`；卡住则 detour / 记坏点 / 失败。

```
请求目标 B
    │
    ▼
PrepareWalkTarget：B 是否在可信网格上？
    │ 否 → 试中间路点 / 失败（不发直线 Move）
    ▼ 是
MovePoint(generatePath=true)
    │
    ▼
引擎沿 navmesh 折线移动 → 到达 / 卡住重试
```

**原理要点**：真正「聪明」的是 **navmesh + PathGenerator**；本模块的价值是 **挑终点、拒假路径、少打断、跨图别乱用同图走路**。

---

## 5. 跨图：原理（逐步）

目标：人在 map A，任务在 map B。

1. payload 带 **`map: B`**（包括 `map: 0`）。
2. **MoveTo** 发现 `当前 map ≠ B` → 只走 **AdvanceCrossMap**，不对本图发「朝 B 的 xyz 直走」。
3. 规则顺序：
   1. **炉石**：`homebind` 在 B → 施放炉石，等换图。
   2. **船/门**：在已加载的 continent transfers 里找 A→B 最合适的边（按阵营过滤）→ 设 `travelLeg*` 为本图码头/门口 → 用 **同图 navmesh** 走到那里 → `transfer_waiting` 等引擎换图。
   3. 否则 **`cross_map_unreachable`** 失败。
4. 一旦 `GetMapId() == B` → 视为跨图到达，再进入第 4 节同图走路去最终 xyz。

```
人在 map A，目标在 map B
        │
        ├─ 炉石绑定在 B？ ──是──► 炉石 ──► 到 B 后再 navmesh
        │
        ├─ 有 A→B 的船/门（travelnode）？ ─是─► 本图走到登船点 ──► 等换图 ──► 到 B 后再 navmesh
        │
        └─ 都没有 ──► 失败（原地），禁止在 A 上朝 B 的坐标冲
```

**原理要点**：跨图不是「再算一次更长的 navmesh」，而是 **先换地图，再在新地图上寻路**。船和门是「换图交通」，不是网格上的普通路点。

---

## 6. 和 playerbots 的关系

| 能力 | 来源 |
|---|---|
| 同图 PathGenerator / MovePoint | AzerothCore 引擎（playerbots 也用同类手段） |
| 坏网格格子 `isBadMmap` | 只读 `TravelMgr` |
| 船/门 | 只读 `playerbots_travelnode`（节点+跨图 link）；现版 TravelMgr 启动时表是空的 |
| 多跳飞行 `FindTaxiPath` | 只读 `TravelNodeMap` |
| 炉石动作 | 优先调用 playerbots 的 `hearthstone` action |
| 战斗、职业循环 | 仍由 playerbots；作业期间会关掉部分 RPG/travel 自主策略，避免抢控制 |

编译开关 **`MYBOTS_HAVE_TRAVELMGR`**：坏格子 / 多跳飞行仍只在检测到兼容 TravelMgr 头文件后启用。船和传送门走 `playerbots_travelnode` SQL，不依赖该开关。

---

## 7. 常见误解

1. **「直来直去 = 没开寻路」**  
   同图默认已是网格寻路。直线往往是：假路径（`NOT_USING_PATH`）被执行了，或跨图时在错误地图上朝无效坐标走。

2. **「map 写 0 等于没写 map」**  
   已修复。`0` 是东部王国；未指定用 `MAP_UNSPECIFIED`（`0xFFFFFFFF`）。

3. **「坐鸟可以远程激活」**  
   不行。必须走近 FlightMaster，否则会出现诡异空中瞬移。

4. **「跨图失败就是坏了」**  
   若角色炉石不在目标图、当前位置又对不上已知船/门，**安全失败是设计**。要比「穿越大海直线走」正确。

5. **「LLM 负责绕开石头」**  
   LLM 只规划高层步骤（如先 `travel_to`）；贴地绕障是 navmesh 的事。

---

## 8. 配置相关（名字解释）

具体键名以 `conf/mybots.conf.dist` 为准，常见概念：

| 概念 | 作用 |
|---|---|
| 是否启用 taxi | 关则同图再远也只走网格 |
| taxi 最小距离 | 近距离不坐鸟 |
| 重算路径间隔 | 避免每 tick 重寻路导致抽搐 |
| 卡住重试次数 / 绕路 | stuck 与 detour 策略 |
| 是否使用 TravelMgr | 坏格子（船/门不走这个开关，启动必载 travelnode 跨图边） |

---

## 9. 阅读代码的推荐顺序

1. `MyBotsExecutor.cpp` → `IssueMove`、`MoveTo`（总调度）
2. `MyBotsNav.cpp` → `PrepareWalkTarget`、`TryTaxi`、`IsCredibleMeshPath`
3. `MyBotsTravel.cpp` → `AdvanceCrossMap`（跨图规则）
4. 引擎侧（AzerothCore 源码，不在本仓库）：`PathGenerator`、`MotionMaster::MovePoint`

---

*文档对应实现：跨图安全失败、可信网格过滤、炉石 / mapTransfer / 多跳 taxi。若行为与本文不符，以当前分支代码为准。*
