// 轻舟 RPC (Qingzhou RPC)
// src/common/config.h
//
// 职责：极简 INI 读取器。只做"文件 → 键值表"这一件事，不承载任何业务语义。
//
// 为什么不做成 phxrpc 那种"配置类继承 + DoRead 虚函数"：
//   那套设计会让"加一个配置项"必须改 4 个地方（头文件、基类、子类 DoRead、文档），
//   而且把配置对象变成全局可变态。轻舟 RPC 的做法是：
//     Config（本类，只负责解析） → 填充到 POD 结构体（ClientOptions / ServerOptions）
//   结构体构造后不可变，天然线程安全，也可以完全绕过文件用代码直接构造。
//
// 语法支持：
//   * 段：`[Section]`（段名大小写不敏感，内部统一转小写）
//   * 键值：`Key = Value`（键大小写不敏感；值保留原始大小写并 trim 首尾空白）
//   * 注释：整行以 `#` 或 `;` 开头；行内 `#` 之后视为注释（可用 `\#` 转义）
//   * 值可选双引号包裹（`Path = "C:\logs\a.log"`），去引号后不再 trim
//   * 不支持嵌套段、数组、include
//
// 段外键值：出现在任何 `[Section]` 之前的 `k = v`，归入**空段名 `""`**，
//           用 GetString("", "k") 读取。ServerOptions/ClientOptions::FromConfig 只读
//           "server"/"client" 段，因此全局键不会污染它们；但自定义读取方若也用空段名，
//           请确保全项目只有一种约定。
//
// 失败语义：LoadFromFile/LoadFromString 是**非事务性**的——边解析边写入，遇到错误立即
//           返回 false，**不回滚已解析的内容**。所以调用方拿到 false 时应当整体丢弃该
//           Config 对象（或重新构造一个），不要"用能读到的部分继续跑"：
//           半份配置比没有配置更危险，因为它会让本该失败的默认值静默生效。
//
// 线程安全：LoadFromFile / LoadFromString / Set 是**非线程安全**的，只允许在初始化期调用；
//           Get* 系列是只读操作，在多线程下并发调用安全（前提是无人并发写入）。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace qzrpc {

// INI 配置解析器。
class Config {
 public:
  Config();
  ~Config();

  // 从文件加载。多次调用会**合并**到现有内容（同 section+key 覆盖）。
  // 参数：path - 文件路径；err - 可选，失败时写入原因。
  // 返回：true 表示文件已成功解析。
  bool LoadFromFile(const std::string& path, std::string* err = nullptr);

  // 从内存中的文本加载（便于测试与默认值内嵌）。
  // 参数：content - INI 文本；err - 可选，失败时写入 "line N: ..."。
  bool LoadFromString(const std::string& content, std::string* err = nullptr);

  // 直接设值（用于代码里覆盖文件配置）。
  void Set(const std::string& section, const std::string& key, const std::string& value);

  // 键是否存在。
  bool Has(const std::string& section, const std::string& key) const;

  // 取字符串，缺失时返回 default_value。
  std::string GetString(const std::string& section, const std::string& key,
                        const std::string& default_value = std::string()) const;

  // 取整数，支持十进制与 0x 前缀十六进制。缺失或非法时返回 default_value。
  std::int64_t GetInt(const std::string& section, const std::string& key,
                      std::int64_t default_value = 0) const;

  // 取布尔。接受的真值："1"/"true"/"yes"/"on"（大小写不敏感）；其余为假。
  bool GetBool(const std::string& section, const std::string& key,
               bool default_value = false) const;

  // 取浮点。缺失或非法时返回 default_value。
  double GetDouble(const std::string& section, const std::string& key,
                   double default_value = 0.0) const;

  // 取某段下全部键名（已转小写，按字典序）。
  std::vector<std::string> GetKeys(const std::string& section) const;

  // 取全部段名（已转小写，按字典序）。
  std::vector<std::string> GetSections() const;

  // 序列化回 INI 文本（用于打印当前生效配置，排除问题时的利器）。
  std::string ToString() const;

  // 清空全部内容。
  void Clear();

 private:
  // 外层 key: 小写段名；内层 key: 小写键名；value: 原始值。
  std::map<std::string, std::map<std::string, std::string>> sections_;
  std::string last_error_;
};

}  // namespace qzrpc
