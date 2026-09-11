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
{"ok":true,"service":"mod-mybots","version":"0.2.0"}
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

#### `complete_quest`

```json
{
  "type":"complete_quest",
  "questId":7,
  "giverEntry":197,
  "turninEntry":197
}
```

若 `mybots_quest_script` 有该 `quest_id`，优先用脚本 steps；也可在 body 里带 `"steps":[...]`。

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
| `move_to` | `{"x":..,"y":..,"z":..}` 或 `{"entry":197}` | 移动 + 卡住检测 |
| `interact` | `{"entry":197}` | 与 NPC 交互 |
| `gossip_select` | `{"entry":197,"menu":0,"option":0}` | 选择 gossip |
| `accept_quest` | `{"questId":7,"entry":197}` | 接任务 |
| `turnin_quest` | `{"questId":7,"entry":197}` | 交任务 |
| `wait` | `{"seconds":5}` | 等待 |
| `until` | `{"questId":7}` | 等到任务目标完成 |
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
