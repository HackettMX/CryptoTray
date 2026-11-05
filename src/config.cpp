#define _CRT_SECURE_NO_WARNINGS
#include "config.hpp"
#include "util.hpp"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <cstdlib>

// 配置存放于当前用户 AppData，避免对安装目录写权限的要求
static std::string config_path() {
    const char* app = getenv("APPDATA");
    std::string p = app ? std::string(app) : ".";
    p += "\\CryptoTray";
    CreateDirectoryW(to_w(p).c_str(), NULL);
    p += "\\config.ini";
    return p;
}

static std::string trim(std::string s) {
    // 历史版本可能把 NUL 写进配置，先清掉
    for (size_t i = s.find('\0'); i != std::string::npos; i = s.find('\0')) s.erase(i, 1);
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    size_t e = s.find_last_not_of(" \t\r\n");
    if (e == std::string::npos) return "";
    return s.substr(0, e + 1);
}

bool config_load(Config& c, std::string* err) {
    std::ifstream f(config_path());
    if (!f) { config_save(c, nullptr); return true; } // 首次运行：写入默认配置
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (k == "platform") { if (!v.empty()) c.platform = v; }
        else if (k == "lang") { if (!v.empty()) c.lang = v; }
        else if (k == "bg_interval") c.bgInterval = atoi(v.c_str());
        else if (k == "win_interval") c.winInterval = atoi(v.c_str());
        else if (k == "coins") {
            c.coins.clear();
            std::stringstream ss(v); std::string item;
            while (std::getline(ss, item, ',')) {
                item = trim(item);
                if (!item.empty()) c.coins.push_back(item);
            }
        }
        else if (k == "no_title_bar") c.noTitleBar = (atoi(v.c_str()) != 0);
        else if (k == "bg_alpha") c.bgAlpha = atoi(v.c_str());
        else if (k == "pin_to_desktop") c.pinToDesktop = (atoi(v.c_str()) != 0);
        else if (k == "always_on_top") c.alwaysOnTop = (atoi(v.c_str()) != 0);
        else if (k == "mouse_through") c.mouseThrough = (atoi(v.c_str()) != 0);
        else if (k == "dock_taskbar") c.dockTaskbar = (atoi(v.c_str()) != 0);
        else if (k == "dock_offset_x") c.dockOffsetX = atoi(v.c_str());
        else if (k == "popup_width") c.popupWidth = atoi(v.c_str());
        else if (k == "window_x") c.windowX = atoi(v.c_str());
        else if (k == "window_y") c.windowY = atoi(v.c_str());
        else if (k == "kline_bar") {
            if (!v.empty()) c.klineBar = v;
        }
    }
    if (c.coins.empty()) c.coins = { "BTC-USDT", "ETH-USDT", "SOL-USDT" };
    if (c.bgInterval < 5) c.bgInterval = 5;
    if (c.winInterval < 1) c.winInterval = 1;
    if (c.bgAlpha < 0) c.bgAlpha = 0;
    if (c.bgAlpha > 255) c.bgAlpha = 255;
    if (c.popupWidth < 260) c.popupWidth = 260;
    if (c.popupWidth > 460) c.popupWidth = 460;
    return true;
}

bool config_save(const Config& c, std::string* err) {
    // 先写临时文件再原子替换：全量覆盖写如果中途被打断（崩溃 / 另一个实例同时写），
    // 会留下半截配置，下次启动币种就被截断了
    std::string path = config_path();
    std::string tmp = path + ".tmp";
    std::ofstream f(tmp);
    if (!f) { if (err) *err = "cannot write config"; return false; }
    f << "# CryptoTray 配置文件（修改后保存即生效）\n";
    f << "# bg_interval : 后台刷新间隔(秒, 建议>=5)\n";
    f << "# win_interval: 窗口显示时的刷新间隔(秒)\n";
    f << "# coins       : 关注币种, 逗号分隔, 格式为 instId (如 BTC-USDT)\n";
    f << "# no_title_bar: 无标题栏 (1/0)\n";
    f << "# bg_alpha    : 整体透明度 0-255 (230≈90%, 越小越透)\n";
    f << "# pin_to_desktop: 固定到桌面 1/0 (始终显示、最前、不自动隐藏)\n";
    f << "# always_on_top : 置于最顶层 1/0 (压在其它窗口之上)\n";
    f << "# mouse_through : 鼠标穿透 1/0 (点击直接落到下层窗口)\n";
    f << "# dock_taskbar  : 停靠任务栏 1/0 (变成嵌在任务栏内部的迷你横条)\n";
    f << "# dock_offset_x : 横条在任务栏内的水平偏移像素 (-1 = 默认位置)\n";
    f << "# kline_bar   : 走势图 K 线周期 (5m/15m/1H/4H/1D/1W/1M/3M)\n";
    f << "# popup_width : 弹窗宽度 260-460 (默认 300，越小越紧凑)\n";
    f << "# platform    : 数据源 okx/binance/bybit/gate/mexc/kucoin/htx/bitget\n";
    f << "# lang        : 界面语言 zh-CN / zh-TW / en-US (留空 = 按系统语言自动选)\n";
    f << "platform=" << c.platform << "\n";
    f << "lang=" << c.lang << "\n";
    f << "bg_interval=" << c.bgInterval << "\n";
    f << "win_interval=" << c.winInterval << "\n";
    f << "coins=";
    for (size_t i = 0; i < c.coins.size(); i++) { if (i) f << ","; f << c.coins[i]; }
    f << "\n";
    f << "no_title_bar=" << (c.noTitleBar ? 1 : 0) << "\n";
    f << "bg_alpha=" << c.bgAlpha << "\n";
    f << "pin_to_desktop=" << (c.pinToDesktop ? 1 : 0) << "\n";
    f << "always_on_top=" << (c.alwaysOnTop ? 1 : 0) << "\n";
    f << "mouse_through=" << (c.mouseThrough ? 1 : 0) << "\n";
    f << "dock_taskbar=" << (c.dockTaskbar ? 1 : 0) << "\n";
    f << "dock_offset_x=" << c.dockOffsetX << "\n";
    f << "window_x=" << c.windowX << "\n";
    f << "window_y=" << c.windowY << "\n";
    f << "kline_bar=" << c.klineBar << "\n";
    f << "popup_width=" << c.popupWidth << "\n";
    f.close();
    if (!f) { if (err) *err = "cannot write config"; return false; }
    if (!MoveFileExW(to_w(tmp).c_str(), to_w(path).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(to_w(tmp).c_str());
        if (err) *err = "cannot replace config";
        return false;
    }
    return true;
}
