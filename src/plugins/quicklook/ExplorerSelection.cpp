#include "ExplorerSelection.h"

#include "QuickLookDebug.h"

#include <QFileInfo>

#include <windows.h>
#include <ExDisp.h>
#include <ShlDisp.h>
#include <oleauto.h>

namespace cgplay::quicklook {

namespace {

template<typename T>
void safeRelease(T*& ptr)
{
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

HWND topLevelWindow(HWND hwnd)
{
    if (!hwnd) {
        return nullptr;
    }
    return GetAncestor(hwnd, GA_ROOT);
}

bool isExplorerWindowClass(HWND hwnd)
{
    const HWND root = topLevelWindow(hwnd);
    if (!root) {
        return false;
    }

    wchar_t className[256] = {};
    const int count = GetClassNameW(root, className, 255);
    if (count <= 0) {
        return false;
    }

    const QString cls = QString::fromWCharArray(className);
    return cls == QStringLiteral("CabinetWClass") ||
           cls == QStringLiteral("ExploreWClass");
}

QString folderItemPath(FolderItem* item)
{
    if (!item) {
        return {};
    }

    BSTR path = nullptr;
    const HRESULT hr = item->get_Path(&path);
    if (FAILED(hr) || !path) {
        return {};
    }

    const QString result = QString::fromWCharArray(path);
    SysFreeString(path);
    return result;
}

} // namespace

bool isExplorerForegroundWindow()
{
    return isExplorerWindowClass(GetForegroundWindow());
}

bool isExplorerTextInputFocused()
{
    const HWND foreground = GetForegroundWindow();
    if (!isExplorerWindowClass(foreground)) {
        return false;
    }

    DWORD threadId = GetWindowThreadProcessId(foreground, nullptr);
    GUITHREADINFO guiInfo{ sizeof(GUITHREADINFO) };
    if (!threadId || !GetGUIThreadInfo(threadId, &guiInfo)) {
        return false;
    }

    // Explorer's rename box, address bar, and search box are child controls;
    // walk up from the focused control so the space hook never consumes text
    // input while still retaining the normal Space-to-preview behavior.
    for (HWND current = guiInfo.hwndFocus; current; current = GetParent(current)) {
        wchar_t className[128] = {};
        const int count = GetClassNameW(current, className, 127);
        if (count <= 0) {
            continue;
        }
        const QString cls = QString::fromWCharArray(className);
        if (cls == QStringLiteral("Edit") ||
            cls == QStringLiteral("ComboBox") ||
            cls == QStringLiteral("ComboBoxEdit") ||
            cls.startsWith(QStringLiteral("RichEdit")) ||
            cls.contains(QStringLiteral("SearchBox"), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

QString currentExplorerSelection()
{
    const HWND foreground = topLevelWindow(GetForegroundWindow());
    if (!foreground || !isExplorerWindowClass(foreground)) {
        logQuickLook(QStringLiteral("ExplorerSelection: foreground is not Explorer"));
        return {};
    }

    IShellWindows* shellWindows = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_ShellWindows,
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&shellWindows));
    if (FAILED(hr) || !shellWindows) {
        logQuickLook(QStringLiteral("ExplorerSelection: CoCreateInstance(IShellWindows) failed"));
        return {};
    }

    long count = 0;
    hr = shellWindows->get_Count(&count);
    if (FAILED(hr)) {
        logQuickLook(QStringLiteral("ExplorerSelection: get_Count failed"));
        safeRelease(shellWindows);
        return {};
    }

    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;

        IDispatch* dispatch = nullptr;
        hr = shellWindows->Item(index, &dispatch);
        VariantClear(&index);
        if (FAILED(hr) || !dispatch) {
            continue;
        }

        IWebBrowserApp* browser = nullptr;
        hr = dispatch->QueryInterface(IID_PPV_ARGS(&browser));
        safeRelease(dispatch);
        if (FAILED(hr) || !browser) {
            continue;
        }

        SHANDLE_PTR hwndValue = 0;
        hr = browser->get_HWND(&hwndValue);
        if (FAILED(hr)) {
            safeRelease(browser);
            continue;
        }

        const HWND browserHwnd = topLevelWindow(reinterpret_cast<HWND>(hwndValue));
        if (browserHwnd != foreground) {
            safeRelease(browser);
            continue;
        }

        IDispatch* documentDispatch = nullptr;
        hr = browser->get_Document(&documentDispatch);
        safeRelease(browser);
        if (FAILED(hr) || !documentDispatch) {
            logQuickLook(QStringLiteral("ExplorerSelection: get_Document failed for active Explorer"));
            continue;
        }

        IShellFolderViewDual3* folderView = nullptr;
        hr = documentDispatch->QueryInterface(IID_PPV_ARGS(&folderView));
        if (FAILED(hr) || !folderView) {
            IShellFolderViewDual2* folderView2 = nullptr;
            hr = documentDispatch->QueryInterface(IID_PPV_ARGS(&folderView2));
            if (SUCCEEDED(hr) && folderView2) {
                hr = folderView2->QueryInterface(IID_PPV_ARGS(&folderView));
                safeRelease(folderView2);
            }
        }
        safeRelease(documentDispatch);
        if (FAILED(hr) || !folderView) {
            logQuickLook(QStringLiteral("ExplorerSelection: QueryInterface(IShellFolderViewDual3) failed"));
            continue;
        }

        FolderItems* selectedItems = nullptr;
        hr = folderView->SelectedItems(&selectedItems);
        safeRelease(folderView);
        if (FAILED(hr) || !selectedItems) {
            logQuickLook(QStringLiteral("ExplorerSelection: SelectedItems() failed"));
            continue;
        }

        long selectedCount = 0;
        hr = selectedItems->get_Count(&selectedCount);
        if (FAILED(hr) || selectedCount <= 0) {
            safeRelease(selectedItems);
            logQuickLook(QStringLiteral("ExplorerSelection: no selected items"));
            continue;
        }

        VARIANT selectedIndex;
        VariantInit(&selectedIndex);
        selectedIndex.vt = VT_I4;
        selectedIndex.lVal = 0;

        FolderItem* item = nullptr;
        hr = selectedItems->Item(selectedIndex, &item);
        VariantClear(&selectedIndex);
        safeRelease(selectedItems);
        if (FAILED(hr) || !item) {
            logQuickLook(QStringLiteral("ExplorerSelection: FolderItems::Item(0) failed"));
            continue;
        }

        const QString path = folderItemPath(item);
        safeRelease(item);
        if (path.isEmpty()) {
            logQuickLook(QStringLiteral("ExplorerSelection: selected item path empty"));
            continue;
        }

        const QFileInfo info(path);
        if (info.exists() && info.isFile()) {
            safeRelease(shellWindows);
            logQuickLook(QStringLiteral("ExplorerSelection: selected file = %1").arg(info.absoluteFilePath()));
            return info.absoluteFilePath();
        }
    }

    safeRelease(shellWindows);
    logQuickLook(QStringLiteral("ExplorerSelection: no matching Explorer selection found"));
    return {};
}

} // namespace cgplay::quicklook
