#include "WindowsFileAssociations.h"

#include "media/MediaProbe.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <iterator>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#endif

namespace cgplay {

namespace {

constexpr auto kProgId = L"CGPlay.Video";
constexpr auto kThumbnailShellEx = L"{E357FCCD-A995-4576-B01F-234630154E96}";
constexpr auto kThumbnailClsid = L"{B71A2E3C-9D47-4B1C-8D4D-2D4C04E1D9A7}";
constexpr auto kDevicePolicyKey = L"Software\\Policies\\Microsoft\\Windows\\System";
constexpr auto kDevicePolicyValue = L"DefaultAssociationsConfiguration";

#ifdef Q_OS_WIN

std::wstring toWide(const QString& value)
{
    return value.toStdWString();
}

QString fromWide(const std::wstring& value)
{
    return QString::fromStdWString(value);
}

QString keyForExtension(const QString& extension)
{
    return QStringLiteral("Software\\Classes\\.%1").arg(extension);
}

bool readString(const QString& key, const QString& name, QString* value);

QString userChoiceKeyForExtension(const QString& extension)
{
    return QStringLiteral(
        "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.%1\\UserChoice")
        .arg(extension);
}

bool isCgPlayProgId(const QString& progId)
{
    return progId.compare(QString::fromWCharArray(kProgId), Qt::CaseInsensitive) == 0 ||
        progId.compare(QStringLiteral("Applications\\CGPlay.exe"), Qt::CaseInsensitive) == 0;
}

QString commandExecutable(const QString& command)
{
    QString value = command.trimmed();
    if (value.isEmpty()) return {};
    if (value.startsWith(QLatin1Char('"'))) {
        const int end = value.indexOf(QLatin1Char('"'), 1);
        if (end > 1) value = value.mid(1, end - 1);
    } else {
        const int end = value.indexOf(QRegularExpression(QStringLiteral("\\s")));
        if (end > 0) value = value.left(end);
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(value));
}

bool commandTargetsCurrentApplication(const QString& command)
{
    const QString actual = commandExecutable(command);
    const QString expected = QDir::cleanPath(
        QDir::fromNativeSeparators(QCoreApplication::applicationFilePath()));
    if (actual.isEmpty()) return false;
    if (actual.compare(expected, Qt::CaseInsensitive) == 0) return true;

    // The settings dialog can be launched from a Release build while the
    // installed association still points at the packaged CGPlay.exe.  Both
    // are the same product, so report the association as effective when the
    // resolved executable is an existing CGPlay.exe rather than falsely
    // showing 0/31 until the installed binary is launched.
    const QFileInfo actualInfo(actual);
    const QFileInfo expectedInfo(expected);
    return actualInfo.exists() &&
        actualInfo.fileName().compare(expectedInfo.fileName(), Qt::CaseInsensitive) == 0 &&
        actualInfo.fileName().compare(QStringLiteral("CGPlay.exe"), Qt::CaseInsensitive) == 0;
}

bool hasExplicitUserChoice(const QString& extension)
{
    QString value;
    return readString(userChoiceKeyForExtension(extension), QStringLiteral("ProgId"), &value) &&
        !value.trimmed().isEmpty();
}

void pruneStaleOpenWithProgIds(const QString& extension)
{
    const QStringList keys {
        keyForExtension(extension) + QStringLiteral("\\OpenWithProgids"),
        QStringLiteral(
            "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.%1\\OpenWithProgids")
            .arg(extension)
    };
    const QString cgplayProgId = QString::fromWCharArray(kProgId);
    for (const QString& key : keys) {
        HKEY handle = nullptr;
        const auto keyWide = toWide(key);
        if (RegOpenKeyExW(HKEY_CURRENT_USER, keyWide.c_str(), 0,
                          KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &handle) != ERROR_SUCCESS) {
            continue;
        }
        QStringList values;
        DWORD index = 0;
        for (;;) {
            wchar_t name[512]{};
            DWORD nameLength = static_cast<DWORD>(std::size(name));
            const LONG result = RegEnumValueW(handle, index, name, &nameLength, nullptr,
                                               nullptr, nullptr, nullptr);
            if (result == ERROR_NO_MORE_ITEMS) break;
            if (result == ERROR_SUCCESS) {
                const QString valueName = QString::fromWCharArray(name, static_cast<int>(nameLength));
                if (valueName.compare(cgplayProgId, Qt::CaseInsensitive) != 0) values.push_back(valueName);
            }
            ++index;
        }
        for (const QString& value : values) {
            const auto valueWide = toWide(value);
            RegDeleteValueW(handle, valueWide.c_str());
        }
        RegCloseKey(handle);
    }
}

void pruneStaleOpenWithProgIdsForSupportedExtensions()
{
    for (const QString& extension : MediaProbe::videoExtensions()) {
        if (!hasExplicitUserChoice(extension)) pruneStaleOpenWithProgIds(extension);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

QString queryEffectiveOpenCommand(const QString& extension)
{
    const std::wstring association = (QStringLiteral(".") + extension).toStdWString();
    DWORD characterCount = 0;
    HRESULT result = AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, association.c_str(), nullptr,
                                       nullptr, &characterCount);
    if (result != S_FALSE || characterCount == 0) return {};
    std::wstring buffer(characterCount, L'\0');
    result = AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, association.c_str(), nullptr,
                               buffer.data(), &characterCount);
    if (FAILED(result)) return {};
    if (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
    return fromWide(buffer);
}

bool setStringAt(HKEY root, const QString& key, const QString& name, const QString& value)
{
    HKEY handle = nullptr;
    const auto keyWide = toWide(key);
    const auto nameWide = toWide(name);
    const auto valueWide = toWide(value);
    const LONG opened = RegCreateKeyExW(
        root, keyWide.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &handle, nullptr);
    if (opened != ERROR_SUCCESS) return false;
    const LONG written = RegSetValueExW(
        handle,
        nameWide.c_str(),
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(valueWide.c_str()),
        static_cast<DWORD>((valueWide.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle);
    return written == ERROR_SUCCESS;
}

bool setString(const QString& key, const QString& name, const QString& value)
{
    return setStringAt(HKEY_CURRENT_USER, key, name, value);
}

bool registerFileThumbnailInitialization(HKEY root, const QString& clsid)
{
    // This existing handler uses IInitializeWithFile to pass a seekable media
    // path to the separate FFmpeg process. The Shell's isolated thumbnail host
    // only initializes IInitializeWithStream handlers. Opt this CLSID into the
    // documented file-handler mode; this is not a global Explorer setting.
    const auto key = toWide(QStringLiteral("Software\\Classes\\CLSID\\%1").arg(clsid));
    HKEY handle = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                       KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &handle, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD enabled = 1;
    const LONG result = RegSetValueExW(handle, L"DisableProcessIsolation", 0, REG_DWORD,
                                      reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
    RegCloseKey(handle);
    return result == ERROR_SUCCESS;
}

bool readStringAt(HKEY root, const QString& key, const QString& name, QString* value)
{
    if (!value) return false;
    HKEY handle = nullptr;
    const auto keyWide = toWide(key);
    const auto nameWide = toWide(name);
    if (RegOpenKeyExW(root, keyWide.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &handle) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = RegQueryValueExW(
        handle, nameWide.c_str(), nullptr, &type, nullptr, &bytes);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(handle);
        return false;
    }
    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    result = RegQueryValueExW(
        handle,
        nameWide.c_str(),
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(buffer.data()),
        &bytes);
    RegCloseKey(handle);
    if (result != ERROR_SUCCESS) return false;
    if (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
    *value = fromWide(buffer);
    return true;
}

bool readString(const QString& key, const QString& name, QString* value)
{
    return readStringAt(HKEY_CURRENT_USER, key, name, value);
}

void deleteValueAt(HKEY root, const QString& key, const QString& name)
{
    HKEY handle = nullptr;
    const auto keyWide = toWide(key);
    const auto nameWide = toWide(name);
    if (RegOpenKeyExW(root, keyWide.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &handle) == ERROR_SUCCESS) {
        RegDeleteValueW(handle, nameWide.c_str());
        RegCloseKey(handle);
    }
}

void deleteValue(const QString& key, const QString& name)
{
    deleteValueAt(HKEY_CURRENT_USER, key, name);
}

void deleteTreeAt(HKEY root, const QString& key)
{
    const auto keyWide = toWide(key);
    RegDeleteTreeW(root, keyWide.c_str());
}

void deleteTree(const QString& key)
{
    deleteTreeAt(HKEY_CURRENT_USER, key);
}

QString devicePolicyXmlPath()
{
    QString programData;
    PWSTR knownFolder = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &knownFolder))) {
        programData = QString::fromWCharArray(knownFolder);
        CoTaskMemFree(knownFolder);
    }
    if (programData.isEmpty()) {
        programData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }
    return QDir(programData.isEmpty() ? QDir::rootPath() : programData)
        .filePath(QStringLiteral("CGPlay/DefaultAssociations.xml"));
}

bool writeTextFile(const QString& path, const QString& text, QString* error)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error) *error = QStringLiteral("无法创建设备策略目录：%1").arg(info.absolutePath());
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("无法写入设备策略文件：%1").arg(file.errorString());
        return false;
    }
    const QByteArray bytes = text.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("无法保存设备策略文件：%1").arg(file.errorString());
        return false;
    }
    return true;
}

