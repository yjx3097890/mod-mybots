# mod-mybots HTTP API（给外部管理端）

本仓库**不包含**管理端实现。你的管理系统在 Docker 内网直接调用 worldserver 上的本模块即可。

- Base URL 示例：`http://ac-worldserver:9100`
- 鉴权：除 `/health` 外，均需  
  `Authorization: Bearer <MyBots.Api.Token>`  
  或 `X-MyBots-Token: <token>`
- 写作业接口成功时返回 **202** + `jobId`；队列满 **429**；角色不在线 **409**；token 错误 **401**

Job 运行时请保持 `MyBots.Selfbot.DisableRpgQuest = 1`，避免官方 RPG 与导演抢控制。

---

## 探活

### `GET /health`

无需 token。

```json
{"ok":true,"service":"mod-mybots","version":"0.3.0"}
```

---

## 角色

路径中的 `{id}` 可为**角色名**或 **guid**（数字）。

### `GET /v1/characters/{id}`

在线角色快照。

```json
{
  "ok": true,
  "online": true,
  "guid": 521,
  "accountId": 1,
  "name": "Yjx",
  "selfbot": true,
  "map": 0,
  "zone": 12,
  "x": -8895.0,
  "y": -133.0,
  "z": 80.0,
  "o": 0.0,
  "level": 5,
  "class": 1,
  "hp": [150, 150],
  "power": [100, 100],
  "job": null,
  "latencyMs": 40
}
```

`job` 有进行中作业时为完整 job 对象，否则为 `null`。

### `GET /v1/characters/{id}/quests`（别名 `/questlog`）

**在线内存任务日志**（`PLAYER_QUEST_LOG` 槽位），不是 `characters.character_queststatus` 表。角色须在线，否则 **409** `offline`。

```json
{
  "ok": true,
  "online": true,
  "source": "live",
  "guid": 521,
  "name": "Yjx",
  "items": [
    {
      "questId": 7,
      "title": "Kobold Camp Cleanup",
      "status": 3,
      "status_label": "incomplete",
      "questLevel": 1,
      "minLevel": 1,
      "completable": false,
      "giverEntry": 197,
      "turninEntry": 197,
      "source": "live"
    }
  ]
}
```

`status` 取值与核心一致：`1=complete`、`3=incomplete`、`5=failed`。

### `GET /v1/characters/{id}/quests/available`

当前地图上、角色**本阵营可接**的任务（该 map 有 `creature_queststarter` 刷新 + `CanTakeQuest` / 种族与敌对 NPC 过滤）。按与接任务 NPC 的距离排序。

```json
{
  "ok": true,
  "online": true,
  "source": "map",
  "map": 0,
  "zone": 12,
  "note": "map_questgivers",
  "items": [
    {
      "questId": 33,
      "title": "...",
      "status": 0,
      "status_label": "none",
      "giverEntry": 197,
      "heuristic": false,
      "source": "map"
    }
  ]
}
```

### `POST /v1/characters/{id}/selfbot`

```json
{"enabled": true}
```

`enabled: false` 关闭。空 body 视为开启。

---

## 作业 Jobs

### `POST /v1/characters/{id}/jobs` → **202**

Body 必含 `type`。可选 `replace`（默认跟 `MyBots.Job.Replace`）。

#### `move_to`

```json
{"type":"move_to","x":-8895,"y":-133,"z":80,"dist":2.5}
```

或按生物 entry：

```json
{"type":"move_to","entry":197}
```

寻路说明：目标不在附近网格时，会退回到该 entry 在当前地图上最近的静态刷新点作为目的地。直线距离超过 `MyBots.Nav.TaxiMinDistance`（默认 600 码）时，先走到最近飞行点再走飞行路线，落地后继续步行；卡住会自动侧面绕行，重试次数用尽才以 `stuck` 判失败。跨地图不会自动处理，请自行拆成多个作业。

#### `complete_quest`

```json
{
  "type":"complete_quest",
  "questId":7,
  "giverEntry":197,
  "turninEntry":197
}
```

`giverEntry` / `turninEntry` 可选。不传时模块会从 `creature_queststarter` / `creature_questender`（以及 Playerbots TravelMgr 的任务目的地表）自动解析。

**不传 `steps`、库里也没有 `mybots_quest_script` 时**：

- 若 `MyBots.Llm.Enable=1` 且配置了 `MyBots.Llm.ApiKey`：异步调 DeepSeek 规划 steps。Job 先为 `status=planning`，成功后变为 `queued`/`running`；事件 `llm_plan_ok` / `llm_plan_fallback` / `llm_plan_failed`。失败且 `FallbackRules=1` 时回退规则规划器。
- 执行中若 `move_to` 因 `stuck` / `unreachable` 失败，且 `ReplanOnStuck=1`：Job 再入 `planning`，由 LLM 只改**后续高层 steps**（不发明坐标）；事件 `llm_replan_queued` / `llm_replan_ok` / `llm_replan_failed`。同一 Job 受 `ReplanMax` / `ReplanCooldownSec` 限制。
- 否则按任务模板和角色当前任务状态**规则展开**：

