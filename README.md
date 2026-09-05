# 🐻 熊出没聊天室（C++ WebSocket 聊天室 + 小游戏合集）

一个用 **C++ 手写 epoll + Reactor 多线程模型**实现的 WebSocket 聊天室，附带账号系统、朋友圈、消息收藏，以及 6 款可单机 / 联机的 H5 小游戏合集。所有游戏逻辑均由 C++ 引擎实现（纯内存状态机 + 序列化同步），前端仅负责渲染与输入。

## ✨ 功能总览

### 聊天室
- **账号体系**：注册 / 登录（SQLite 持久化，SHA-256 + 随机盐存储），会话 token
- **管理员**：默认管理员 `admin / admin123`（首次登录请尽快修改密码），可封禁用户
- **个人主页**：头像、昵称、个性签名、个人动态管理
- **朋友圈**：发布动态（文字 + 图片），支持 公开 / 私密 权限，且**发布后可随时修改某条动态的权限**
- **消息收藏**：⭐ 收藏模式，收藏任意类型消息（文字/图片/文件/视频/语音），聊天记录清空后收藏仍保留
- **实时通信**：WebSocket 全双工广播、私聊、在线列表、进入/离开提示
- **多媒体**：图片、文件、视频、语音（按住说话，需 HTTPS 或 localhost 才能用麦克风）上传与下载
- **消息能力**：真正撤回（服务端删除 + 广播）、发送时间分隔、消息时间戳
- **断线重连**：自动重连、会话恢复

### 小游戏合集（+ 菜单进入，全部支持联机 + 聊天室邀请）
| 游戏 | 引擎 | 说明 |
| --- | --- | --- |
| 象棋 ♟ | `chess_engine.h` | 完整规则、将军/吃子特效、悔棋（需对方同意）、房间码 |
| 五子棋 ⚫⚪ | `gomoku.html` | 双人联机 |
| 三消（羊了个羊式）🧩 | `tile_magic_engine.h` | DFS 可解性校验、槽位堆叠 |
| 贪吃蛇 🐍 | `snake_engine.h` | 单机 / 双人联机、加速代价、互杀 |
| 坦克大战 🎖 | `tank_engine.h` | 可破坏墙体、弹夹换弹、大地图 |
| 枪战肉鸽 🔫 | `gun_engine.h` | 玩家分工、经验独立计算、升级三选一（不暂停）、波次难度递增 |

所有联机游戏统一机制：创建房间（可设置参数）→ 房间码 / 聊天室邀请 → 双方准备后开局 → 一局结束可再来一局。

## 🏗 架构

> 详细设计文档见 [**docs/ARCHITECTURE.md**](docs/ARCHITECTURE.md)：覆盖网络模型 / 线程模型 / 协议 / 数据层 / 游戏引擎 / 部署 / 压测 / 优化方向。

```
┌─────────────┐   WebSocket   ┌──────────────────────────────┐
│  浏览器前端   │ ◄──────────► │        C++ Server            │
│ www/*.html  │   /ws 协议    │  epoll + Reactor 多线程      │
└─────────────┘               │  主线程 accept → 子线程处理   │
                              │                              │
                              │  ┌─ SQLite（用户/消息/动态）  │
                              │  ├─ Redis（在线/缓存，可选）  │
                              │  └─ 游戏引擎（纯 C++ 状态机）  │
                              └──────────────────────────────┘
```

- **网络模型**：epoll 边缘触发 + `one loop per thread` Reactor，主线程 accept 连接，分发给子线程处理
- **协议**：文本协议（`命令|参数`），WebSocket 服务端握手 + 帧编解码手写实现
- **持久化**：SQLite 存储用户、消息历史、朋友圈、收藏；Redis（hiredis）可选用作在线列表与消息缓存
- **前端**：原生 HTML/CSS/JS，无任何框架与构建步骤；C++ 游戏引擎输出状态序列，前端 Canvas 渲染

## 📦 目录结构

