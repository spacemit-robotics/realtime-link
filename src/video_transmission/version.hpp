#ifndef VERSION_HPP
#define VERSION_HPP

#include <string>

namespace video_transmission {
// 构建身份：库版本、源码修订、构建时间、构建选项与编译器。
// 全部来自构建期注入的宏，不做系统调用；返回值进程内只构造一次。
// 文本不含逗号与空白，可安全写入日志行和 trace CSV 的 note 列。
const std::string& BuildInfo();
// 源码修订：<短修订号>（工作区含未提交修改时追加 -dirty）；取不到时为 unknown。
const std::string& SourceRevision();
}  // namespace video_transmission

#endif  // VERSION_HPP
