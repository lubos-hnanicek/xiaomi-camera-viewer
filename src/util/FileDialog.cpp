#include "util/FileDialog.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <format>

namespace xv {
namespace {

using Microsoft::WRL::ComPtr;

std::string hresult(HRESULT value) {
    return std::format("0x{:08X}", static_cast<uint32_t>(value));
}

} // namespace

std::optional<std::filesystem::path> chooseRecordingFile(
    HWND owner, const std::filesystem::path& initialDirectory, std::string& error) {
    ComPtr<IFileOpenDialog> dialog;
    HRESULT result =
        ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(result)) {
        error = "could not create the file picker: " + hresult(result);
        return std::nullopt;
    }

    constexpr COMDLG_FILTERSPEC filters[] = {
        {L"Matroska recordings (*.mkv)", L"*.mkv"},
        {L"All files (*.*)", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetTitle(L"Open Xiaomi Viewer recording");

    ComPtr<IShellItem> folder;
    if (!initialDirectory.empty() &&
        SUCCEEDED(::SHCreateItemFromParsingName(initialDirectory.c_str(), nullptr,
                                                IID_PPV_ARGS(folder.GetAddressOf())))) {
        dialog->SetFolder(folder.Get());
    }

    result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return std::nullopt;
    }
    if (FAILED(result)) {
        error = "could not show the file picker: " + hresult(result);
        return std::nullopt;
    }

    ComPtr<IShellItem> item;
    result = dialog->GetResult(item.GetAddressOf());
    if (FAILED(result)) {
        error = "could not read the selected file: " + hresult(result);
        return std::nullopt;
    }

    PWSTR selected = nullptr;
    result = item->GetDisplayName(SIGDN_FILESYSPATH, &selected);
    if (FAILED(result) || selected == nullptr) {
        error = "could not read the selected path: " + hresult(result);
        return std::nullopt;
    }

    std::filesystem::path path(selected);
    ::CoTaskMemFree(selected);
    return path;
}

} // namespace xv
