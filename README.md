# mod-mybots

AzerothCore 模块：在官方 `mod-playerbots` 之上托管**当前在线角色**（Selfbot），并给已有管理系统后端暴露 JSON 接口。

worldserver 与管理系统后端都跑在 Docker 里；本仓库不提供 Web 页面。P0 只做：模块能编进 worldserver、启停 Selfbot、查询角色快照。

设计说明见 [docs/architecture.md](docs/architecture.md)。

## 依赖

- [mod-playerbots/azerothcore-wotlk](https://github.com/mod-playerbots/azerothcore-wotlk) 的 `Playerbot` 分支
- 官方 [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots)，与本模块**并排放**，不要改它的源码

```text
azerothcore-wotlk/modules/
  mod-playerbots/     # 官方
  mod-mybots/         # 本仓库，目录名必须是 mod-mybots
```

`playerbots.conf` 中需要 `AiPlayerbot.Enabled = 1`。Selfbot 权限仍建议 `AiPlayerbot.SelfBotLevel = 2`（本模块会直接挂 AI，但 Playerbots 总开关必须开）。

## 配置

与 `playerbots.conf` 同目录。Docker 部署时复制为：

```text
azerothcore-wotlk/docker/vol/etc/modules/mybots.conf
```

对应关系：

```text
azerothcore-wotlk/docker/vol/etc/modules/
  playerbots.conf   # 官方 mod-playerbots
  mybots.conf       # 本模块（由 conf/mybots.conf.dist 复制并改名）
```

```bash
cp modules/mod-mybots/conf/mybots.conf.dist docker/vol/etc/modules/mybots.conf
```

至少改掉默认 token；Docker 网桥互通时建议 `Bind = "0.0.0.0"`：

```ini
MyBots.Api.Bind = "0.0.0.0"
MyBots.Api.Port = 9100
MyBots.Api.Token = "your-secret"
```

**部署前提：worldserver 与管理系统后端都跑在 Docker 里**（同机 Docker Compose / 同一 Docker 网络即可）。模块 API 只给管理容器用，不要把 9100 映射到公网。

同网桥时绑 `0.0.0.0`，由 Docker 网络隔离；若用 `network_mode: host` 或只本容器访问，再绑 `127.0.0.1`。

## JSON API（给管理系统后端）

所有写操作和角色查询都要带：

```http
Authorization: Bearer your-secret
```

或 `X-MyBots-Token: your-secret`。

请求会投递到 worldserver 线程再执行，HTTP 线程等待结果（默认 3 秒）。

### `GET /health`

无需 token。用于探活。

```json
{"ok":true,"service":"mod-mybots","version":"0.1.0"}
```

### `GET /v1/characters/{name}`

也可 `GET /v1/characters/{guid}` 或 `GET /v1/characters?name=Thralljr`。

角色必须在线。

```json
{
  "ok": true,
  "online": true,
  "guid": 1,
  "accountId": 12,
  "name": "Thralljr",
  "selfbot": false,
  "map": 0,
  "zone": 12,
  "x": -8949.95,
  "y": -132.49,
  "z": 83.53,
  "o": 0.0,
  "level": 80,
  "class": 1,
  "hp": [150, 150],
  "latencyMs": 40
}
```

### `POST /v1/characters/{name}/selfbot`

```json
{"enabled": true}
```

`enabled: false` 关闭。空 body 视为开启。

成功时 `200`，角色不在线 `409`，token 错误 `401`。

```bash
curl -s http://127.0.0.1:9100/v1/characters/Thralljr/selfbot \
  -H "Authorization: Bearer your-secret" \
  -H "Content-Type: application/json" \
  -d '{"enabled":true}'
```

## 游戏内 / SOAP 命令

控制台和 SOAP 也可调用（若你的后台已经在用 AC SOAP）：

```text
.mybots status Name
.mybots selfbot on Name
.mybots selfbot off Name
```

游戏内不带名字则操作自己。`MyBots.Selfbot.SelfOnlyInGame = 1` 时，非 GM 不能指定别人。

开启后会去掉该角色的 `rpg quest` / `travel` / `rpg` 非战斗策略，避免官方 RPG 抢控制；战斗策略保留。操作者应松开键盘，否则可能橡皮筋。

## P0 验收

1. 模块与 `mod-playerbots` 一起编进 worldserver
2. 登录角色，`POST selfbot enabled:true`，客户端能看到角色开始自行行动
3. `GET /v1/characters/Name` 里 `selfbot` 为 true
4. `enabled:false` 后停止托管

作业、任务、巡逻接口属于后续 P1/P2，表结构已建好但尚未使用。