bool securePolicyPath(const QString& path, bool directory, QString* error)
{
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID systemSid = nullptr;
    PSID administratorsSid = nullptr;
    PSID usersSid = nullptr;
    const bool allocated =
        AllocateAndInitializeSid(&ntAuthority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &systemSid) &&
        AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &administratorsSid) &&
        AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS,
                                 0, 0, 0, 0, 0, 0, &usersSid);
    if (!allocated) {
        if (systemSid) FreeSid(systemSid);
        if (administratorsSid) FreeSid(administratorsSid);
        if (usersSid) FreeSid(usersSid);
        if (error) *error = QStringLiteral("无法准备设备策略文件权限（Windows 错误 %1）").arg(GetLastError());
        return false;
    }

    EXPLICIT_ACCESSW entries[3]{};
    const DWORD inheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    const auto fill = [inheritance](EXPLICIT_ACCESSW& entry, PSID sid, DWORD permissions) {
        entry.grfAccessPermissions = permissions;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = inheritance;
        BuildTrusteeWithSidW(&entry.Trustee, sid);
    };
    fill(entries[0], systemSid, GENERIC_ALL);
    fill(entries[1], administratorsSid, GENERIC_ALL);
    fill(entries[2], usersSid, GENERIC_READ | GENERIC_EXECUTE);

    PACL acl = nullptr;
    DWORD result = SetEntriesInAclW(3, entries, nullptr, &acl);
    if (result == ERROR_SUCCESS) {
        std::wstring pathWide = toWide(path);
        result = SetNamedSecurityInfoW(
            pathWide.data(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, acl, nullptr);
    }
    if (acl) LocalFree(acl);
    FreeSid(systemSid);
    FreeSid(administratorsSid);
    FreeSid(usersSid);
    if (result != ERROR_SUCCESS) {
        if (error) *error = QStringLiteral("无法保护设备策略文件权限（Windows 错误 %1）").arg(result);
        return false;
    }
    return true;
}

