#include "platform.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace liquidcooling::detail {
namespace {

constexpr std::size_t kMaxPathLength = 4096;
constexpr std::size_t kMaxFileBytes = 64u * 1024u * 1024u;

[[nodiscard]] bool is_separator(char c) noexcept { return c == '\\' || c == '/'; }

[[nodiscard]] bool is_reserved_device_name(std::string_view component) {
    static constexpr std::array<std::string_view, 22> kReserved = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    const std::size_t dot = component.find('.');
    const std::string_view stem = dot == std::string_view::npos ? component : component.substr(0, dot);
    for (const auto& reserved : kReserved) {
        if (stem.size() == reserved.size()) {
            bool equal = true;
            for (std::size_t i = 0; i < stem.size(); ++i) {
                const char c = stem[i];
                const char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
                if (upper != reserved[i]) {
                    equal = false;
                    break;
                }
            }
            if (equal) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] Result<std::string> normalise_root(std::string root) {
    if (root.empty()) {
        return Status{StatusCode::StorePathInvalid, "store root must not be empty"};
    }
    if (root.size() > kMaxPathLength) {
        return Status{StatusCode::StorePathInvalid, "store root exceeds the maximum path length"};
    }
    if (root.find('\0') != std::string::npos) {
        return Status{StatusCode::StorePathInvalid, "store root contains an embedded NUL byte"};
    }
    for (char& c : root) {
        if (c == '/') {
            c = '\\';
        }
    }
    // Traversal components are refused before resolution rather than being
    // silently folded away, so an accidental relative escape cannot quietly
    // place a durable store somewhere unintended.
    {
        std::string_view view{root};
        std::size_t start = 0;
        while (start <= view.size()) {
            const std::size_t end = view.find('\\', start);
            const std::string_view component =
                view.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            if (component == "." || component == "..") {
                return Status{StatusCode::StorePathInvalid,
                              "store root must not contain '.' or '..' path components"};
            }
            // Win32 silently strips trailing dots and spaces from a component,
            // so two spellings would otherwise denote one directory but produce
            // two different lock identities.  This is checked on the caller's
            // spelling because the resolution step may normalise them away.
            if (!component.empty() && (component.back() == '.' || component.back() == ' ')) {
                return Status{StatusCode::StorePathInvalid,
                              "store root must not have a path component ending with a dot or space"};
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
    }
    // Reject trailing separators beyond a drive root so that two spellings of
    // the same directory cannot produce different lock identities.
    while (root.size() > 3 && root.back() == '\\') {
        root.pop_back();
    }
    return root;
}

#ifdef _WIN32

[[nodiscard]] Result<std::wstring> to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring{};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return Status{StatusCode::StorePathInvalid, "path is not valid UTF-8"};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                            static_cast<int>(utf8.size()), wide.data(), needed);
    if (written != needed) {
        return Status{StatusCode::StorePathInvalid, "path transcoding failed"};
    }
    return wide;
}

[[nodiscard]] Result<std::string> to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string{};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) {
        return Status{StatusCode::StorePathInvalid, "path transcoding failed"};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                                            needed, nullptr, nullptr);
    if (written != needed) {
        return Status{StatusCode::StorePathInvalid, "path transcoding failed"};
    }
    return utf8;
}

/// Prefixes an already absolute path with the extended-length marker.
[[nodiscard]] std::wstring to_extended(const std::wstring& absolute) {
    if (absolute.rfind(L"\\\\?\\", 0) == 0) {
        return absolute;
    }
    if (absolute.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + absolute.substr(2);
    }
    return L"\\\\?\\" + absolute;
}

[[nodiscard]] Status last_error_status(std::string_view what) {
    const DWORD code = GetLastError();
    return Status{StatusCode::StoreIoError, std::string(what) + " failed with Win32 error " + std::to_string(code)};
}

[[nodiscard]] Result<std::wstring> absolute_wide(const std::string& path) {
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    const DWORD needed = GetFullPathNameW(wide.value().c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return last_error_status("GetFullPathNameW");
    }
    std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = GetFullPathNameW(wide.value().c_str(), needed, buffer.data(), nullptr);
    if (written == 0 || written >= needed) {
        return last_error_status("GetFullPathNameW");
    }
    buffer.resize(static_cast<std::size_t>(written));
    while (buffer.size() > 3 && buffer.back() == L'\\') {
        buffer.pop_back();
    }
    return buffer;
}

[[nodiscard]] std::string lowercase_ascii(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

#else  // POSIX

[[nodiscard]] Result<std::string> absolute_posix(const std::string& path) {
    if (!path.empty() && path.front() == '/') {
        return path;
    }
    std::array<char, kMaxPathLength> buffer{};
    if (getcwd(buffer.data(), buffer.size()) == nullptr) {
        return Status{StatusCode::StoreIoError, "getcwd failed"};
    }
    std::string combined{buffer.data()};
    combined.push_back('/');
    combined.append(path);
    return combined;
}

#endif

}  // namespace

Result<std::string> validate_component(std::string_view name) {
    if (name.empty()) {
        return Status{StatusCode::StorePathInvalid, "path component must not be empty"};
    }
    if (name.size() > 255) {
        return Status{StatusCode::StorePathInvalid, "path component exceeds 255 bytes"};
    }
    if (name == "." || name == "..") {
        return Status{StatusCode::StorePathInvalid, "relative path components are not permitted"};
    }
    for (const char c : name) {
        if (c == '\0') {
            return Status{StatusCode::StorePathInvalid, "path component contains an embedded NUL byte"};
        }
        if (c == '\\' || c == '/' || c == ':') {
            return Status{StatusCode::StorePathInvalid, "path component contains a separator or colon"};
        }
        if (static_cast<unsigned char>(c) < 0x20u) {
            return Status{StatusCode::StorePathInvalid, "path component contains a control character"};
        }
    }
    if (name.back() == '.' || name.back() == ' ') {
        return Status{StatusCode::StorePathInvalid,
                      "path component ends with a dot or space, which is ambiguous on some file systems"};
    }
    if (is_reserved_device_name(name)) {
        return Status{StatusCode::StorePathInvalid, "path component is a reserved device name"};
    }
    return std::string(name);
}

Result<CanonicalPath> CanonicalPath::create(const std::string& root, bool create_if_missing) {
    const auto normalised = normalise_root(root);
    if (!normalised.ok()) {
        return normalised.status();
    }
#ifdef _WIN32
    const auto absolute = absolute_wide(normalised.value());
    if (!absolute.ok()) {
        return absolute.status();
    }
    const std::wstring& wide = absolute.value();
    if (wide.size() < 3 || wide[1] != L':' || wide[2] != L'\\') {
        return Status{StatusCode::StorePathInvalid, "store root must be an absolute drive or UNC path"};
    }
    // Reject ambiguous components that Win32 silently rewrites.  The drive
    // designator and the UNC server/share prefix are path roots, not components.
    const bool unc = wide.size() >= 2 && wide[0] == L'\\' && wide[1] == L'\\';
    std::size_t start = 0;
    std::size_t root_components = unc ? 3u : 1u;
    std::size_t component_index = 0;
    while (start < wide.size()) {
        std::size_t end = wide.find(L'\\', start);
        if (end == std::wstring::npos) {
            end = wide.size();
        }
        const std::wstring component = wide.substr(start, end - start);
        const bool is_root = component_index < root_components;
        ++component_index;
        if (!is_root && component.size() >= 2) {
            const auto utf8 = to_utf8(component);
            if (utf8.ok() && utf8.value() != "." && utf8.value() != "..") {
                const auto validated = validate_component(utf8.value());
                if (!validated.ok()) {
                    return validated.status();
                }
            }
        }
        start = end + 1;
    }
    const std::wstring extended = to_extended(wide);
    const auto display_utf8 = to_utf8(wide);
    if (!display_utf8.ok()) {
        return display_utf8.status();
    }
    const DWORD attributes = GetFileAttributesW(extended.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (!create_if_missing) {
            return Status{StatusCode::StoreMissing, "store root does not exist"};
        }
        const auto created = create_directories(display_utf8.value());
        if (!created.ok()) {
            return created.status();
        }
    } else {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return Status{StatusCode::StorePathInvalid, "store root exists but is not a directory"};
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return Status{StatusCode::StorePathInvalid,
                          "store root is a reparse point (symlink or junction), which is not permitted"};
        }
    }
    CanonicalPath path;
    path.display_ = display_utf8.value();
    path.identity_ = lowercase_ascii(display_utf8.value());
    return path;
#else
    const auto absolute = absolute_posix(normalised.value());
    if (!absolute.ok()) {
        return absolute.status();
    }
    struct stat info {};
    if (::stat(absolute.value().c_str(), &info) != 0) {
        if (!create_if_missing) {
            return Status{StatusCode::StoreMissing, "store root does not exist"};
        }
        const auto created = create_directories(absolute.value());
        if (!created.ok()) {
            return created.status();
        }
    } else {
        if (!S_ISDIR(info.st_mode)) {
            return Status{StatusCode::StorePathInvalid, "store root exists but is not a directory"};
        }
        if (S_ISLNK(info.st_mode)) {
            return Status{StatusCode::StorePathInvalid,
                          "store root is a symbolic link, which is not permitted"};
        }
    }
    CanonicalPath path;
    path.display_ = absolute.value();
    path.identity_ = absolute.value();
    return path;
#endif
}

