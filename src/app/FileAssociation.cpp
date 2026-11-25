#include "app/FileAssociation.h"

#include "app/settings/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QList>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>
#include <shobjidl.h>
#include <windows.h>
#endif

namespace steplens {
namespace {

#if defined(_WIN32)
constexpr wchar_t kRecordSeparator = 0x1F;

struct OwnedValue {
    QString key;
    QString name;
    QString data;
};

QString commandText() {
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QStringLiteral("\"%1\" \"%2\"").arg(exe, QStringLiteral("%1"));
}

QString recordKey(const OwnedValue& value) {
    return value.key + QChar(kRecordSeparator) + value.name;
}

QString encode(const OwnedValue& value) {
    return value.key + QChar(kRecordSeparator) + value.name + QChar(kRecordSeparator) + value.data;
}

bool decode(const QString& text, OwnedValue* value) {
    const int first = text.indexOf(QChar(kRecordSeparator));
    const int second = first < 0 ? -1 : text.indexOf(QChar(kRecordSeparator), first + 1);
    if (first < 0 || second < 0) {
        return false;
    }
    value->key = text.left(first);
    value->name = text.mid(first + 1, second - first - 1);
    value->data = text.mid(second + 1);
    return !value->key.isEmpty();
}

bool readValue(const OwnedValue& owned, QString* data, bool* present) {
    *present = false;
    data->clear();
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, owned.key.toStdWString().c_str(), 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return true;
    }
    const std::wstring name = owned.name.toStdWString();
    const wchar_t* valueName = owned.name.isEmpty() ? nullptr : name.c_str();
    DWORD type = 0;
    DWORD bytes = 0;
    const LONG sizeRc = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    if (sizeRc == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        return true;
    }
    if (sizeRc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return false;
    }
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    const LONG readRc =
        RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(text.data()), &bytes);
    RegCloseKey(key);
    if (readRc != ERROR_SUCCESS) {
        return false;
    }
    if (!text.empty() && text.back() == L'\0') {
        text.pop_back();
    }
    *present = true;
    *data = QString::fromWCharArray(text.c_str());
    return true;
}

bool writeValue(const OwnedValue& owned) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, owned.key.toStdWString().c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, &disposition) != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring name = owned.name.toStdWString();
    const wchar_t* valueName = owned.name.isEmpty() ? nullptr : name.c_str();
    const std::wstring text = owned.data.toStdWString();
    const DWORD bytes = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t));
    const LONG rc =
        RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()), bytes);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool deleteValue(const OwnedValue& owned) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, owned.key.toStdWString().c_str(), 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
        return true;
    }
    const std::wstring name = owned.name.toStdWString();
    const wchar_t* valueName = owned.name.isEmpty() ? nullptr : name.c_str();
    const LONG rc = RegDeleteValueW(key, valueName);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

void deleteKeyIfEmpty(const wchar_t* keyPath) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    DWORD subkeys = 0;
    DWORD values = 0;
    const LONG info = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                                       nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    if (info == ERROR_SUCCESS && subkeys == 0 && values == 0) {
        RegDeleteKeyW(HKEY_CURRENT_USER, keyPath);
    }
}

bool undoWritten(const QList<OwnedValue>& written, const QHash<QString, QString>& previous) {
    for (const OwnedValue& owned : written) {
        QString current;
        bool present = false;
        if (!readValue(owned, &current, &present)) {
            return false;
        }
        if (!present || current != owned.data) {
            continue;
        }
        const auto found = previous.constFind(recordKey(owned));
        if (found == previous.cend()) {
            if (!deleteValue(owned)) {
                return false;
            }
        } else {
            OwnedValue restore = owned;
            restore.data = found.value();
            if (!writeValue(restore)) {
                return false;
            }
        }
    }
    return true;
}

QHash<QString, QString> previousValues() {
    QHash<QString, QString> values;
    for (const QString& line : Settings::instance().savedAssociations()) {
        OwnedValue owned;
        if (decode(line, &owned)) {
            values.insert(recordKey(owned), owned.data);
        }
    }
    return values;
}

QList<OwnedValue> intendedRecords() {
    const QString command = commandText();
    const QString capabilities = QStringLiteral("Software\\StepLens\\Capabilities");
    const QString progId = QStringLiteral("Software\\Classes\\StepLens.Document");
    return {
        {progId, QString(), QStringLiteral("STEP/IGES")},
        {progId + QStringLiteral("\\shell\\open\\command"), QString(), command},
        {capabilities, QStringLiteral("ApplicationName"), QStringLiteral("StepLens")},
        {capabilities, QStringLiteral("ApplicationDescription"), QStringLiteral("StepLens")},
        {capabilities + QStringLiteral("\\FileAssociations"), QStringLiteral(".step"),
         QStringLiteral("StepLens.Document")},
        {capabilities + QStringLiteral("\\FileAssociations"), QStringLiteral(".stp"),
         QStringLiteral("StepLens.Document")},
        {capabilities + QStringLiteral("\\FileAssociations"), QStringLiteral(".iges"),
         QStringLiteral("StepLens.Document")},
        {capabilities + QStringLiteral("\\FileAssociations"), QStringLiteral(".igs"),
         QStringLiteral("StepLens.Document")},
        {QStringLiteral("Software\\RegisteredApplications"), QStringLiteral("StepLens"), capabilities},
    };
}