bool ensurePolicyDirectorySecurity(const QString& directory, QString* error)
{
    if (!QDir().mkpath(directory) || !securePolicyPath(directory, true, error)) {
        if (error && error->isEmpty()) *error = QStringLiteral("无法创建或保护设备策略目录");
        return false;
    }
    return true;
}

bool registerMachineDefaults(QString* error)
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString icon = exe + QStringLiteral(",0");
    const QString command = QStringLiteral("\"%1\" \"%2\"").arg(exe, QStringLiteral("%1"));
    const QString progId = QString::fromWCharArray(kProgId);
    const QString shellEx = QString::fromWCharArray(kThumbnailShellEx);
    const QString clsid = QString::fromWCharArray(kThumbnailClsid);
    const QString provider = WindowsFileAssociations::thumbnailProviderPath();
    const auto set = [](const QString& key, const QString& name, const QString& value) {
        return setStringAt(HKEY_LOCAL_MACHINE, key, name, value);
    };
    if (!set(QStringLiteral("Software\\Classes\\CGPlay.Video"), QString(), QStringLiteral("CGPlay 视频")) ||
        !set(QStringLiteral("Software\\Classes\\CGPlay.Video\\DefaultIcon"), QString(), icon) ||
        !set(QStringLiteral("Software\\Classes\\CGPlay.Video\\shell\\open\\command"), QString(), command) ||
        !set(QStringLiteral("Software\\Classes\\Applications\\CGPlay.exe\\shell\\open\\command"), QString(), command) ||
        !set(QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\CGPlay.exe"), QString(), exe) ||
        !set(QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\CGPlay.exe"), QStringLiteral("Path"), QFileInfo(exe).absolutePath()) ||
        !set(QStringLiteral("Software\\CGPlay\\Capabilities"), QStringLiteral("ApplicationName"), QStringLiteral("CGPlay")) ||
        !set(QStringLiteral("Software\\CGPlay\\Capabilities"), QStringLiteral("ApplicationDescription"), QStringLiteral("CGPlay 视频播放器与审片工具")) ||
        !set(QStringLiteral("Software\\CGPlay\\Capabilities"), QStringLiteral("ApplicationIcon"), icon) ||
        !set(QStringLiteral("Software\\RegisteredApplications"), QStringLiteral("CGPlay"), QStringLiteral("Software\\CGPlay\\Capabilities"))) {
        if (error) *error = QStringLiteral("无法写入设备级播放器注册信息，请确认已允许管理员权限");
        return false;
    }
    if (QFileInfo::exists(provider) &&
        (!registerFileThumbnailInitialization(HKEY_LOCAL_MACHINE, clsid) ||
         !set(QStringLiteral("Software\\Classes\\CLSID\\%1\\InprocServer32").arg(clsid), QString(), provider) ||
         !set(QStringLiteral("Software\\Classes\\CLSID\\%1\\InprocServer32").arg(clsid), QStringLiteral("ThreadingModel"), QStringLiteral("Apartment")) ||
         !set(QStringLiteral("Software\\Classes\\CGPlay.Video\\ShellEx\\%1").arg(shellEx), QString(), clsid))) {
        if (error) *error = QStringLiteral("无法注册设备级缩略图组件");
        return false;
    }
    for (const QString& extension : WindowsFileAssociations::supportedVideoExtensions()) {
        const QString normalized = extension.toLower();
        const QString key = QStringLiteral("Software\\Classes\\.%1").arg(normalized);
        if (!set(key, QString(), progId) ||
            !set(key, QStringLiteral("PerceivedType"), QStringLiteral("video")) ||
            !set(key, QStringLiteral("Content Type"), QStringLiteral("video/%1").arg(normalized)) ||
            !set(key + QStringLiteral("\\OpenWithProgids"), progId, QString()) ||
            !set(key + QStringLiteral("\\ShellEx\\%1").arg(shellEx), QString(), clsid) ||
            !set(QStringLiteral("Software\\Classes\\Applications\\CGPlay.exe\\SupportedTypes"),
                QStringLiteral(".%1").arg(normalized), QString()) ||
            !set(QStringLiteral("Software\\CGPlay\\Capabilities\\FileAssociations"),
                QStringLiteral(".%1").arg(normalized), progId)) {
            if (error) *error = QStringLiteral("无法注册设备级 .%1 文件关联").arg(normalized);
            return false;
        }
    }
    return true;
}

void unregisterMachineDefaults()
{
    const QString progId = QString::fromWCharArray(kProgId);
    const QString shellEx = QString::fromWCharArray(kThumbnailShellEx);
    deleteTreeAt(HKEY_LOCAL_MACHINE, QStringLiteral("Software\\Classes\\CGPlay.Video"));
    deleteTreeAt(HKEY_LOCAL_MACHINE, QStringLiteral("Software\\Classes\\Applications\\CGPlay.exe"));
    deleteTreeAt(HKEY_LOCAL_MACHINE, QStringLiteral("Software\\Classes\\CLSID\\%1").arg(QString::fromWCharArray(kThumbnailClsid)));
    deleteTreeAt(HKEY_LOCAL_MACHINE, QStringLiteral("Software\\CGPlay\\Capabilities"));
    deleteValueAt(HKEY_LOCAL_MACHINE, QStringLiteral("Software\\RegisteredApplications"), QStringLiteral("CGPlay"));
    deleteTreeAt(HKEY_LOCAL_MACHINE, QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\CGPlay.exe"));
    for (const QString& extension : WindowsFileAssociations::supportedVideoExtensions()) {
        const QString normalized = extension.toLower();
        const QString key = QStringLiteral("Software\\Classes\\.%1").arg(normalized);
        QString current;
        if (readStringAt(HKEY_LOCAL_MACHINE, key, QString(), &current) &&
            current.compare(progId, Qt::CaseInsensitive) == 0) {
            deleteValueAt(HKEY_LOCAL_MACHINE, key, QString());
        }
        QString perceived;
        if (readStringAt(HKEY_LOCAL_MACHINE, key, QStringLiteral("PerceivedType"), &perceived) &&
            perceived.compare(QStringLiteral("video"), Qt::CaseInsensitive) == 0) {
            deleteValueAt(HKEY_LOCAL_MACHINE, key, QStringLiteral("PerceivedType"));
        }
        QString contentType;
        if (readStringAt(HKEY_LOCAL_MACHINE, key, QStringLiteral("Content Type"), &contentType) &&
            contentType.compare(QStringLiteral("video/%1").arg(normalized), Qt::CaseInsensitive) == 0) {
            deleteValueAt(HKEY_LOCAL_MACHINE, key, QStringLiteral("Content Type"));
        }
        deleteValueAt(HKEY_LOCAL_MACHINE, key + QStringLiteral("\\OpenWithProgids"), progId);
        deleteValueAt(HKEY_LOCAL_MACHINE, key + QStringLiteral("\\ShellEx\\%1").arg(shellEx), QString());
    }
}

bool runDevicePolicyWorker(bool remove, QString* error)
{
    const QString policyKey = QString::fromWCharArray(kDevicePolicyKey);
    const QString policyValue = QString::fromWCharArray(kDevicePolicyValue);
    const QString xmlPath = QDir::toNativeSeparators(devicePolicyXmlPath());
    QString existing;
    const bool hasExisting = readStringAt(HKEY_LOCAL_MACHINE, policyKey, policyValue, &existing);
    if (!remove) {
        if (hasExisting && existing.compare(xmlPath, Qt::CaseInsensitive) != 0) {
            if (error) *error = QStringLiteral("检测到其他程序的设备级默认关联策略，未覆盖：%1").arg(existing);
            return false;
        }
        if (!hasExisting) {
            QString machineApp;
            QString machineProgId;
            const bool machineRegistrationExists =
                readStringAt(HKEY_LOCAL_MACHINE, QStringLiteral("Software\\RegisteredApplications"),
                             QStringLiteral("CGPlay"), &machineApp) ||
                readStringAt(HKEY_LOCAL_MACHINE, QStringLiteral("Software\\Classes\\CGPlay.Video"),
                             QString(), &machineProgId);
            if (machineRegistrationExists) {
                if (error) *error = QStringLiteral("检测到已有 CGPlay 机器级注册，但没有对应策略；未覆盖，请先撤销旧部署或重新安装。");
                return false;
            }
        }
        if (hasExisting && !QFileInfo::exists(xmlPath)) {
            if (error) *error = QStringLiteral("CGPlay 设备策略文件丢失，未覆盖现有策略：%1").arg(xmlPath);
            return false;
        }
        QByteArray previousXml;
        if (hasExisting) {
            QFile previous(xmlPath);
            if (!previous.open(QIODevice::ReadOnly)) {
                if (error) *error = QStringLiteral("无法读取现有 CGPlay 设备策略文件：%1").arg(previous.errorString());
                return false;
            }
            previousXml = previous.readAll();
        }
        const auto restoreXml = [&]() {
            if (!hasExisting) {
                return QFile::remove(xmlPath) || !QFileInfo::exists(xmlPath);
            }
            QSaveFile restore(xmlPath);
            return restore.open(QIODevice::WriteOnly) &&
                restore.write(previousXml) == previousXml.size() &&
                restore.commit() && securePolicyPath(xmlPath, false, nullptr);
        };
        if (!ensurePolicyDirectorySecurity(QFileInfo(xmlPath).absolutePath(), error)) return false;
        if (!writeTextFile(xmlPath, WindowsFileAssociations::defaultAssociationsXml(), error)) return false;
        if (!securePolicyPath(xmlPath, false, error)) {
            if (!restoreXml() && error) *error += QStringLiteral("；且无法恢复原策略文件");
            return false;
        }
        if (!registerMachineDefaults(error)) {
            if (!restoreXml() && error) *error += QStringLiteral("；且无法恢复原策略文件");
            if (!hasExisting) unregisterMachineDefaults();
            return false;
        }
        if (!setStringAt(HKEY_LOCAL_MACHINE, policyKey, policyValue, xmlPath)) {
            if (!restoreXml() && error) *error += QStringLiteral("；且无法恢复原策略文件");
            if (!hasExisting) unregisterMachineDefaults();
            if (error) *error = QStringLiteral("无法启用 Windows 默认关联策略，请确认管理员权限");
            return false;
        }
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return true;
    }

    if (!hasExisting) {
        return true;
    }
    if (existing.compare(xmlPath, Qt::CaseInsensitive) != 0) {
        if (error) *error = QStringLiteral("当前设备默认关联策略不是 CGPlay，未删除：%1").arg(existing);
        return false;
    }
    deleteValueAt(HKEY_LOCAL_MACHINE, policyKey, policyValue);
    QFile::remove(xmlPath);
    unregisterMachineDefaults();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool launchElevatedWorker(const QString& argument, QString* error)
{
    const std::wstring executable = QCoreApplication::applicationFilePath().toStdWString();
    const std::wstring parameters = argument.toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        if (error) *error = code == ERROR_CANCELLED
            ? QStringLiteral("已取消管理员授权")
            : QStringLiteral("无法启动管理员部署（Windows 错误 %1）").arg(code);
        return false;
    }
    constexpr DWORD kWorkerTimeoutMs = 120000;
    const DWORD waitResult = WaitForSingleObject(info.hProcess, kWorkerTimeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hProcess);
        if (error) *error = QStringLiteral("管理员部署超时，已终止策略工作进程");
        return false;
    }
    if (waitResult != WAIT_OBJECT_0) {
        CloseHandle(info.hProcess);
        if (error) *error = QStringLiteral("等待管理员部署进程失败（Windows 错误 %1）").arg(GetLastError());
        return false;
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    if (exitCode != 0) {
        if (error) *error = QStringLiteral("管理员部署失败（退出码 %1）").arg(exitCode);
        return false;
    }
    return true;
}

bool setDefaultForExtension(const QString& extension, bool enabled)
{
    const QString key = keyForExtension(extension);
    if (enabled) {
        return setString(key, QString(), QString::fromWCharArray(kProgId)) &&
            setString(key, QStringLiteral("PerceivedType"), QStringLiteral("video")) &&
            setString(key, QStringLiteral("Content Type"), QStringLiteral("video/%1").arg(extension)) &&
            setString(key + QStringLiteral("\\OpenWithProgids"), QString::fromWCharArray(kProgId), QString()) &&
            setString(key + QStringLiteral("\\ShellEx\\%1").arg(QString::fromWCharArray(kThumbnailShellEx)),
                      QString(), QString::fromWCharArray(kThumbnailClsid));
    }

    deleteValue(key + QStringLiteral("\\OpenWithProgids"), QString::fromWCharArray(kProgId));
    deleteValue(key + QStringLiteral("\\ShellEx\\%1").arg(QString::fromWCharArray(kThumbnailShellEx)), QString());
    QString current;
    if (readString(key, QString(), &current) && current.compare(QString::fromWCharArray(kProgId), Qt::CaseInsensitive) == 0) {
        deleteValue(key, QString());
    }
    QString perceived;
    if (readString(key, QStringLiteral("PerceivedType"), &perceived) && perceived.compare(QStringLiteral("video"), Qt::CaseInsensitive) == 0) {
        deleteValue(key, QStringLiteral("PerceivedType"));
    }
    QString contentType;
    if (readString(key, QStringLiteral("Content Type"), &contentType) && contentType.compare(QStringLiteral("video/%1").arg(extension), Qt::CaseInsensitive) == 0) {
        deleteValue(key, QStringLiteral("Content Type"));
    }
    return true;
}

bool providerRegistryReady(QString* error)
{
    const QString provider = WindowsFileAssociations::thumbnailProviderPath();
    if (!QFileInfo::exists(provider)) {
        if (error) *error = QStringLiteral("缩略图组件不存在：%1").arg(provider);
        return false;
    }
    const QString key = QStringLiteral("Software\\Classes\\CLSID\\%1\\InprocServer32")
        .arg(WindowsFileAssociations::thumbnailProviderClsid());
    if (!registerFileThumbnailInitialization(HKEY_CURRENT_USER, WindowsFileAssociations::thumbnailProviderClsid()) ||
        !setString(key, QString(), provider) ||
        !setString(key, QStringLiteral("ThreadingModel"), QStringLiteral("Apartment"))) {
        if (error) *error = QStringLiteral("无法注册 Windows 缩略图组件（注册表写入失败）");
        return false;
    }
    return setString(
        QStringLiteral("Software\\Classes\\CGPlay.Video\\ShellEx\\%1")
            .arg(QString::fromWCharArray(kThumbnailShellEx)),
        QString(),
        WindowsFileAssociations::thumbnailProviderClsid());
}

#endif

} // namespace

QStringList WindowsFileAssociations::supportedVideoExtensions()
{
    return MediaProbe::videoExtensions();
}

QSet<QString> WindowsFileAssociations::selectedVideoExtensions()
{
    QSet<QString> result;
#ifdef Q_OS_WIN
    for (const QString& extension : supportedVideoExtensions()) {
        QString value;
        const QString key = keyForExtension(extension);
        const bool defaultedToCgPlay = readString(key, QString(), &value) &&
            value.compare(QString::fromWCharArray(kProgId), Qt::CaseInsensitive) == 0;
        const bool offeredByCgPlay = readString(
            key + QStringLiteral("\\OpenWithProgids"), QString::fromWCharArray(kProgId), &value);
        if (defaultedToCgPlay || offeredByCgPlay) result.insert(extension);
    }
#endif
    return result;
}

QSet<QString> WindowsFileAssociations::effectiveDefaultVideoExtensions()
{
    QSet<QString> result;
#ifdef Q_OS_WIN
    const QString progIdKey = QStringLiteral("Software\\Classes\\CGPlay.Video\\shell\\open\\command");
    const QString applicationKey = QStringLiteral("Software\\Classes\\Applications\\CGPlay.exe\\shell\\open\\command");
    for (const QString& extension : supportedVideoExtensions()) {
        QString userChoice;
        if (readString(userChoiceKeyForExtension(extension), QStringLiteral("ProgId"), &userChoice) &&
            !userChoice.trimmed().isEmpty()) {
            if (!isCgPlayProgId(userChoice)) continue;
            QString command;
            const QString commandKey = userChoice.compare(QStringLiteral("Applications\\CGPlay.exe"), Qt::CaseInsensitive) == 0
                ? applicationKey
                : progIdKey;
            if (readString(commandKey, QString(), &command) && commandTargetsCurrentApplication(command)) {
                result.insert(extension);
            }
            continue;
        }

        const QString command = queryEffectiveOpenCommand(extension);
        if (commandTargetsCurrentApplication(command)) result.insert(extension);
    }
#endif
    return result;
}

bool WindowsFileAssociations::applyVideoExtensions(const QSet<QString>& extensions, QString* error)
{
#ifndef Q_OS_WIN
    if (error) *error = QStringLiteral("文件关联设置仅支持 Windows");
    Q_UNUSED(extensions);
    return false;
#else
    if (!providerRegistryReady(error)) return false;

    const QString progId = QString::fromWCharArray(kProgId);
    const QString shellEx = QStringLiteral("ShellEx\\%1").arg(QString::fromWCharArray(kThumbnailShellEx));
    if (!setString(QStringLiteral("Software\\Classes\\CGPlay.Video"), QString(), QStringLiteral("CGPlay 视频")) ||
        !setString(QStringLiteral("Software\\Classes\\CGPlay.Video\\DefaultIcon"), QString(),
                   QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("CGPlay.exe,0"))) ||
        !setString(QStringLiteral("Software\\Classes\\CGPlay.Video\\shell\\open\\command"), QString(),
                   QStringLiteral("\"%1\" \"%2\"").arg(
                       QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), QStringLiteral("%1"))) ||
        !setString(QStringLiteral("Software\\Classes\\Applications\\CGPlay.exe\\shell\\open\\command"), QString(),
                   QStringLiteral("\"%1\" \"%2\"").arg(
                       QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), QStringLiteral("%1"))) ||
        !setString(QStringLiteral("Software\\CGPlay\\Capabilities"), QStringLiteral("ApplicationName"), QStringLiteral("CGPlay")) ||
        !setString(QStringLiteral("Software\\CGPlay\\Capabilities"), QStringLiteral("ApplicationDescription"), QStringLiteral("CGPlay 视频播放器与审片工具")) ||
        !setString(QStringLiteral("Software\\CGPlay\\Capabilities"), QStringLiteral("ApplicationIcon"),
                   QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("CGPlay.exe,0"))) ||
        !setString(QStringLiteral("Software\\RegisteredApplications"), QStringLiteral("CGPlay"), QStringLiteral("Software\\CGPlay\\Capabilities"))) {
        if (error) *error = QStringLiteral("文件关联注册失败（注册表写入失败）");
        return false;
    }

    deleteTree(QStringLiteral("Software\\CGPlay\\Capabilities\\FileAssociations"));
    deleteTree(QStringLiteral("Software\\Classes\\Applications\\CGPlay.exe\\SupportedTypes"));
    for (const QString& extension : supportedVideoExtensions()) {
        const QString normalized = extension.toLower();
        const bool enabled = extensions.contains(normalized);
        if (!setDefaultForExtension(normalized, enabled)) {
            if (error) *error = QStringLiteral("无法设置 .%1 文件关联").arg(normalized);
            return false;
        }
        if (enabled) {
            // Windows can prefer stale historical ProgIDs over the Classes
            // default when UserChoice is absent. Preserve explicit choices,
            // but remove only those stale candidates for formats the user
            // explicitly applied to CGPlay.
            if (!hasExplicitUserChoice(normalized)) pruneStaleOpenWithProgIds(normalized);
            if (!setString(QStringLiteral("Software\\Classes\\Applications\\CGPlay.exe\\SupportedTypes"),
                           QStringLiteral(".%1").arg(normalized), QString()) ||
                !setString(QStringLiteral("Software\\CGPlay\\Capabilities\\FileAssociations"),
                           QStringLiteral(".%1").arg(normalized), progId)) {
                if (error) *error = QStringLiteral("无法登记 .%1 文件关联能力").arg(normalized);
                return false;
            }
        }
    }
    if (!setString(QStringLiteral("Software\\Classes\\CGPlay.Video\\%1").arg(shellEx), QString(), thumbnailProviderClsid())) {
        if (error) *error = QStringLiteral("无法登记缩略图处理器");
        return false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
#endif
}

