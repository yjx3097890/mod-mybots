# mod-mybots 架构设计

- 状态：草案 v0.1
- 日期：2026-09-11
- 范围：在 AzerothCore Playerbot fork 上，用独立模块托管指定角色、用 Web 管理端下发任务，并在后续接入 LLM 规划。
- 非目标：重写 Playerbots 战斗循环；替代官方 `mod-playerbots`；把 SOAP 当任务 API。

配套阅读：上游 [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots)、[discussion #2275](https://github.com/mod-playerbots/mod-playerbots/discussions/2275)（命令端口与异步意图）、[AzerothCore Remote Access](https://www.azerothcore.org/wiki/remote-access)。

---

## 1. 结论

1. **引擎用 Playerbots，导演写在本仓库。** `mod-playerbots` 原样安装、可 `git pull`。本模块只编排「这个角色下一步做什么」。
2. **Web 管理端禁止直连 worldserver。** 对外只暴露 sidecar REST/WebSocket；worldserver 的 Intent/API 只给同 Docker 网络内的管理后端，不对浏览器、不对公网。不要把 SOAP、RA telnet、Playerbots `:8888` 命令端口暴露给浏览器。
3. **任务系统用 HTN（分层任务网）+ 行为树执行。** 这是 Killzone 等 3A 的成熟做法，比在 WoW 任务上硬套 GOAP 更合适。战斗仍交给 Playerbots。
4. **巡逻是「路点环 + MMAP 跟随 + 战斗挂起/恢复」，不是每 tick 重新寻路。** 远距离先走 Taxi/传送图，近距离再走 navmesh。
5. **LLM 只做规划器。** 输出 HTN 目标或步骤，不输出技能和坐标幻觉。所有写世界的操作必须在地图线程校验后再执行。

---

## 2. 系统上下文

```
浏览器 Web 管理端
        │ HTTPS + 会话/JWT
        ▼
  mybots-gateway（sidecar，Node/Go/FastAPI）  ← Docker
        │ 读：MySQL（characters + mybots_*）
        │ 写：Docker 内网 Intent JSON（认证、限流）
        ▼
  worldserver（Playerbot 核心分支）            ← Docker
        ├─ mod-playerbots     战斗 / 移动原语 / Selfbot
        └─ mod-mybots         导演、作业、巡逻、意图队列
                │
                ▼
        3.3.5 客户端（操作者在线观看）
```

实际部署：**worldserver 与管理系统（Gateway）都运行在 Docker 中**。Intent / P0 模块 API 只在 Docker 内网（或 host 网络本机）互通，不对公网暴露。

| 进程 | 职责 | 不做什么 |
|---|---|---|
| Web | 选角色、下发作业、看状态、看日志 | 不持有游戏权威、不算路、不调 LLM 热路径 |
| Gateway | 鉴权、参数校验、作业持久化、把意图投递给模块 | 不直接改 `Player` 对象 |
| `mod-mybots` | 把作业展开成步骤，在地图线程执行/取消/恢复 | 不实现职业循环 |
| `mod-playerbots` | Selfbot、`MoveTo`、接交任务 Action、战斗 | 不被本仓库改业务文件 |

---

## 3. 为什么不直接用现成远程接口

AzerothCore 官方远程通道有四条：游戏内命令、控制台、RA telnet、SOAP。Playerbots 另外有 `AiPlayerbot.CommandServerPort`（默认 8888）。

| 通道 | 现状 | 能否给 Web 用 |
|---|---|---|
| SOAP `:7878` | HTTP POST XML，`executeCommand` 任意 GM 命令，Basic Auth | **不能当产品 API。** 字符串命令、难结构化、权限过大。可留给「公告 / server info」这类运维，且仅本机。 |
| RA telnet `:3443` | 明文、按会话、返回 CLI 文本 | 不适合浏览器和作业队列。 |
| Playerbots `:8888` | 明文 TCP、无认证、thread-per-connection、主要面向随机机器人遥测（`state/position/hp/strategy`） | **禁止对公网。** 可作调试参考，产品控制面要自建。 |
| 游戏内 `.playerbots` | 已有 Selfbot 与策略命令 | 给玩家应急；Web 不走这条。 |

正确控制面：**观察 / 转向 / 管理** 三层（与 #2275 一致）。

- **Observe**：角色是否在线、位置、当前作业、步骤、卡住原因。读库或无锁快照，不进地图 tick 的长路径。
- **Steer**：创建/暂停/取消作业、切巡逻、启停 Selfbot。只投递小意图。
- **Admin**：改配置、热加载脚本。与 Steer 分权限。

硬约束（同样来自 #2275 与 AzerothCore `Map::Update`）：

- 所有改世界的操作只在该地图线程执行。
- HTTP、LLM、数据库慢查询不得阻塞 `Map::Update`。
- 回写只允许小对象：`Noop | EnableSelfbot | AssignJob | CancelJob | Pause | Resume`。
- 意图队列有上限；满了返回 429，绝不在地图线程里等。

---

## 4. 模块内部分层

```
IntentServer（Docker 内网 / host 本机）
        │
        ▼
JobQueue  ──persist──►  MySQL mybots_job / mybots_job_step
        │
        ▼
Director（每角色一份，秒级 tick，不是 100ms）
        │ 展开 HTN
        ▼
Executor  步骤：MoveTo / Interact / Accept / TurnIn / Gossip / Patrol / Wait
        │ 调用
        ▼
PlayerbotAI  DoSpecificAction / MovementAction / ChangeStrategy
        │
        ▼
Player + WorldSession  ──SMSG──►  客户端画面
```

Selfbot 开启后，对本角色执行：

```
nc -rpg quest,-travel
```

避免官方 RPG 与导演抢目标。战斗策略保持开启。

可选的最小核心补丁（尽量上游化，不放业务）：

- Selfbot 期间只丢弃客户端主动移动包（WASD/心跳等），**放行**传送 ACK / 强制速度 ACK，避免加速后回弹。
- 若缺少「把意图投到指定地图线程」的公开入口，再对 `PlayerbotWorldThreadProcessor` 做薄封装。

---

## 5. 任务系统：采用 HTN，而不是 GOAP 打天下

### 5.1 业界分层（3A 与 MMO 的共识）

现代可中断 NPC 几乎都是三层，而不是一个大状态机：

| 层 | 代表 | 频率 | 在本项目中的角色 |
|---|---|---|---|
| 目标选择 | Utility AI / 以后 LLM | 秒～十秒 | 「做任务 123」「巡逻铁炉堡门口」「空闲磨怪」 |
| 规划 | **HTN**（Killzone）或 GOAP（F.E.A.R.） | 目标变化、失败、事件时 | 把「完成任务 123」拆成可执行步骤 |
| 执行 | **行为树** | 每步 / 与 Playerbots tick 协作 | 当前步骤 RUNNING/SUCCESS/FAILURE；战斗可打断 |

GOAP 适合「世界状态差什么就凑什么」（找食物、找掩护）。WoW 任务是**作者写好的方法**：先找谁、点第几句 gossip、去哪个区域刷。HTN 的 method 分解天生对得上任务链。GOAP 全图搜索既贵又容易走出非法步骤。

行为树负责可打断：巡逻 RUNNING 时进战斗 → 挂起巡逻 → Playerbots 打完 → 从最近路点恢复。不要用扁平 `if/else` 堆任务。

### 5.2 作业与原语

Web 下发的是 **Job**，不是「帮我练级」这种自然语言（自然语言留给 V2 LLM，且必须编译成 Job）。

Job 类型（第一期）：

| type | 含义 |
|---|---|
| `selfbot.enable` / `selfbot.disable` | 挂/摘 AI |
| `move_to` | 去坐标或 NPC |
| `accept_quest` | 到 giver 接指定任务 |
| `complete_quest` | 做目标并交 |
| `turnin_quest` | 仅交 |
| `patrol` | 路点环 |
| `grind` | 在区域内击杀直到条件 |
| `idle` | 取消作业，只保留战斗自卫 |
| `cancel` | 取消当前作业 |

HTN 原语（Executor 只认这些）：

| op | 成功条件 | 失败/重试 |
|---|---|---|
| `move_to` | 进入半径 | 卡住超时、地图不对 |
| `interact` | 选中单位/物体 | 不在世界、距离不够 |
| `gossip_select` | 选项存在并点中 | 菜单变了 |
| `accept_quest` | 日志出现 quest_id | 满栏、等级、前置 |
| `turnin_quest` | 日志移除或奖励窗 | 目标未完成 |
| `use_item` | 物品成功使用 | 没有物品 |
| `wait` | 到时 | — |
| `until` | 子条件为真 | 超时 |
| `assist_combat` | 交给 Playerbots，条件满足后返回 | 死亡 |

实现上优先调用已有名字，而不是重写包：

- `DoSpecificAction("accept all quests")`
- `DoSpecificAction("talk to quest giver")`
- `MovementAction` / travel target
- 不够用再发 `CMSG_QUESTGIVER_ACCEPT_QUEST`、gossip opcode

指定 `quest_id` 时，通用「接全部」不够，本模块要做「只接这一条」的薄封装。

### 5.3 任务脚本（数据，不是硬编码）

复杂任务（多步 gossip、用物品、点物体、护送）用 `quest_id → method` 表，热加载，不改 C++。

```json
{
  "questId": 33,
  "title": "Wolves Across the Border",
  "preconditions": ["level>=2", "not_completed:33"],
  "methods": [
    {
      "name": "default",
      "steps": [
        { "op": "move_to", "npc": 197, "radius": 4 },
        { "op": "accept_quest", "quest": 33 },
        {
          "op": "until",
          "cond": "quest_objective:33:0",
          "timeoutMs": 900000,
          "child": { "op": "grind_entry", "entry": 299 }
        },
        { "op": "move_to", "npc": 197, "radius": 4 },
        { "op": "turnin_quest", "quest": 33 }
      ]
    }
  ]
}
```

没有脚本的任务：用 TravelMgr / 任务关系表生成「找 giver → 去目标点 → 找 taker」的默认 method。脚本是优化，默认生成是覆盖面。

失败策略：步骤失败 → 同 method 重试 N 次 → 换 method → Job `failed` 并写原因（距离、满栏、死亡、脚本过期）。Web 能看到，不静默卡死。

---

## 6. 巡逻与寻路

### 6.1 推荐模型

对标 MMO 服务端（Game AI Pro 分层查找表、Hytale `Patrol` 目标、通用 navmesh 跟随）：

1. **路点环（Patrol Circuit）**  
   有序点：`(map, x, y, z, waitMs, optional emote/say)`。`loop=true` 循环，否则走完 Job 成功。
2. **跟随**  
   当前路点用 AzerothCore `PathGenerator`（MMAP）或 Playerbots `MoveTo`。算路可异步；跟随在地图线程。
3. **分层远路**  
   跨区：先 Taxi / 传送门 / 船（高层图），落地后再 MMAP。不要对铁炉堡→暴风城一次扫全大陆网格。
4. **卡住检测**  
   位置在 T 秒内位移小于阈值 → 重寻路 → 换侧面点 → 标记路点坏并跳过。
5. **战斗挂起**  
   `onCombat = suspend`：把巡逻栈压住，Playerbots 进战斗；脱战回到最近路点（不要从头来）。`leash` 超出则放弃当前怪，回线。
6. **不要每 tick 重规划**  
   路有效就跟着走。重规划时机：到达、卡住、地图切换、目标作废、操作者取消。

### 6.1.1 当前落地（`MyBotsNav` + `MyBotsExecutor::MoveTo`）

- 跟随交给 `MotionMaster::MovePoint`（内部走 MMAP），目标高度先经 `PathGenerator` + `GetMapHeight`（与 Playerbots `SearchForBestPath` 同思路）解析；**禁止**从高空 `GetHeight` 贴地（会贴到洞穴层导致钻地）。`forceDestination=false`，走不通不强冲。
- 候选高度按**离目标 Z 最近**取，不取路径最短：穿建筑落到楼下的那条路往往更短，按最短选会把角色送到目标脚下那一层（暴风城地底 / 猪与哨声旅店楼下）。严格 pass 拒绝比目标低 5 码以上的楼层，只有严格 pass 全空时才放宽（应对手写坐标 / 过期 spawn 数据）。
- `PATHFIND_NOT_USING_PATH` 视为无效路径。目标格子的 mmap 瓦片未加载时 `CalculatePath` 会返回 `NORMAL | NOT_USING_PATH` 加一条穿越一切的直线——这是"走直线穿墙"的主因。此时改走 `ApproachWaypoint`：沿直线方向取 160/120/80/50/30 码处仍有真实网格路径的中间点，边走边加载瓦片，后续 tick 自然接上真目标。彻底找不到路时 `move_to` 返回 `unreachable` 原地不动，交给卡住逻辑绕行，绝不硬发直线。
- `CorrectIfUnderground` 只在角色**确实掉出导航网格**（`IsOffMesh`：向前 6 码探路得到 `NOPATH`/`FARFROMPOLY_START`）时才纠正，避免把站在桥下的角色瞬移到桥上；探测高度从头顶 40 码往下找，否则在城市 WMO 下面只会找到更低的地形。
- 远路只做了 Taxi 一层：直线距离超过 `MyBots.Nav.TaxiMinDistance` 时，找出发/落地两个 taxi 节点，先走到出发节点再 `ActivateTaxiPathTo`。落地节点必须比自己明显更接近目标（< 60%）才值得飞。传送门与船暂未做，跨地图请拆作业。
- 卡住后依次：记坏点 → 侧面偏移点（左右扇形，每轮扩大半径，校验地面高度、坏点、LOS）→ 重试次数用尽才 `stuck` 失败。
- 坏点是本模块自己的带 TTL 列表；同时**只读**查询 Playerbots `TravelMgr::isBadMmap`（导航网格加载失败的格子），不回写。
- `move_to {"entry":N}` 找不到已加载生物时，回退到 `creature` 表里同地图最近的刷新点坐标，长途才有起点。

巡逻是一种 Job，也可以是任务 `until` 的子步骤（「在这片区域转直到刷到目标」）。导演里巡逻和任务共用 `move_to`，不要两套移动代码。

操作者在线观看时：Selfbot 驱动样条，客户端收 `SMSG_MONSTER_MOVE`。Web 下发巡逻后应提示「不要按 WASD」。

---

## 7. Web 管理端 API

Gateway 对外 REST。worldserver 只收内网 Intent。作业异步：`202 Accepted` + `jobId`，用轮询或 WebSocket 看进度。

### 7.1 资源

鉴权：管理端登录后 JWT；所有写接口校验「该账号是否拥有该角色」。服务端再验一次，不信任前端 guid。

```
GET    /v1/me/characters
GET    /v1/characters/{guid}
POST   /v1/characters/{guid}/selfbot          { "enabled": true }
POST   /v1/characters/{guid}/jobs             { "type": "complete_quest", "questId": 33 }
GET    /v1/characters/{guid}/jobs
GET    /v1/characters/{guid}/jobs/{jobId}
POST   /v1/characters/{guid}/jobs/{jobId}/pause
POST   /v1/characters/{guid}/jobs/{jobId}/resume
DELETE /v1/characters/{guid}/jobs/{jobId}

POST   /v1/patrols                            { "name": "IF-gate", "waypoints": [...] }
GET    /v1/patrols
POST   /v1/characters/{guid}/jobs             { "type": "patrol", "patrolId": "..." }

GET    /v1/quests/{questId}/script            是否已有 HTN 脚本
WS     /v1/characters/{guid}/stream           位置、作业、步骤、事件
```

`GET /v1/characters/{guid}` 示例：

```json
{
  "guid": 1,
  "name": "Thralljr",
  "online": true,
  "selfbot": true,
  "map": 0,
  "zone": 12,
  "position": { "x": -8949.95, "y": -132.49, "z": 83.53 },
  "hp": [150, 150],
  "job": {
    "id": "8f2c...",
    "type": "complete_quest",
    "status": "running",
    "step": "until:quest_objective:33:0",
    "progress": { "done": 4, "required": 8 }
  }
}
```

创建作业：角色离线 → `409`；已有运行中作业 → `409` 或 `replace=true` 抢占。

### 7.2 内网 Intent

Gateway → `mod-mybots`（JSON 一行或 HTTP localhost）：

```json
{
  "v": 1,
  "id": "intent-...",
  "accountId": 12,
  "charGuid": 1,
  "op": "AssignJob",
  "job": { "type": "patrol", "patrolId": "IF-gate" }
}
```

模块在地图线程执行后回写 `mybots_job.status` 与 `mybots_event`。Gateway 不在请求线程里等战斗结束。

### 7.3 安全

- worldserver 与 Gateway 均在 Docker 内运行；Intent 端口只对 Docker 内网（或 host 网络本机）开放，token 与 Gateway 共享。host 网络可用 `127.0.0.1`；跨容器则绑定内网可达地址，勿映射到公网。
- 公网只开 Gateway HTTPS。
- 限流：每角色每秒意图数、每账号并发作业数 = 1（第一期）。
- 审计：谁在何时对哪个角色下了什么 Job。
- 不把 `.playerbots bot initself` 这类毁号命令接到 Web。

---

## 8. 数据存储

新建表（characters 库或独立 `acore_mybots`，启动时自动应用）：

| 表 | 用途 |
|---|---|
| `mybots_job` | 作业状态机：queued/running/paused/succeeded/failed/cancelled |
| `mybots_job_step` | 当前 HTN 栈、重试次数 |
| `mybots_quest_script` | quest_id → JSON method |
| `mybots_patrol` | 路点环定义 |
| `mybots_event` | 给 Web 的时间线（卡住、接任务、死亡、恢复） |

在线快照以内存为准，落库用于崩溃恢复和历史。不要每 tick 写库。

---

## 9. LLM（P4，模块内 DeepSeek）

输入：角色快照（位置、任务状态）+ 规则 hints（giver/turnin/objectiveEntries）+ 允许的 op/entry 白名单。  
输出：校验后的 Job `steps`（HTN），禁止裸坐标与发明 entry。  
执行：与 Web 共用同一 Intent 管道；HTTPS 调用在 `MyBotsLlm` 工作线程。  
失败：`FallbackRules=1` 时回退 `BuildCompleteQuest`；否则 Job `plan_failed`。  
卡住重规划：`ReplanOnStuck=1` 时，`move_to` 因 `stuck`/`unreachable` 失败会异步请 LLM 改后续 steps（换 NPC/等待/对话），事件 `llm_replan_*`。

---

## 10. 仓库与部署形态

```
mod-mybots/                 本仓库，AC 模块
  src/                      导演、执行器、意图服务
  conf/mybots.conf.dist
  data/sql/
  docs/architecture.md      本文
gateway/                    可选，或后续独立仓库
web/                        管理端
```

安装：

```
azerothcore-wotlk/          分支 Playerbot
  modules/mod-playerbots/   官方原样
  modules/mod-mybots/       本模块
```

运行时：**worldserver（含本模块）与管理系统 Gateway 都部署在 Docker 容器中**，通过 Docker 网络互通 Intent/API；公网只暴露 Gateway。

配置分文件：`playerbots.conf` 与 `mybots.conf`。命令前缀 `.mybots`，不占用 `.playerbots`。

---

## 11. 风险

| 风险 | 缓解 |
|---|---|
| Selfbot + 客户端移动对打 | 忽略移动包；UI 提示勿操作；失败则改旁观小号 |
| 与官方 RPG 抢目标 | 托管时 `-rpg quest,-travel` |
| 脚本任务覆盖不全 | 默认生成 method + 按失败任务补脚本 |
| 地图线程卡死 | 意图队列、异步算路、LLM/HTTP 全在 Gateway 或工作线程 |
| 官方更新改 Action 名 | 本模块做适配层，单点映射 |
| 把 SOAP 当 API | 架构上禁止；Gateway 不转发任意 GM 字符串 |

---

## 12. 实施任务列表

优先级：P0 必须可演示 → P1 Web 能指挥 → P2 任务/巡逻能用 → P3 体验与 LLM。

当前进度：**P0 已实机验证**；**P1～P2.5 Job/执行器/巡逻已实现于模块**；**P3.5 寻路增强已落地**；**P4 模块内 DeepSeek 规划已实现（默认关闭）**；管理端由外部系统对接（接口见 [docs/api.md](api.md)）。

### P0  地基（约 1～2 周）

- [x] 确认运行环境：Playerbot 核心 + 官方 `mod-playerbots`，本模块空壳能编译进 worldserver
- [x] `CMakeLists.txt`、`mybots.conf.dist`、脚本加载器，不与 Playerbots 符号/命令冲突
- [x] 建 `mybots_*` 表并随 worldserver / `ac-db-import` 自动导入（仅保留 `data/sql/db-characters/`；C++ 尚未读写）
- [x] 定位 `GET_PLAYERBOT_AI`、`ChangeStrategy`、世界线程投递接口（`DoSpecificAction` 待执行器阶段再用）
- [x] 日志通道 `mybots`，与 `playerbots` 分开

### P0.5  Selfbot 验收

- [x] `.mybots selfbot on/off` 封装官方 Selfbot
- [x] 开启时对本角色 `nc -rpg quest,-travel`，战斗策略保留
- [x] 双手离开键盘：角色能自动自卫/跟随，客户端能看见（挂官方 Selfbot；需实机确认）
- [x] 记录按 WASD 时的橡皮筋现象；评估是否需要核心小补丁（`IgnoreClientMovement` 丢弃移动包）

### P1  意图管道与观察 API

- [x] localhost Intent/HTTP JSON 服务 + token 认证（队列上限 / 429）
- [x] 世界线程消费：`EnableSelfbot` / `AssignJob` / `CancelJob` / `Pause` / `Resume`
- [x] 角色快照：在线、位置、血蓝、selfbot、当前 job
- [x] Gateway 骨架：不在本仓库实现；外部管理端直连模块 API（见 docs/api.md）
- [x] 写接口返回 `202` + `jobId`，状态从 DB/内存读

### P1.5  执行器原语

- [x] `move_to`（坐标 / creature entry）+ 卡住检测
- [x] `interact`、`gossip_select`
- [x] `accept_quest` / `turnin_quest`（指定 quest_id）
- [x] `wait`、`until`、死亡后复活策略（先用 Playerbots 已有 release/revive）
- [x] 适配层：优先 `DoSpecificAction`，失败再走 MotionMaster / Player API

### P2  任务 Job

- [x] `complete_quest`：无手写脚本时从任务模板 + queststarter/ender + 目标生物/掉落表自动展开完整 HTN；`until` 会主动靠近并进攻；无世界刷新的召唤击杀走法阵 + `use_item`（StartItem）
- [x] JSON 脚本加载与热加载（`mybots_quest_script` + payload steps）
- [x] 先打通 2～3 条新手区任务作为黄金用例（seed SQL）
- [x] 失败原因写入 `mybots_event`，Web 可展示
- [x] 作业抢占：新 Job 取消旧 Job（可配置）

### P2.5  巡逻 Job

- [x] `mybots_patrol` 路点 CRUD（可先仅 API，后做 Web 编辑）
- [x] 路点环执行、等待、循环
- [x] 战斗 suspend/resume + leash（遇怪暂停巡逻，脱战继续）
- [x] 跨区暂不做；同地图巡逻先验收

### P3  Web 管理端

本仓库只提供模块 HTTP（见 [docs/api.md](api.md)），不实现管理端 UI。下列为**模块侧已具备的能力**：

- [x] 在线状态、启停 Selfbot（`GET /v1/characters/{id}`、`POST .../selfbot`）
- [x] 下发：接任务、做任务、去坐标、开始巡逻、取消（`POST/GET/DELETE .../jobs`）
- [x] 实时：位置/步骤（轮询快照中的坐标与 `job`；WebSocket 未做）
- [x] 事件时间线与失败提示（`GET .../events`，含 `job_failed` 等）
- [ ] 角色列表（按账号列出）：模块无此接口，由管理端查 characters 库
- [ ] 权限：只能操作本账号角色（模块仅共享 Token；账号归属校验由管理端做）

### P3.5  寻路增强

- [x] 远距离：Taxi / 飞行点高层图（`MyBotsNav::TryTaxi`，先走到飞行点再 `ActivateTaxiPathTo`）
- [x] 坏点标记、侧面偏移重试（`MyBotsNav::ComputeDetour` + 带 TTL 的坏点表）
- [x] 与 Playerbots TravelMgr 只读复用目的地，不算第二套世界图除非必要
      （只读 `isBadMmap`，CMake 探测不到接口时编译期关闭；不写回 TravelMgr）
- [x] 目标不在网格内时回退到 `creature` 静态刷新点，长途才有起点
- [x] 不再每 tick 重下移动指令（`MyBots.Nav.RepathSec`），消除抽搐

### P4  LLM 规划（模块内 DeepSeek）

- [x] 与 Job 执行器相同的 step/op schema（`ensure_selfbot` / `move_to` / `interact` / `gossip_select` / `accept_quest` / `turnin_quest` / `wait` / `until` / `use_item`）
- [x] 异步：世界线程快照 → LLM 工作线程 → Intent 回投写 steps（不阻塞 `Map::Update`）
- [x] 幻觉防护：entry 白名单 + questId 校验；禁止裸坐标 `move_to`
- [x] 失败回退规则规划器（`MyBots.Llm.FallbackRules`）或 `plan_failed`；事件 `llm_plan_*`
- [x] 卡住/不可达时高层重规划（`MyBots.Llm.ReplanOnStuck`）：只改后续 HTN steps，不替代 mmap 每 tick 寻路

配置见 `MyBots.Llm.*`（默认关闭）。

### 明确不做（第一期）

- 改 `mod-playerbots` 战斗/副本策略文件
- 暴露 SOAP / `:8888` 给浏览器
- 让 LLM 选技能或每 tick 决策
- 多角色并行大规模随机机器人管理（那是官方 Random bot 的事）
- 自定义客户端注入（非目标）

---

## 13. 验收标准（第一期结束）

1. 操作者登录角色，Web 点「开启托管」，角色开始由 AI 驱动且画面可见。
2. Web 指定一条已脚本化的新手任务，角色能接、做、交；失败有原因。
3. Web 下发同地图巡逻，遇怪打架后回到路点继续。
4. 取消作业后角色停止任务行为，仅自卫。
5. 更新官方 `mod-playerbots` 不覆盖本仓库文件；本模块仍能编译（允许改适配层）。