bool launchDefaultAppChoice(QString* message) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = init == S_OK;
    IApplicationAssociationRegistrationUI* dialog = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_ApplicationAssociationRegistrationUI, nullptr,
                                             CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    HRESULT launched = E_FAIL;
    if (SUCCEEDED(created) && dialog != nullptr) {
        launched = dialog->LaunchAdvancedAssociationUI(L"StepLens");
        dialog->Release();
    }
    if (uninit) {
        CoUninitialize();
    }
    if (SUCCEEDED(launched)) {
        return true;
    }
    *message = QStringLiteral("系统没有打开选择默认应用的窗口。没有改现有默认应用");
    return false;
}

FileAssociationResult revokeOwned(bool* keptForeign) {
    *keptForeign = false;
    const QStringList saved = Settings::instance().savedAssociations();
    for (const QString& line : saved) {
        OwnedValue owned;
        if (!decode(line, &owned)) {
            continue;
        }
        QString current;
        bool present = false;
        if (!readValue(owned, &current, &present)) {
            return {false, QStringLiteral("读取已登记项失败，没有继续删除")};
        }
        if (present && current == owned.data) {
            if (!deleteValue(owned)) {
                return {false, QStringLiteral("删除本程序登记项失败")};
            }
        } else if (present) {
            *keptForeign = true;
        }
    }
    deleteKeyIfEmpty(L"Software\\Classes\\StepLens.Document\\shell\\open\\command");
    deleteKeyIfEmpty(L"Software\\Classes\\StepLens.Document\\shell\\open");
    deleteKeyIfEmpty(L"Software\\Classes\\StepLens.Document\\shell");
    deleteKeyIfEmpty(L"Software\\Classes\\StepLens.Document");
    deleteKeyIfEmpty(L"Software\\StepLens\\Capabilities\\FileAssociations");
    deleteKeyIfEmpty(L"Software\\StepLens\\Capabilities");
    deleteKeyIfEmpty(L"Software\\StepLens");
    if (!Settings::instance().setSavedAssociations({})) {
        return {false, QStringLiteral("本程序的登记已处理，但设置里的备份没有清掉")};
    }
    if (*keptForeign) {
        return {true, QStringLiteral("已去掉本程序仍拥有的登记。被别人改过的项保留")};
    }
    return {true, QStringLiteral("已取消本程序的文件关联登记")};
}
#endif

} // namespace

FileAssociationResult enableFileAssociation() {
#if !defined(_WIN32)
    return {false, QStringLiteral("文件关联只走 Windows 的默认应用选择窗口，这个系统不登记")};
#else
    const QList<OwnedValue> records = intendedRecords();
    const QHash<QString, QString> previous = previousValues();
    for (const OwnedValue& owned : records) {
        QString current;
        bool present = false;
        if (!readValue(owned, &current, &present)) {
            return {false, QStringLiteral("读取现有登记失败，没有写入")};
        }
        if (!present || current == owned.data) {
            continue;
        }
        const auto found = previous.constFind(recordKey(owned));
        if (found == previous.cend() || found.value() != current) {
            return {false, QStringLiteral("已有登记不是本程序写下的，没有覆盖")};
        }
    }
    QStringList snapshot;
    QList<OwnedValue> written;
    snapshot.reserve(records.size());
    for (const OwnedValue& owned : records) {
        if (!writeValue(owned)) {
            undoWritten(written, previous);
            return {false, QStringLiteral("登记本程序失败，已尝试撤回刚写的项")};
        }
        written.push_back(owned);
        snapshot.push_back(encode(owned));
    }
    if (!Settings::instance().setSavedAssociations(snapshot)) {
        undoWritten(written, previous);
        return {false, QStringLiteral("登记备份没有写入设置，已尝试撤回")};
    }
    QString dialogMessage;
    if (!launchDefaultAppChoice(&dialogMessage)) {
        bool keptForeign = false;
        const FileAssociationResult revoked = revokeOwned(&keptForeign);
        if (!revoked.ok) {
            return {false, dialogMessage + QStringLiteral("\n") + revoked.message};
        }
        return {false, dialogMessage};
    }
    return {true, QStringLiteral("已登记本程序，并打开系统的默认应用选择窗口。没有直接改默认应用")};
#endif
}

FileAssociationResult disableFileAssociation() {
#if !defined(_WIN32)
    return {false, QStringLiteral("这里不能访问 Windows 注册表，没有改关联")};
#else
    bool keptForeign = false;
    return revokeOwned(&keptForeign);
#endif
}

} // namespace steplens