QString WindowsFileAssociations::defaultAssociationsXml()
{
    QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                 "<DefaultAssociations Version=\"1\">\n");
    for (const QString& extension : supportedVideoExtensions()) {
        const QString normalized = extension.toLower();
        xml += QStringLiteral("  <Association Identifier=\".%1\" ProgId=\"CGPlay.Video\" "
                              "ApplicationName=\"CGPlay\" Suggested=\"false\" />\n").arg(normalized);
    }
    xml += QStringLiteral("</DefaultAssociations>\n");
    return xml;
}

bool WindowsFileAssociations::requestDeviceDefaultAssociations(QString* error)
{
#ifndef Q_OS_WIN
    if (error) *error = QStringLiteral("设备级默认播放器策略仅支持 Windows");
    return false;
#else
    // Clean only stale OpenWith candidates for extensions that do not have a
    // protected UserChoice. Existing explicit user choices remain untouched.
    pruneStaleOpenWithProgIdsForSupportedExtensions();
    return launchElevatedWorker(QStringLiteral("--cgplay-deploy-default-associations"), error);
#endif
}

bool WindowsFileAssociations::requestRemoveDeviceDefaultAssociations(QString* error)
{
#ifndef Q_OS_WIN
    if (error) *error = QStringLiteral("设备级默认播放器策略仅支持 Windows");
    return false;
#else
    return launchElevatedWorker(QStringLiteral("--cgplay-remove-default-associations"), error);
#endif
}

bool WindowsFileAssociations::runDeviceDefaultAssociationsWorker(QString* error)
{
#ifndef Q_OS_WIN
    if (error) *error = QStringLiteral("设备级默认播放器策略仅支持 Windows");
    return false;
#else
    return runDevicePolicyWorker(false, error);
#endif
}

bool WindowsFileAssociations::runRemoveDeviceDefaultAssociationsWorker(QString* error)
{
#ifndef Q_OS_WIN
    if (error) *error = QStringLiteral("设备级默认播放器策略仅支持 Windows");
    return false;
#else
    return runDevicePolicyWorker(true, error);
#endif
}

bool WindowsFileAssociations::registerThumbnailProvider(QString* error)
{
#ifndef Q_OS_WIN
    if (error) *error = QStringLiteral("缩略图组件仅支持 Windows");
    return false;
#else
    return providerRegistryReady(error);
#endif
}

QString WindowsFileAssociations::thumbnailProviderClsid()
{
    return QString::fromWCharArray(kThumbnailClsid);
}

QString WindowsFileAssociations::thumbnailProviderPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("CGPlayThumbnailProvider.dll"));
}

} // namespace cgplay
