# Linux System Monitor v3.0

基于 **Linux C 语言、TCP Socket、pthread、Vue 3 和 ECharts** 开发的轻量级 Linux 系统资源监控与告警平台。

项目采用 C 语言实现系统资源采集、网络通信、并发处理和阈值告警，并通过 HTTP API 向 Vue 3 前端提供监控数据，实现 Linux 系统运行状态的实时可视化。

## 1. 项目简介

Linux System Monitor 运行于 Linux 环境，主要用于监控 CPU、内存、磁盘及进程运行状态。

系统通过读取 Linux `/proc` 文件系统及调用 `statvfs` 等系统接口获取资源信息，使用 TCP Socket 完成监控数据传输，使用 pthread 实现多客户端并发处理，并结合 Vue 3 和 ECharts 展示实时监控数据。

项目在 Ubuntu 24.04（WSL2）环境下开发和测试。

## 2. 技术栈

| 分类 | 技术 |
|---|---|
| 开发语言 | C、JavaScript |
| 操作系统 | Ubuntu 24.04 / WSL2 |
| 编译工具 | GCC、Make |
| 系统编程 | `/proc`、`statvfs` |
| 网络通信 | TCP Socket、HTTP |
| 并发编程 | pthread、mutex |
| 数据格式 | JSON、cJSON |
| 前端框架 | Vue 3、Vite |
| 数据可视化 | ECharts |
| 版本管理 | Git、GitHub |

## 3. 系统主要功能

### 3.1 Linux 系统资源采集

使用 C 语言获取 Linux 主机运行状态，包括：

- CPU 使用率
- 内存使用率
- 磁盘使用率
- 运行进程数量

### 3.2 TCP Socket 网络通信

客户端采集系统监控数据，并以 JSON 格式通过 TCP Socket 发送至服务端。

服务端接收数据后进行解析、校验和处理。

### 3.3 多客户端并发处理

服务端基于 pthread 实现多客户端并发通信，最多支持 32 个客户端连接，并对线程资源进行管理和回收。

### 3.4 JSON 数据校验

服务端使用 cJSON 解析监控数据，对必要字段、数据类型和数值范围进行校验，避免无效数据影响监控处理。

### 3.5 阈值告警与恢复检测

支持为 CPU、内存和磁盘设置独立告警阈值。

当监控指标达到或超过阈值时，服务端记录告警事件；当指标恢复正常时，记录恢复事件，并避免持续异常状态下重复触发相同告警。

### 3.6 日志持久化

服务端将运行日志写入 `logs/server.log`，包括客户端连接、异常数据、告警和恢复等信息。

多线程写入日志时通过互斥锁保护共享资源。

### 3.7 外部配置管理

通过 `config/monitor.conf` 设置告警阈值：

```ini
CPU_THRESHOLD=80
MEMORY_THRESHOLD=85
DISK_THRESHOLD=90
```

修改配置文件并重启服务端后即可应用新阈值，无需重新编译程序。

### 3.8 HTTP API

在 TCP 监控服务之外，C 服务端提供 HTTP 接口：

```text
GET http://127.0.0.1:8080/api/monitor
```

前端通过该接口获取服务端当前监控数据，并展示系统资源状态。

### 3.9 Vue 3 可视化监控界面

使用 Vue 3 构建 Web 监控界面，并通过 ECharts 展示 CPU、内存和磁盘的使用率变化趋势。

主要展示内容包括：

- CPU、内存和磁盘实时使用率
- Linux 运行进程数量
- 系统资源变化趋势图
- 当前系统运行状态
- 告警与恢复事件展示
- 服务端连接状态

前端定期请求 HTTP API，实现监控数据自动刷新。

**说明：** 当前趋势数据和前端事件记录主要保存在浏览器运行会话中，服务端运行日志单独持久化保存。

## 4. 系统架构

```text
Linux /proc + 系统接口
          |
          v
     C 资源采集模块
          |
          v
       TCP Client
          |
          | JSON / TCP Socket
          v
       C Server
          |
          +---- pthread 多客户端处理
          +---- cJSON 数据校验
          +---- 阈值告警与恢复检测
          +---- 日志持久化
          +---- HTTP API :8080
                     |
                     v
                 Vue 3 前端
                     |
                     v
              ECharts 实时可视化

```

## 5. 项目目录

```text
linux-monitor-v3/
├── main.c
├── cpu.c
├── cpu.h
├── memory.c
├── memory.h
├── disk.c
├── disk.h
├── process.c
├── process.h
├── client.c
├── server.c
├── Makefile
├── config/
│   └── monitor.conf
├── frontend/
│   ├── package.json
│   ├── index.html
│   └── src/
│       ├── App.vue
│       ├── main.js
│       └── style.css
├── README.md
└── TEST_REPORT.md
```

## 6. 安装与运行

### 6.1 安装依赖

```bash
sudo apt update
sudo apt install build-essential libcjson-dev
```

前端还需要安装 Node.js 和 npm。

### 6.2 编译 C 程序

在项目根目录执行：

```bash
make
```

### 6.3 启动服务端

```bash
./server
```

### 6.4 启动客户端

打开第二个终端：

```bash
./client
```

### 6.5 启动 Vue 前端

打开第三个终端：

```bash
cd frontend
npm install
npm run dev
```

浏览器访问：

http://localhost:5173/

### 6.6 验证 HTTP API

```bash
curl http://127.0.0.1:8080/api/monitor
```

## 7. 项目测试

已完成以下功能测试：

- TCP 客户端与服务端通信
- 多客户端并发连接及线程回收
- JSON 数据校验
- CPU、内存和磁盘告警及恢复
- 日志持久化
- 配置文件校验
- HTTP API 数据返回
- Vue 前端与 C 服务端联调
- ECharts 实时数据显示

详细的 C 服务端测试内容见 `TEST_REPORT.md`。

## 8. 项目技术亮点

**Linux 系统编程：** 基于 `/proc` 文件系统和 Linux 系统接口实现资源采集。

**网络与并发编程：** 使用 TCP Socket 和 pthread 实现多客户端通信及并发处理。

**异常检测：** 结合 JSON 数据校验、阈值状态管理及日志持久化，提高系统运行可靠性。

**前后端集成：** C 服务端提供 HTTP API，Vue 3 前端获取实时数据，并通过 ECharts 展示系统资源趋势。

**工程化实践：** 使用 Make 管理项目编译，通过 Git 和 GitHub 进行版本控制。

## 9. 项目版本

当前版本：**v3.0**

- v2.7：完成 Linux 资源监控、TCP 通信、多线程、告警、日志和配置管理。
- v3.0：增加 HTTP API 和 Vue 3 + ECharts 可视化界面。

## 10. 项目说明

本项目为 Linux C 系统编程与前后端集成实践项目，主要用于学习和展示 Linux 系统接口、网络编程、多线程、数据处理及 Web 可视化技术。

目前主要面向本地开发与测试环境，不作为生产级监控平台使用。