1. `ensure_selfbot`
2. 尚未接取：有接任务 NPC → `move_to` + `accept_quest`（**已接过则跳过**）
3. 有击杀/掉落/对话事件目标：`move_to` 目标 + `until`
4. **召唤击杀**（目标生物无 `creature` 刷新，如术士 1689 虚空行者）：走到召唤法阵 GO → `use_item`（任务 `StartItem`）→ `until`（可再使用物品）→ 交任务。不会对召唤物做 `move_to`
5. 未交：`move_to` 交任务 NPC + `turnin_quest`

若 body 里带了 `"steps":[...]`，或以 `mybots_quest_script` 手写脚本为准，则不走 LLM / 自动展开。

#### LLM 试规划（不派发 Job）

### `POST /v1/characters/{id}/plan`

Body：`{"questId":5929,"giverEntry":11802,"turninEntry":11802}`（与 complete_quest 相同字段）。

返回上下文 + 模型 steps（或 `plan_failed`）。需角色在线；耗时可能接近 `MyBots.Llm.TimeoutMs`。

任务 256（通缉 Chok'sul）这类「交物品」任务，目标生物来自 `creature_questitem` / Playerbots 已解析的掉落表；生成后的 `until.detail` 会带 `"entries":[生物entry,...]`。

#### `patrol`

```json
{"type":"patrol","patrolId":"demo-northshire"}
```

或内联路点：

```json
{
  "type":"patrol",
  "waypoints":[
    {"x":-8895,"y":-133,"z":80,"wait":2},
    {"x":-8949,"y":-132,"z":83,"wait":2}
  ]
}
```

#### `script`

```json
{
  "type":"script",
  "steps":[
    {"op":"move_to","detail":"{\"x\":1,\"y\":2,\"z\":3}"},
    {"op":"wait","detail":"{\"seconds\":3}"}
  ]
}
```

成功响应示例：

```json
{
  "ok": true,
  "code": "accepted",
  "jobId": "job-...",
  "job": { "id": "job-...", "status": "queued", "type": "move_to", "steps": [] }
}
```

### `GET /v1/characters/{id}/jobs`

该角色作业列表。

### `GET /v1/characters/{id}/jobs/{jobId}`

单作业详情（含 steps / status / error）。

每个 step 有两个字符串字段：`detail` 是创建作业时的参数，不会变；`result` 是执行器上一次的返回，用来看进度，取值如 `moving`、`in_combat`、`detour`、`taxi_approach`、`in_flight`、`arrived`。

### `POST /v1/characters/{id}/jobs/{jobId}/pause`

暂停。

### `POST /v1/characters/{id}/jobs/{jobId}/resume`

恢复。

### `DELETE /v1/characters/{id}/jobs/{jobId}`

取消指定作业。

### `DELETE /v1/characters/{id}/jobs`

取消该角色当前活动作业。

---

## 事件

### `GET /v1/characters/{id}/events`

```json
{
  "ok": true,
  "events": [
    {"id":1,"jobId":"job-...","kind":"job_failed","message":"stuck","createdAt":1710000000}
  ]
}
```

`kind` 取值：`job_created`、`job_running`、`job_succeeded`、`job_failed`、`job_cancelled`、`patrol_resume`、`patrol_loop`，以及寻路升级事件 `nav`（`message` 为 `detour_retry` / `repath` / `taxi_approach` / `taxi_boarded` / `taxi_no_money`）。走得慢时先看 `nav` 事件，能区分是在绕障碍还是在赶飞行点。

---

## 巡逻定义

### `GET /v1/patrols`

### `POST /v1/patrols`

```json
{
  "id": "demo-northshire",
  "name": "Northshire demo loop",
  "waypoints": [
    {"x":-8895,"y":-133,"z":80,"wait":2},
    {"x":-8949,"y":-132,"z":83,"wait":2}
  ],
  "accountId": 0
}
```

---

## 步骤 op 一览（脚本 / script job）

| op | detail 示例 | 说明 |
|---|---|---|
| `ensure_selfbot` | `{}` | 确保已挂 Selfbot |
| `move_to` | `{"x":..,"y":..,"z":..}` 或 `{"entry":197}` | 移动：远距离走飞行点，卡住则绕行重试 |
| `interact` | `{"entry":197}` | 与 NPC 交互 |
| `gossip_select` | `{"entry":197,"menu":0,"option":0}` | 选择 gossip |
| `accept_quest` | `{"questId":7,"entry":197}` | 接任务 |
| `turnin_quest` | `{"questId":7,"entry":197}` | 交任务 |
| `wait` | `{"seconds":5}` | 等待 |
| `until` | `{"questId":7,"entries":[6]}` | 主动靠近/攻击目标直至任务完成；`entries` 可由系统自动填 |
| `use_item` | `{"itemId":6928}` | 使用背包物品（召唤任务的 StartItem 等）；引导中会保持 running |
| `revive` | `{}` | 复活 |

---

## curl 示例

```bash
TOKEN='your-secret'
HOST='http://ac-worldserver:9100'

curl -s "$HOST/health"

curl -s "$HOST/v1/characters/Yjx" \
  -H "Authorization: Bearer $TOKEN"

curl -s "$HOST/v1/characters/Yjx/selfbot" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"enabled":true}'

curl -s "$HOST/v1/characters/Yjx/jobs" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"type":"move_to","x":-8895,"y":-133,"z":80}'
```
