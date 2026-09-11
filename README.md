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

## 安装后怎么用

按顺序做完下面步骤，才能在游戏里或通过 API 托管角色。

**先分清：重编 vs 重启**

| 你改了什么 | 要做什么 |
|---|---|
| 第一次放入 `mod-mybots`，或改了 `.cpp` / `.h` / `CMakeLists.txt` | **必须重新编译**，再启动 worldserver |
| 只改 `mybots.conf` / `playerbots.conf` | **只需重启** worldserver，不用重编 |

模块是静态编进 `worldserver` 的，不是热加载；只重启装不上新模块。

### 1. 放入模块并重新编译

在 `azerothcore-wotlk` 根目录：

```bash
cd modules
git clone https://github.com/yjx3097890/mod-mybots.git mod-mybots
# 或已有目录：cd mod-mybots && git pull
cd ..
```

目录必须是 `modules/mod-mybots`，与 `mod-playerbots` 并列：

```text
azerothcore-wotlk/modules/
  mod-playerbots/
  mod-mybots/
```

#### Docker（推荐，与你当前部署一致）

在 `azerothcore-wotlk` 根目录**重建并启动**（首次装模块或改了源码都走这一步）：

```bash
docker compose up -d --build
```

若你平时只重建 world 服务：

```bash
docker compose build ac-worldserver
docker compose up -d ac-worldserver
```

编译很慢是正常的（往往十几分钟到几十分钟）。`--build` 才会把 `mod-mybots` 编进镜像；只 `docker compose restart` **不够**。

#### 数据库表（自动创建）

本模块把表建在 **`acore_characters`**（不是独立库）。SQL 路径与官方模块一致：

```text
modules/mod-mybots/data/sql/db-characters/base/2026_09_11_00_mybots.sql
```

会建：`mybots_job`、`mybots_job_step`、`mybots_quest_script`、`mybots_patrol`、`mybots_event`。

| 部署 | 谁自动执行 |
|---|---|
| Docker | **`ac-db-import`**（worldserver 容器通常关掉了主库 Updates） |
| 源码 | worldserver 启动时的 DB Updater |

要让 Docker 自动进模块 SQL，确认 `dbimport.conf`（或生成物）里：

```ini
Updates.EnableDatabases = 7
Updates.AllowedModules = "all"
```

（`"all"` 也可写成包含 `mod-mybots`。若 `AllowedModules` 为空，模块 SQL **不会**跑。）

装上本模块后需要 **重建并跑一次** `ac-db-import`（`docker compose up -d --build` 即可）。日志里应出现 Applying `2026_09_11_00_mybots.sql`；库中可用：

```sql
SHOW TABLES LIKE 'mybots_%';
```

说明：Playerbots 的 `acore_playerbots` 是它自己的 `Playerbots.Updates` 管的；本模块走 AzerothCore **标准模块 SQL**，机制不同，但效果一样——启动/dbimport 后自动建表。

P0 启停 Selfbot **还不读写**这些表；缺表也能先测命令/API。后续 Job/巡逻会用到。

若自动导入失败，可临时手动：

```bash
docker exec -i ac-database mysql -uacore -p acore_characters \
  < modules/mod-mybots/data/sql/db-characters/base/2026_09_11_00_mybots.sql
```

#### 源码编译（非 Docker）

在已有 Playerbot 核心的 `build` 目录：

```bash
cd build
# 新增模块后建议重新 cmake，再编译安装
cmake .. -DCMAKE_INSTALL_PREFIX=../env/dist -DSCRIPTS=static -DMODULES=static
make -j$(nproc)
make install
```

macOS 可把 `$(nproc)` 换成 `$(sysctl -n hw.ncpu)`。也可用 AzerothCore 脚本：

```bash
./acore.sh compiler build
```

装好后启动 `authserver` / `worldserver`（或你现有的启动方式）。

编译成功的标志：worldserver 启动日志出现 `module.mybots` 相关输出；游戏内输入 `.mybots status` 有响应（而不是未知命令）。

### 2. 配置 Playerbots（必做）

编辑与 `mybots.conf` 同目录的 `playerbots.conf`：

```text
azerothcore-wotlk/docker/vol/etc/modules/playerbots.conf
```

至少保证：

```ini
AiPlayerbot.Enabled = 1
AiPlayerbot.SelfBotLevel = 2
```

`Enabled = 0` 时本模块无法挂 AI。`SelfBotLevel = 2` 允许普通玩家也能用官方 Selfbot；本模块 API/命令会直接挂 AI，但仍依赖总开关打开。

### 3. 配置本模块

```bash
cp modules/mod-mybots/conf/mybots.conf.dist docker/vol/etc/modules/mybots.conf
```

得到：

```text
azerothcore-wotlk/docker/vol/etc/modules/
  playerbots.conf
  mybots.conf
```

打开 `mybots.conf`，至少改这些：

```ini
MyBots.Enable = 1
MyBots.Selfbot.Allow = 1

# Docker 网桥互通用 0.0.0.0；host 网络或本容器自测可用 127.0.0.1
MyBots.Api.Enable = 1
MyBots.Api.Bind = "0.0.0.0"
MyBots.Api.Port = 9100
MyBots.Api.Token = "换成你自己的长随机串"
```