Result<std::string> CanonicalPath::child(std::string_view name) const {
    const auto validated = validate_component(name);
    if (!validated.ok()) {
        return validated.status();
    }
    std::string out = display_;
    if (out.empty()) {
        return Status{StatusCode::StoreNotOpen, "canonical path is not initialised"};
    }
    if (out.back() != '\\') {
        out.push_back('\\');
    }
    out.append(validated.value());
    return out;
}

Result<bool> path_exists(const std::string& path) {
#ifdef _WIN32
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    const DWORD attributes = GetFileAttributesW(to_extended(wide.value()).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        return last_error_status("GetFileAttributesW");
    }
    return true;
#else
    struct stat info {};
    if (::stat(path.c_str(), &info) == 0) {
        return true;
    }
    if (errno == ENOENT) {
        return false;
    }
    return Status{StatusCode::StoreIoError, "stat failed"};
#endif
}

Result<bool> is_directory(const std::string& path) {
#ifdef _WIN32
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    const DWORD attributes = GetFileAttributesW(to_extended(wide.value()).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        return last_error_status("GetFileAttributesW");
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return false;
    }
    return S_ISDIR(info.st_mode);
#endif
}

Result<bool> is_reparse_point(const std::string& path) {
#ifdef _WIN32
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    const DWORD attributes = GetFileAttributesW(to_extended(wide.value()).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        return last_error_status("GetFileAttributesW");
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        return false;
    }
    return S_ISLNK(info.st_mode);
#endif
}

