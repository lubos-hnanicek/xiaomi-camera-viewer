#include "util/Encoding.h"

#include <windows.h>

#include <wincrypt.h>

#include <cstring>

namespace xv::encoding {

std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
        } else {
            result.append(backslashes * (character == L'"' ? 2 : 1), L'\\');
            backslashes = 0;
            if (character == L'"') {
                result += L'\\';
            }
            result += character;
        }
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}

std::wstring createProcessCommand(const std::wstring& executable,
                                  const std::wstring& parameters) {
    return quoteArgument(executable) + L" " + parameters;
}

std::string base64Encode(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return {};
    }

    DWORD length = 0;
    if (!::CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()),
                                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length)) {
        return {};
    }

    std::string out(length, '\0');
    if (!::CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()),
                                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &length)) {
        return {};
    }

    // The reported length includes the terminator this API always appends.
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::vector<uint8_t> base64Decode(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    DWORD length = 0;
    if (!::CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                                nullptr, &length, nullptr, nullptr)) {
        return {};
    }

    std::vector<uint8_t> out(length);
    if (!::CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                                out.data(), &length, nullptr, nullptr)) {
        return {};
    }

    out.resize(length);
    return out;
}

} // namespace xv::encoding