- Token 不能留空，也不能继续用默认的 `change-me`（否则 API 会拒绝启动/请求）。
- **不要把 9100 映射到公网**；只给同 Docker 网络里的管理后端用。
- **只改了 conf：重启 worldserver 即可**（Docker：`docker compose restart ac-worldserver`）。不要为此再 `--build`。
- 若改完 conf 仍像没生效，确认改的是 `docker/vol/etc/modules/mybots.conf`（运行时配置），不是仓库里的 `conf/mybots.conf.dist`。

启动日志里应能看到本模块相关输出（通道 `module.mybots`）。`mybots_*` 表由 `ac-db-import` / worldserver Updater 按上一节自动创建。

### 4. 登录角色并开启托管

角色必须**在线、已进入世界**。两种方式任选其一。

**方式 A：游戏内命令（不连管理端也能用）**

聊天框输入：

```text
.mybots selfbot on
.mybots status
.mybots selfbot off
```

不带名字则操作自己。带名字可指定在线角色（`MyBots.Selfbot.SelfOnlyInGame = 1` 时，非 GM 只能操作自己）。

控制台 / SOAP 用法相同，但必须带角色名，例如：

```text
.mybots selfbot on Thralljr
```

**方式 B：HTTP API（给管理后端）**

角色在线后：

```bash
# 探活（无需 token）
curl -s http://<worldserver容器或主机>:9100/health

# 开启托管
curl -s http://<worldserver容器或主机>:9100/v1/characters/Thralljr/selfbot \
  -H "Authorization: Bearer 你的Token" \
  -H "Content-Type: application/json" \
  -d '{"enabled":true}'

# 查快照
curl -s http://<worldserver容器或主机>:9100/v1/characters/Thralljr \
  -H "Authorization: Bearer 你的Token"

# 关闭托管
curl -s http://<worldserver容器或主机>:9100/v1/characters/Thralljr/selfbot \
  -H "Authorization: Bearer 你的Token" \
  -H "Content-Type: application/json" \
  -d '{"enabled":false}'
```

也可用请求头 `X-MyBots-Token: 你的Token`。

### 5. 开启后你会看到什么

- 客户端里角色开始由 Playerbots AI 驱动；**请松开键盘**，否则可能橡皮筋。
- 默认 `MyBots.Selfbot.DisableRpgQuest = 1`：去掉自主任务/旅行策略，**不会**自动接任务跑图；战斗策略保留，可自卫。
- Selfbot 本身也不像随机机器人那样默认会自己玩。若设 `DisableRpgQuest = 0`，本模块会主动挂上 `+new rpg,+grind,-follow`，才会去磨怪/做任务。改配置后需重启，并重新 `.mybots selfbot off` 再 `on`。
- 本仓库的任务 Job / 巡逻 / 管理 Web 尚未实现；P0 只验证「能挂/能摘 Selfbot」。

### 快速验收

1. 模块与 `mod-playerbots` 一起编进 worldserver 并成功启动  
2. 登录角色，`.mybots selfbot on` 或 `POST .../selfbot`，角色开始自行行动  
3. `.mybots status` 或 `GET /v1/characters/Name` 里 `selfbot` 为 true  
4. `off` / `enabled:false` 后停止托管  

## Job API（P1+）

作业异步：`POST .../jobs` 返回 **202** + `jobId`，用 GET 轮询状态。意图队列满返回 **429**。

```bash
# 去坐标
curl -s http://127.0.0.1:9100/v1/characters/Yjx/jobs \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"type":"move_to","x":-8895,"y":-133,"z":80}'

# 做任务
curl -s http://127.0.0.1:9100/v1/characters/Yjx/jobs \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"type":"complete_quest","questId":7,"giverEntry":197}'

# 巡逻（需先有 patrol 定义，seed 含 demo-northshire）
curl -s http://127.0.0.1:9100/v1/characters/Yjx/jobs \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"type":"patrol","patrolId":"demo-northshire"}'
```

其它：`GET/DELETE .../jobs/{jobId}`、`POST .../pause|resume`、`GET .../events`、`GET/POST /v1/patrols`。

管理端请对接本模块 HTTP，接口说明见 [docs/api.md](docs/api.md)。本仓库不包含管理端实现。

Job 运行时请保持 `MyBots.Selfbot.DisableRpgQuest = 1`。`IgnoreClientMovement = 1` 可减轻闪现/橡皮筋。

## JSON API 参考

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

## 配置项摘要

详见 `conf/mybots.conf.dist`（中英备注）。常用项：

| 配置 | 作用 |
|---|---|
| `MyBots.Enable` | 总开关 |
| `MyBots.Selfbot.Allow` | 是否允许挂 Selfbot |
| `MyBots.Selfbot.SelfOnlyInGame` | 游戏内非 GM 只能操作自己 |
| `MyBots.Selfbot.DisableRpgQuest` | 开启时去掉自动任务/旅行策略 |
| `MyBots.Api.*` | HTTP 绑定、端口、Token、超时 |

作业、任务、巡逻接口属于后续 P1/P2，表结构已建好但尚未使用。
