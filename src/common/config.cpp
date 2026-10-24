// 轻舟 RPC (Qingzhou RPC)
// src/common/config.cpp
//
// 极简 INI 读取器。要点（对应 docs/00-file-map.md C-06）：
//   * 逐行状态机：[section] / key=value / 整行注释（# 或 ;）/ 行内 # 注释 / 可选引号；
//   * section 与 key 一律转小写（大小写不敏感），value 保留原始大小写并 trim 首尾空白；
//   * 引号内的内容**原样保留**（去掉引号后不再 trim），用于 `Path = "C:\logs\a.log"` 这类值；
//   * 未加引号的值支持 `\#` 转义 —— 否则 Windows 路径里的反斜杠会被误当转义符；
//   * 所有错误信息都带 `line N`，因为配置文件报错时"第几行"是唯一有用的线索。
//
// 不支持：嵌套段、数组、include。这是刻意的 —— 配置解析器越"聪明"，行为越难预测。

#include "common/config.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/string_util.h"

namespace qzrpc {
namespace {

constexpr char kTrimChars[] = " \t\r\n";

std::string MakeLineError(std::size_t line_no, const std::string& message) {
  return "line " + std::to_string(line_no) + ": " + message;
}

// 解析 '=' 右侧的值。成功返回 true 并写入 out。
bool ParseValue(const std::string& text, std::size_t line_no, std::string* out, std::string* err) {
  const std::size_t n = text.size();
  std::size_t i = 0;
  while (i < n && (text[i] == ' ' || text[i] == '\t')) {
    ++i;  // 跳过前导空白
  }
  if (i < n && text[i] == '"') {
    const std::size_t closing = text.find('"', i + 1);
    if (closing == std::string::npos) {
      *err = MakeLineError(line_no, "unterminated quoted value, expected closing '\"'");
      return false;
    }
    // 引号内不做任何转义与 trim：Windows 路径里的反斜杠必须原样保留。
    *out = text.substr(i + 1, closing - i - 1);
    std::size_t tail = closing + 1;
    while (tail < n && (text[tail] == ' ' || text[tail] == '\t')) {
      ++tail;
    }
    if (tail < n && text[tail] != '#') {
      *err = MakeLineError(line_no, "unexpected characters after quoted value");
      return false;
    }
    return true;
  }

  std::string value;
  while (i < n) {
    const char c = text[i];
    if (c == '\\' && i + 1 < n && text[i + 1] == '#') {
      value.push_back('#');  // `\#` -> 字面 '#'，不视为注释起点
      i += 2;
      continue;
    }
    if (c == '#') {
      break;  // 行内注释起点
    }
    value.push_back(c);
    ++i;
  }
  *out = StringUtil::Trim(value, kTrimChars);
  return true;
}

// 把值序列化回 INI 文本时，决定是否需要加引号/转义。
// 目标是"ToString() 出来的文本能被自己读回去"，ReadBack 一致性优先于输出美观。
std::string FormatValue(const std::string& value) {
  const bool has_hash = value.find('#') != std::string::npos;
  const bool has_quote = value.find('"') != std::string::npos;
  const bool edge_space =
      !value.empty() && (value.front() == ' ' || value.front() == '\t' || value.back() == ' ' ||
                         value.back() == '\t');
  if (!has_hash && !has_quote && (value.empty() || edge_space)) {
    return "\"" + value + "\"";  // 引号内原样保留，读回来完全一致
  }
  if (has_hash) {
    // 只能转义 '#'；此时若同时又含首尾空白，读回时空白会被 trim（已知且可接受的降级）。
    return StringUtil::ReplaceAll(value, "#", "\\#");
  }
  return value;
}

// 逐行解析并**合并**到 sections（同 section+key 覆盖）。
bool ParseText(const std::string& content,
               std::map<std::string, std::map<std::string, std::string>>* sections,
               std::string* err) {
  std::string current_section;  // 段外键值归入空段名 ""
  std::size_t line_no = 0;

  std::size_t begin = 0;
  // 跳过 UTF-8 BOM：Windows 记事本保存的文件会带它，否则第一行的 section 名会被污染。
  if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
      static_cast<unsigned char>(content[1]) == 0xBB &&
      static_cast<unsigned char>(content[2]) == 0xBF) {
    begin = 3;
  }

  const std::size_t total = content.size();
  while (begin < total) {
    const std::size_t newline = content.find('\n', begin);
    std::string line = (newline == std::string::npos) ? content.substr(begin)
                                                      : content.substr(begin, newline - begin);
    ++line_no;

    if (!line.empty() && line.back() == '\r') {
      line.pop_back();  // 兼容 CRLF
    }

    const std::string trimmed = StringUtil::Trim(line, kTrimChars);
    if (trimmed.empty()) {
      // 空行
    } else if (trimmed[0] == '#' || trimmed[0] == ';') {
      // 整行注释
    } else if (trimmed[0] == '[') {
      const std::size_t close = trimmed.find(']', 1);
      if (close == std::string::npos) {
        *err = MakeLineError(line_no, "unterminated section header, expected ']'");
        return false;
      }
      const std::string tail = StringUtil::Trim(trimmed.substr(close + 1), kTrimChars);
      if (!tail.empty() && tail[0] != '#' && tail[0] != ';') {
        *err = MakeLineError(line_no, "unexpected characters after section header");
        return false;
      }
      const std::string name =
          StringUtil::ToLower(StringUtil::Trim(trimmed.substr(1, close - 1), kTrimChars));
      if (name.empty()) {
        *err = MakeLineError(line_no, "empty section name");
        return false;
      }
      current_section = name;
    } else {
      const std::size_t equal = trimmed.find('=');
      if (equal == std::string::npos) {
        *err = MakeLineError(line_no, "expected 'key = value'");
        return false;
      }
      const std::string key =
          StringUtil::ToLower(StringUtil::Trim(trimmed.substr(0, equal), kTrimChars));
      if (key.empty()) {
        *err = MakeLineError(line_no, "empty key name");
        return false;
      }
      if (key.find(' ') != std::string::npos || key.find('\t') != std::string::npos) {
        *err = MakeLineError(line_no, "whitespace inside key name");
        return false;
      }
      std::string value;
      if (!ParseValue(trimmed.substr(equal + 1), line_no, &value, err)) {
        return false;
      }
      (*sections)[current_section][key] = value;
    }

    if (newline == std::string::npos) {
      break;
    }
    begin = newline + 1;
  }
  return true;
}

}  // namespace

