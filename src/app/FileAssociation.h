#pragma once

#include <QString>

namespace steplens {

struct FileAssociationResult {
    bool ok = false;
    QString message;
};

// 只在用户明确打开或关闭时调用。启动路径不得调用，本环境也不得执行。
// Windows 上只登记当前用户的应用能力，再打开系统的默认应用选择窗口。
// 不写、不伪造 UserChoice，也不改扩展名的现有默认值。
[[nodiscard]] FileAssociationResult enableFileAssociation();
[[nodiscard]] FileAssociationResult disableFileAssociation();

} // namespace steplens