Result<void> create_directories(const std::string& path) {
    const auto exists = is_directory(path);
    if (!exists.ok()) {
        return exists.status();
    }
    if (exists.value()) {
        return ok_result();
    }
    std::size_t position = 0;
    while (position < path.size()) {
        position = path.find('\\', position + 1);
        if (position == std::string::npos) {
            break;
        }
        if (position == 0 || (position == 2 && path[1] == ':')) {
            continue;
        }
        const std::string prefix = path.substr(0, position);
#ifdef _WIN32
        const auto wide = to_wide(prefix);
        if (!wide.ok()) {
            return wide.status();
        }
        if (!CreateDirectoryW(to_extended(wide.value()).c_str(), nullptr)) {
            const DWORD code = GetLastError();
            if (code != ERROR_ALREADY_EXISTS) {
                return last_error_status("CreateDirectoryW");
            }
        }
#else
        if (::mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST) {
            return Status{StatusCode::StoreIoError, "mkdir failed"};
        }
#endif
    }
#ifdef _WIN32
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    if (!CreateDirectoryW(to_extended(wide.value()).c_str(), nullptr)) {
        const DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS) {
            return last_error_status("CreateDirectoryW");
        }
    }
#else
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        return Status{StatusCode::StoreIoError, "mkdir failed"};
    }
#endif
    return ok_result();
}

Result<std::vector<std::string>> list_directory(const std::string& path, std::size_t max_entries) {
    std::vector<std::string> entries;
#ifdef _WIN32
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    std::wstring pattern = to_extended(wide.value());
    if (pattern.back() != L'\\') {
        pattern.push_back(L'\\');
    }
    pattern.push_back(L'*');
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            return entries;
        }
        return last_error_status("FindFirstFileW");
    }
    for (;;) {
        const std::wstring name = data.cFileName;
        if (name != L"." && name != L"..") {
            const auto utf8 = to_utf8(name);
            if (!utf8.ok()) {
                FindClose(handle);
                return utf8.status();
            }
            if (entries.size() >= max_entries) {
                FindClose(handle);
                return Status{StatusCode::StoreBoundsExceeded, "directory listing exceeded the configured bound"};
            }
            entries.push_back(utf8.value());
        }
        if (FindNextFileW(handle, &data) == 0) {
            break;
        }
    }
    FindClose(handle);
    return entries;