Config::Config() = default;

Config::~Config() = default;

bool Config::LoadFromFile(const std::string& path, std::string* err) {
  std::FILE* file = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&file, path.c_str(), "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "rb");
#endif
  if (file == nullptr) {
    last_error_ = path + ": cannot open file";
    if (err != nullptr) {
      *err = last_error_;
    }
    return false;
  }

  std::string content;
  char chunk[4096];
  std::size_t got = 0;
  while ((got = std::fread(chunk, 1, sizeof(chunk), file)) != 0) {
    content.append(chunk, got);
  }
  const bool read_failed = std::ferror(file) != 0;
  std::fclose(file);
  if (read_failed) {
    last_error_ = path + ": read error";
    if (err != nullptr) {
      *err = last_error_;
    }
    return false;
  }

  std::string parse_error;
  if (!LoadFromString(content, &parse_error)) {
    last_error_ = path + ": " + parse_error;
    if (err != nullptr) {
      *err = last_error_;
    }
    return false;
  }
  return true;
}

bool Config::LoadFromString(const std::string& content, std::string* err) {
  std::string error;
  if (!ParseText(content, &sections_, &error)) {
    last_error_ = error;
    if (err != nullptr) {
      *err = error;
    }
    return false;
  }
  last_error_.clear();
  return true;
}

void Config::Set(const std::string& section, const std::string& key, const std::string& value) {
  sections_[StringUtil::ToLower(section)][StringUtil::ToLower(key)] = value;
}

bool Config::Has(const std::string& section, const std::string& key) const {
  const auto outer = sections_.find(StringUtil::ToLower(section));
  if (outer == sections_.end()) {
    return false;
  }
  return outer->second.find(StringUtil::ToLower(key)) != outer->second.end();
}

std::string Config::GetString(const std::string& section, const std::string& key,
                              const std::string& default_value) const {
  const auto outer = sections_.find(StringUtil::ToLower(section));
  if (outer == sections_.end()) {
    return default_value;
  }
  const auto inner = outer->second.find(StringUtil::ToLower(key));
  if (inner == outer->second.end()) {
    return default_value;
  }
  return inner->second;
}

std::int64_t Config::GetInt(const std::string& section, const std::string& key,
                            std::int64_t default_value) const {
  const std::string raw = StringUtil::Trim(GetString(section, key, std::string()), kTrimChars);
  if (raw.empty()) {
    return default_value;
  }
  // 只认十进制与 0x 前缀十六进制；刻意不支持 "010" 这种八进制写法（配置里的前导 0 多半是笔误）。
  std::size_t sign = 0;
  if (raw[0] == '+' || raw[0] == '-') {
    sign = 1;
  }
  const bool hex = raw.size() >= sign + 2 && raw[sign] == '0' &&
                   (raw[sign + 1] == 'x' || raw[sign + 1] == 'X');
  errno = 0;
  char* end = nullptr;
  const long long value = std::strtoll(raw.c_str(), &end, hex ? 16 : 10);
  if (end == nullptr || *end != '\0' || errno == ERANGE) {
    return default_value;
  }
  return static_cast<std::int64_t>(value);
}

bool Config::GetBool(const std::string& section, const std::string& key,
                     bool default_value) const {
  if (!Has(section, key)) {
    return default_value;
  }
  const std::string value =
      StringUtil::ToLower(StringUtil::Trim(GetString(section, key, std::string()), kTrimChars));
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

double Config::GetDouble(const std::string& section, const std::string& key,
                         double default_value) const {
  const std::string raw = StringUtil::Trim(GetString(section, key, std::string()), kTrimChars);
  if (raw.empty()) {
    return default_value;
  }
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(raw.c_str(), &end);
  if (end == nullptr || *end != '\0' || errno == ERANGE) {
    return default_value;
  }
  return value;
}

std::vector<std::string> Config::GetKeys(const std::string& section) const {
  std::vector<std::string> keys;
  const auto outer = sections_.find(StringUtil::ToLower(section));
  if (outer == sections_.end()) {
    return keys;
  }
  keys.reserve(outer->second.size());
  for (const auto& item : outer->second) {
    keys.push_back(item.first);
  }
  return keys;  // std::map 本身有序，这里天然按字典序
}

std::vector<std::string> Config::GetSections() const {
  std::vector<std::string> names;
  names.reserve(sections_.size());
  for (const auto& item : sections_) {
    names.push_back(item.first);
  }
  return names;
}

std::string Config::ToString() const {
  std::string out;
  for (const auto& section : sections_) {
    if (!section.first.empty()) {
      out += '[';
      out += section.first;
      out += "]\n";
    }
    for (const auto& item : section.second) {
      out += item.first;
      out += " = ";
      out += FormatValue(item.second);
      out += '\n';
    }
    if (!section.first.empty()) {
      out += '\n';
    }
  }
  return out;
}

void Config::Clear() {
  sections_.clear();
  last_error_.clear();
}

}  // namespace qzrpc