```
.
├── server.cpp              # 主服务（C++17，epoll Reactor + WebSocket + SQLite/Redis + 全部游戏协议）
├── chess_engine.h          # 象棋引擎
├── tile_magic_engine.h     # 三消（羊了个羊式）引擎
├── snake_engine.h          # 贪吃蛇引擎
├── tank_engine.h           # 坦克大战引擎
├── gun_engine.h            # 枪战肉鸽引擎
├── www/                    # 前端静态资源
│   ├── chat.html           # 聊天室主页
│   ├── xiangqi.html        # 象棋
│   ├── gomoku.html         # 五子棋
│   ├── sanxiao.html        # 三消
│   ├── snake.html          # 贪吃蛇
│   ├── tank.html           # 坦克大战
│   └── gun.html            # 枪战肉鸽
├── tools/
│   ├── benchmark.py        # WebSocket 压测脚本
│   └── chess_engine_test.cpp
├── docs/
│   └── nginx.conf          # Nginx 反向代理示例（/ws /upload /api）
├── Makefile                # 一键编译
└── LICENSE / .gitignore
```

## 🚀 快速开始

### 依赖
- Linux（本机 / 云服务器均可）
- g++（C++17）、libsqlite3、hiredis（可选，去掉 `-lhiredis` 编译即为内存模式）
- 阿里云等服务器部署时需在安全组放行端口

### 编译

```bash
# 方式一：Makefile
make

# 方式二：手动
g++ -O2 -std=c++17 -Wno-stringop-overread -o server server.cpp -lpthread -lhiredis -lsqlite3
```

### 运行

```bash
# ./server <静态资源目录> [端口，默认 8888]
./server www          # 端口 8888
./server www 9000     # 自定义端口
```

启动后访问 `http://服务器IP:8888/chat.html`。

- 首次启动自动创建 SQLite 数据库与默认管理员 `admin / admin123`
- 上传文件默认存放到 `uploads/` 目录

### 前端更新
```bash
# 修改 www/*.html 后直接覆盖即可，无需重启 server（由 server 静态服务）
```

## 🔐 Nginx 反代 + HTTPS（推荐）

聊天室的**语音功能**依赖麦克风，而浏览器只允许 HTTPS 或 localhost 下调用 `getUserMedia`。公网访问时请配置 HTTPS：

```bash
sudo apt install nginx certbot python3-certbot-nginx
sudo cp docs/nginx.conf /etc/nginx/sites-enabled/chatroom
# 将 server_name 换成你的域名
sudo nginx -t && sudo systemctl reload nginx
sudo certbot --nginx -d 你的域名   # 申请免费 HTTPS 证书
```

参考配置见 [`docs/nginx.conf`](docs/nginx.conf)。

## 🤖 CI 自动构建

`.github/workflows/build.yml`（GitHub Actions）：每次 push / PR 自动执行
1. Ubuntu 环境安装 g++ / sqlite / hiredis
2. `make` 编译主服务
3. 编译并运行引擎单元测试（象棋 22 项用例）
4. 冒烟测试：启动 server → curl 校验全部前端页面
5. 校验仓库无运行时产物（`*.db` / `*.log` / `uploads/`）

仓库顶部显示绿色 ✔ 即为构建通过。

## 📊 压测

```bash
python3 tools/benchmark.py 200 60 1    # 200 并发，60 秒，每连接每秒 1 条消息
```

实测（4 核云服务器，200 并发 / 60s）：连接成功率 >94%，收包速率约 1.5 万条/秒。

## 🛠 协议速览（文本协议）

| 指令 | 说明 |
| --- | --- |
| `/login <token>` | 登录 |
| `/join <昵称>` | 设置昵称 |
| `POST\|内容\|隐私` | 发朋友圈动态 |
| `POSTPRIV\|id\|0/1` | 修改动态权限（公开/私密） |
| `POSTDEL\|id` | 删除动态 |
| `FAV\|msgid\|发送者\|时间\|内容` | 收藏消息 |
| `FAVS\|offset\|limit` | 查看收藏 |
| `/recall <msgid>` | 撤回消息 |
| `/to 昵称 消息` | 私聊 |
| `GUN\|...` / `TANK\|...` 等 | 各游戏联机协议 |

完整协议以源码为准。

## ⚠️ 安全说明
- 密码以 `SHA-256(随机盐 + 密码)` 存储，不保存明文
- 默认管理员 `admin / admin123` **仅在首次启动时创建，上线后务必立即修改密码**
- 本项目中未包含任何数据库文件、上传文件、日志等运行时数据（见 `.gitignore`），仓库提交的仅是源码

## 📄 License

[MIT](LICENSE)