#else
    DIR* directory = ::opendir(path.c_str());
    if (directory == nullptr) {
        return Status{StatusCode::StoreIoError, "opendir failed"};
    }
    while (const dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (entries.size() >= max_entries) {
            ::closedir(directory);
            return Status{StatusCode::StoreBoundsExceeded, "directory listing exceeded the configured bound"};
        }
        entries.push_back(name);
    }
    ::closedir(directory);
    return entries;
#endif
}

Result<std::vector<std::uint8_t>> read_file(const std::string& path, std::size_t max_bytes) {
#ifdef _WIN32
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    // Readers must never block the atomic replace that publishes a new
    // generation, so the file is opened sharing read, write and delete.  The
    // handle keeps referring to the generation that existed when it was opened,
    // so a concurrent publication cannot produce a mixed read.
    HANDLE handle = CreateFileW(to_extended(wide.value()).c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return Status{StatusCode::StoreMissing, "store file does not exist"};
        }
        if (code == ERROR_SHARING_VIOLATION) {
            return Status{StatusCode::StoreLocked, "store file is exclusively held by another process"};
        }
        return last_error_status("CreateFileW");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        CloseHandle(handle);
        return last_error_status("GetFileInformationByHandle");
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        return Status{StatusCode::StorePathInvalid, "store file is a reparse point"};
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        CloseHandle(handle);
        return last_error_status("GetFileSizeEx");
    }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > static_cast<std::uint64_t>(max_bytes)) {
        CloseHandle(handle);
        return Status{StatusCode::StoreBoundsExceeded, "store file exceeds the configured maximum size"};
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < data.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(handle, data.data() + offset, request, &read, nullptr)) {
            CloseHandle(handle);
            return last_error_status("ReadFile");
        }
        if (read == 0) {
            CloseHandle(handle);
            return Status{StatusCode::StoreCorrupt, "store file was truncated while being read"};
        }
        offset += read;
    }
    CloseHandle(handle);
    return data;
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Status{errno == ENOENT ? StatusCode::StoreMissing : StatusCode::StoreIoError, "open failed"};
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        ::close(fd);
        return Status{StatusCode::StoreIoError, "fstat failed"};
    }
    if (S_ISLNK(info.st_mode)) {
        ::close(fd);
        return Status{StatusCode::StorePathInvalid, "store file is a symbolic link"};
    }
    if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > static_cast<std::uint64_t>(max_bytes)) {
        ::close(fd);
        return Status{StatusCode::StoreBoundsExceeded, "store file exceeds the configured maximum size"};
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t got = ::read(fd, data.data() + offset, data.size() - offset);
        if (got <= 0) {
            ::close(fd);
            return Status{StatusCode::StoreCorrupt, "store file was truncated while being read"};
        }
        offset += static_cast<std::size_t>(got);
    }
    ::close(fd);
    return data;
#endif
}

