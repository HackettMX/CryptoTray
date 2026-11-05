#pragma once
#include <string>

// 极简多语言支持：按 key 查表，表里三种语言并排，缺译自动回退到简体中文。
// 用法：TR(L"menu.settings")、swprintf(buf, n, TR(L"hint.rate_over"), per2s)
namespace i18n {

// 支持的语言（顺序即界面上的按钮顺序）
int            count();
const wchar_t* id(int idx);            // "zh-CN" / "zh-TW" / "en-US"（写进配置文件）
const wchar_t* native_name(int idx);   // "简体" / "繁體" / "EN"（界面上显示，本身不翻译）
int            from_id(const std::string& s);   // 找不到返回 -1
int            detect_system();        // 按系统 UI 语言猜一个

void           set(int idx);
int            current();

// 取文案；key 不存在时返回 key 本身（便于一眼看出漏翻）
const wchar_t* t(const wchar_t* key);

// 行情错误键 → 本地化文案；不认识的键按 UTF-8 原样返回（保留平台原始报错）
std::wstring   err_text(const std::string& key);

// 启动自检：返回三语表里缺译的条目数（写 log 用）
int            audit_missing();

}  // namespace i18n

#define TR(k) ::i18n::t(k)
