# CryptoTray · 任务栏加密货币行情

> 原生 Win32 / C++ 编写的 Windows 任务栏行情小工具：可嵌入任务栏变成一条迷你横条，或用悬浮窗看走势线。单个 exe，免安装、免 API Key。

一个轻量到几乎「隐形」的桌面行情挂件。启动后常驻托盘，右键即可添加关注币种；勾选「嵌入任务栏」后，行情会以一条紧凑的迷你横条贴在任务栏空白处，鼠标点一下又能展开带走势图的浮窗。所有数据来自各交易所的**公开行情接口**，不需要注册、不需要填密钥。

---

## 功能特性

- **两种形态**
  - **嵌入任务栏**：迷你横条，一个币种一格（代号 + 涨跌幅 / 价格），可直接拖到任务栏任意空位，位置自动记忆。
  - **悬浮窗 + 走势图**：每个币种一行，右侧 30 根 K 线 sparkline，趋势一眼可见。
- **8 家公开行情源，随时切换**：OKX / Binance / Bybit / Gate / MEXC / KuCoin / HTX / Bitget。切换后立即按新源取数。
- **多币种关注列表**：添加 / 删除即时生效，改动即刻落盘，无需「保存」。
- **涨跌幅跟随走势图周期**：显示的是「当前所选周期窗口内的净涨跌」，切换周期（5 分钟 ~ 3 月）后百分比随之变化，而不是固定的 24h。
- **秒级刷新**：后台 / 前台两档刷新间隔可分别设置（最短 1 秒）。
- **不卡顿、不排队**：每个请求 1 秒硬超时；上一轮没完成时直接忽略新请求，绝不排队堆积，刷新节奏稳定。
- **客户端限频预算**：按平台公共接口的频控额度预分配请求数，避免触发限频。
- **配置原子写入**：先写临时文件再原子替换，断电/崩溃也不会写坏配置。
- **外观可调**：整体不透明度、置于最顶层、鼠标穿透、固定到桌面。
- **支持永续合约**：如 OKX 的 `SNDK-USDT-SWAP`（填 `BASE-QUOTE-SWAP` 即可）。

---

## 数据源

| 平台 | 平台 ID | 行情接口 |
|------|---------|----------|
| OKX | `okx` | `GET /api/v5/market/ticker` |
| Binance | `binance` | `GET /api/v3/ticker/24hr` |
| Bybit | `bybit` | `GET /v5/market/tickers` |
| Gate | `gate` | `GET /api/v4/spot/tickers` |
| MEXC | `mexc` | `GET /api/v3/ticker/24hr` |
| KuCoin | `kucoin` | `GET /api/v1/market/stats` |
| HTX (火币) | `htx` | `GET /market/detail/merged` |
| Bitget | `bitget` | `GET /api/v2/spot/market/tickers` |

> 均为公开接口，无需 API Key；按 IP 有频率限制，程序已内置客户端限频。

---

## 快速开始

### 环境要求

- Windows 10 / 11（x64）
- Visual Studio（含「使用 C++ 的桌面开发」工作负载，提供 MSVC 编译器）
- CMake ≥ 3.20
- Ninja

### 构建

1. 打开 `build.bat`，把顶部的 `VCTOOLS` / `SDKDIR` / `SDKVER` 改成你本机对应的路径（`build.bat` 为免 `vcvars` 手动设置环境变量，方便在受限环境编译）。
2. 运行：

```bat
build.bat
```

或手动执行：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build build
```

产物：`build/CryptoTray.exe`（静态链接 MSVC 运行库，**单文件**，拷走即用）。

### 附带测试程序

| 程序 | 用途 |
|------|------|
| `test_okx.exe` | 冒烟测试：`test_okx [platform] [instId] [bar]`，`test_okx all` 遍历全部平台 |
| `test_rate.exe` | 验证客户端限频在突发请求下是否生效 |
| `test_timeout.exe` | 验证慢接口会被 1 秒截止时间掐断 |

```bash
./build/test_okx.exe okx BTC-USDT 1h
./build/test_okx.exe okx SNDK-USDT-SWAP 1m
```

---

## 使用

1. 运行 `CryptoTray.exe`，托盘出现图标。
2. **右键托盘 → 设置**：
   - 在「关注币种」输入框填符号（如 `BTC-USDT`；永续合约填 `BTC-USDT-SWAP`），点「添加」。
   - 选择数据源、走势图周期、刷新频率。
   - 勾选「嵌入任务栏（迷你横条）」即可变成任务栏横条。
3. **双击托盘图标**显示 / 隐藏浮窗。
4. 配置自动保存在 `%APPDATA%\CryptoTray\config.ini`。

### 币种写法

- 现货：`BASE-QUOTE`，例如 `BTC-USDT`、`ETH-USDT`。
- 永续合约：`BASE-QUOTE-SWAP`，例如 `SNDK-USDT-SWAP`（OKX）。
- 也兼容 `BTCUSDT`、`btc_usdt` 等写法，程序会自动归一化。

---

## 项目结构

```
.
├── src/
│   ├── main.cpp       # 主程序：托盘、悬浮窗、任务栏横条、设置界面
│   ├── market.cpp     # 多平台行情抽象：符号归一化、ticker / K 线请求与解析
│   ├── http.cpp       # WinHTTP 封装：超时、长连接复用、客户端限频
│   ├── config.cpp     # 配置读写（原子写入）
│   ├── json.hpp       # 极简 JSON 解析器
│   ├── util.hpp       # 通用工具（编码转换、锁等）
│   ├── market.hpp / http.hpp / config.hpp
├── test_okx.cpp       # 行情冒烟测试
├── test_rate.cpp      # 限频测试
├── test_timeout.cpp   # 超时测试
├── CMakeLists.txt
└── build.bat
```

---

## 技术要点

- **纯 Win32 + GDI 绘制**，无第三方 UI 框架，启动快、体积小。
- **分层窗口**（Layered Window）实现圆角、半透明、逐像素透明；任务栏横条用 `UpdateLayeredWindow` 合成，背景真正透出任务栏。
- **WinHTTP** 长连接复用；`WinHttpSetTimeouts` + 总 deadline 双保险，1 秒内必须返回。
- **单线程串行拉取 + 固定节拍**：刷新节奏由拉取线程自己掌握，不被消息队列延迟影响。
- **数据代数（generation）**：切换平台 / 币种 / 周期时作废旧轮请求，新配置立刻接管，不等旧请求。

---

## 已知限制

- 仅支持 Windows。
- 公共接口按 IP 限频：币种多 + 1 秒刷新会撞到频控，此时数据约 1~2 秒一轮属正常现象。
- 不同平台对同一币种的现货 / 合约支持不同；若该平台没有此交易对，会提示「该平台无此交易对」。
- 深色任务栏下若文字发暗，可在系统设置中调整，或反馈。

---

## 免责声明

本项目仅供学习与个人使用。行情数据来自第三方交易所的公开接口，其延迟、准确性与可用性以各平台为准。**本项目不构成任何投资建议**。

---

## License

[MIT](LICENSE)