Result<void> write_file_durable(const std::string& path, std::span<const std::uint8_t> data) {
#ifdef _WIN32
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    HANDLE handle = CreateFileW(to_extended(wide.value()).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return last_error_status("CreateFileW");
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(handle, data.data() + offset, request, &written, nullptr)) {
            CloseHandle(handle);
            return last_error_status("WriteFile");
        }
        if (written == 0) {
            CloseHandle(handle);
            return Status{StatusCode::StoreIoError, "WriteFile wrote no bytes"};
        }
        offset += written;
    }
    if (!FlushFileBuffers(handle)) {
        CloseHandle(handle);
        return last_error_status("FlushFileBuffers");
    }
    // Read-back verification: the bytes accepted by the file system must match
    // exactly what was requested before the file is treated as publishable.
    LARGE_INTEGER origin{};
    if (!SetFilePointerEx(handle, origin, nullptr, FILE_BEGIN)) {
        CloseHandle(handle);
        return last_error_status("SetFilePointerEx");
    }
    std::vector<std::uint8_t> verify(std::min<std::size_t>(data.size(), 1u << 16));
    std::size_t verified = 0;
    while (verified < data.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(data.size() - verified, verify.size()));
        DWORD read = 0;
        if (!ReadFile(handle, verify.data(), request, &read, nullptr)) {
            CloseHandle(handle);
            return last_error_status("ReadFile (verify)");
        }
        if (read == 0 || std::memcmp(verify.data(), data.data() + verified, read) != 0) {
            CloseHandle(handle);
            return Status{StatusCode::StoreIoError, "staged store file failed read-back verification"};
        }
        verified += read;
    }
    CloseHandle(handle);
    return ok_result();
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return Status{StatusCode::StoreIoError, "open for write failed"};
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written <= 0) {
            ::close(fd);
            return Status{StatusCode::StoreIoError, "write failed"};
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        return Status{StatusCode::StoreIoError, "fsync failed"};
    }
    std::vector<std::uint8_t> verify(std::min<std::size_t>(data.size(), 1u << 16));
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        ::close(fd);
        return Status{StatusCode::StoreIoError, "lseek failed"};
    }
    std::size_t verified = 0;
    while (verified < data.size()) {
        const ssize_t got = ::read(fd, verify.data(), std::min<std::size_t>(data.size() - verified, verify.size()));
        if (got <= 0 || std::memcmp(verify.data(), data.data() + verified, static_cast<std::size_t>(got)) != 0) {
            ::close(fd);
            return Status{StatusCode::StoreIoError, "staged store file failed read-back verification"};
        }
        verified += static_cast<std::size_t>(got);
    }
    ::close(fd);
    return ok_result();
#endif
}

Result<void> write_file_plain(const std::string& path, std::span<const std::uint8_t> data) {
#ifdef _WIN32
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    HANDLE handle = CreateFileW(to_extended(wide.value()).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return last_error_status("CreateFileW");
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(handle, data.data() + offset, request, &written, nullptr) || written == 0) {
            CloseHandle(handle);
            return last_error_status("WriteFile");
        }
        offset += written;
    }
    if (!FlushFileBuffers(handle)) {
        CloseHandle(handle);
        return last_error_status("FlushFileBuffers");
    }
    CloseHandle(handle);
    return ok_result();
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return Status{StatusCode::StoreIoError, "open for write failed"};
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written <= 0) {
            ::close(fd);
            return Status{StatusCode::StoreIoError, "write failed"};
        }
        offset += static_cast<std::size_t>(written);
    }
    ::fsync(fd);
    ::close(fd);
    return ok_result();
#endif
}

Result<void> replace_file(const std::string& src, const std::string& dst) {
#ifdef _WIN32
    const auto src_wide = to_wide(src);
    if (!src_wide.ok()) {
        return src_wide.status();
    }
    const auto dst_wide = to_wide(dst);
    if (!dst_wide.ok()) {
        return dst_wide.status();
    }
    // A rename that replaces an existing file can be denied transiently while
    // another component (an indexer, a scanner, a backup agent) holds a share
    // mode that forbids deletion.  Retrying a bounded number of times does not
    // weaken atomicity: each attempt is still an all-or-nothing replacement.
    constexpr int kMaxAttempts = 40;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (MoveFileExW(to_extended(src_wide.value()).c_str(), to_extended(dst_wide.value()).c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return ok_result();
        }
        const DWORD code = GetLastError();
        if (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION) {
            return last_error_status("MoveFileExW");
        }
        if (attempt + 1 == kMaxAttempts) {
            return last_error_status("MoveFileExW");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return last_error_status("MoveFileExW");
#else
    if (::rename(src.c_str(), dst.c_str()) != 0) {
        return Status{StatusCode::StoreIoError, "rename failed"};
    }
    const int dir = ::open(".", O_RDONLY);
    if (dir >= 0) {
        ::fsync(dir);
        ::close(dir);
    }
    return ok_result();
#endif
}

Result<void> remove_file(const std::string& path) {
#ifdef _WIN32
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    if (!DeleteFileW(to_extended(wide.value()).c_str())) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return ok_result();
        }
        return last_error_status("DeleteFileW");
    }
    return ok_result();
#else
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        return Status{StatusCode::StoreIoError, "unlink failed"};
    }
    return ok_result();
#endif
}

Result<std::uint64_t> file_size(const std::string& path) {
#ifdef _WIN32
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(to_extended(wide.value()).c_str(), GetFileExInfoStandard, &data)) {
        return last_error_status("GetFileAttributesExW");
    }
    return static_cast<std::uint64_t>(data.nFileSizeHigh) << 32u | static_cast<std::uint64_t>(data.nFileSizeLow);
#else
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return Status{StatusCode::StoreIoError, "stat failed"};
    }
    return static_cast<std::uint64_t>(info.st_size);
#endif
}

Result<std::int64_t> file_modified_unix_nanos(const std::string& path) {
#ifdef _WIN32
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(to_extended(wide.value()).c_str(), GetFileExInfoStandard, &data)) {
        return last_error_status("GetFileAttributesExW");
    }
    ULARGE_INTEGER stamp{};
    stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
    stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
    constexpr std::uint64_t kUnixEpochInFiletime = 116444736000000000ull;
    if (stamp.QuadPart < kUnixEpochInFiletime) {
        return std::int64_t{0};
    }
    return static_cast<std::int64_t>((stamp.QuadPart - kUnixEpochInFiletime) * 100ull);
#else
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return Status{StatusCode::StoreIoError, "stat failed"};
    }
    return static_cast<std::int64_t>(info.st_mtime) * 1'000'000'000;
#endif
}

ExclusiveLock::ExclusiveLock(ExclusiveLock&& other) noexcept : handle_(other.handle_) { other.handle_ = 0; }

ExclusiveLock& ExclusiveLock::operator=(ExclusiveLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = other.handle_;
        other.handle_ = 0;
    }
    return *this;
}

ExclusiveLock::~ExclusiveLock() { release(); }

void ExclusiveLock::release() noexcept {
    if (handle_ == 0) {
        return;
    }
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    ::flock(static_cast<int>(handle_), LOCK_UN);
    ::close(static_cast<int>(handle_));
#endif
    handle_ = 0;
}

bool ExclusiveLock::held() const noexcept { return handle_ != 0; }

Result<ExclusiveLock> ExclusiveLock::acquire(const std::string& path, std::string_view owner_note) {
#ifdef _WIN32
    const auto wide = to_wide(path);
    if (!wide.ok()) {
        return wide.status();
    }
    // No sharing at all: a second process cannot even open the file, which makes
    // the exclusion independent of advisory-lock semantics.
    HANDLE handle = CreateFileW(to_extended(wide.value()).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
            return Status{StatusCode::StoreLocked, "another process holds the exclusive store lock"};
        }
        return last_error_status("CreateFileW (lock)");
    }
    ExclusiveLock lock;
    lock.handle_ = reinterpret_cast<std::uintptr_t>(handle);
    if (!owner_note.empty()) {
        const char* note = owner_note.data();
        const DWORD length = static_cast<DWORD>(owner_note.size());
        DWORD written = 0;
        SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
        if (WriteFile(handle, note, length, &written, nullptr)) {
            SetEndOfFile(handle);
            FlushFileBuffers(handle);
        }
    }
    return lock;
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return Status{StatusCode::StoreIoError, "open lock file failed"};
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return Status{StatusCode::StoreLocked, "another process holds the exclusive store lock"};
    }
    ExclusiveLock lock;
    lock.handle_ = static_cast<std::uintptr_t>(fd);
    if (!owner_note.empty()) {
        if (::ftruncate(fd, 0) == 0) {
            ssize_t ignored = ::write(fd, owner_note.data(), owner_note.size());
            (void)ignored;
            ::fsync(fd);
        }
    }
    return lock;
#endif
}

Result<std::string> ExclusiveLock::read_owner_note(const std::string& path) const {
    if (!held()) {
        return Status{StatusCode::StoreNotOpen, "lock is not held"};
    }
    const auto data = read_file(path, 4096);
    if (!data.ok()) {
        return data.status();
    }
    std::string note;
    note.reserve(data.value().size());
    for (const std::uint8_t byte : data.value()) {
        if (byte == 0) {
            break;
        }
        note.push_back(static_cast<char>(byte));
    }
    return note;
}

unsigned long current_process_id() noexcept {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

std::string staging_suffix() {
#ifdef _WIN32
    const auto pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t value = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    return std::to_string(pid) + "-" + std::to_string(value) + ".tmp";
}

}  // namespace liquidcooling::detail
