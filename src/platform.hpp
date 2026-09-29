#pragma once

// Internal platform layer: canonical store paths, hardened file I/O and the
// cross-process exclusive store lock.
//
// Every path handled here is already canonical: absolute, backslash separated,
// free of "." / ".." / trailing dot or space components and free of reserved
// device names.  Windows file operations use the extended-length prefix so that
// the canonical form is never re-interpreted by the Win32 path parser.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "liquidcooling/status.hpp"

namespace liquidcooling::detail {

/// Canonical, hardened representation of a store root directory.
class CanonicalPath final {
public:
    CanonicalPath() = default;

    /// Validates and canonicalises an existing or creatable directory path.
    [[nodiscard]] static Result<CanonicalPath> create(const std::string& root, bool create_if_missing);

    [[nodiscard]] const std::string& display() const noexcept { return display_; }
    [[nodiscard]] const std::string& identity() const noexcept { return identity_; }
    [[nodiscard]] bool valid() const noexcept { return !identity_.empty(); }

    /// Joins a single validated file name onto the canonical root.
    [[nodiscard]] Result<std::string> child(std::string_view name) const;

private:
    std::string display_{};
    std::string identity_{};
};

/// Validates one file name component for use inside the store root.
[[nodiscard]] Result<std::string> validate_component(std::string_view name);

// --- File system ------------------------------------------------------------

[[nodiscard]] Result<bool> path_exists(const std::string& path);
[[nodiscard]] Result<bool> is_directory(const std::string& path);
[[nodiscard]] Result<bool> is_reparse_point(const std::string& path);
[[nodiscard]] Result<void> create_directories(const std::string& path);

/// Lists the file names directly inside a directory (bounded).
[[nodiscard]] Result<std::vector<std::string>> list_directory(const std::string& path, std::size_t max_entries);

/// Reads a whole file, refusing anything larger than max_bytes.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const std::string& path, std::size_t max_bytes);

/// Writes, flushes and reads back a file, verifying the bytes round trip.
[[nodiscard]] Result<void> write_file_durable(const std::string& path, std::span<const std::uint8_t> data);

/// As write_file_durable, but does not verify by reading back (used for the
/// superseded-generation copy, whose integrity is checked by CRC on load).
[[nodiscard]] Result<void> write_file_plain(const std::string& path, std::span<const std::uint8_t> data);

/// Atomically replaces dst with src.  The commit point of a store publication.
[[nodiscard]] Result<void> replace_file(const std::string& src, const std::string& dst);

[[nodiscard]] Result<void> remove_file(const std::string& path);

[[nodiscard]] Result<std::uint64_t> file_size(const std::string& path);

/// The last write timestamp of a file, in nanoseconds since the Unix epoch.
/// Zero when the platform cannot report it.  Used only for diagnostics.
[[nodiscard]] Result<std::int64_t> file_modified_unix_nanos(const std::string& path);

// --- Cross-process exclusion ------------------------------------------------

/// An OS-level exclusive lock held for the lifetime of an open store.
class ExclusiveLock final {
public:
    ExclusiveLock() = default;
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
    ExclusiveLock(ExclusiveLock&& other) noexcept;
    ExclusiveLock& operator=(ExclusiveLock&& other) noexcept;
    ~ExclusiveLock();

    /// Acquires the lock, writing an owner note into the lock file.
    [[nodiscard]] static Result<ExclusiveLock> acquire(const std::string& path, std::string_view owner_note);

    [[nodiscard]] bool held() const noexcept;

    /// Reads the owner note written by the process holding the lock.
    [[nodiscard]] Result<std::string> read_owner_note(const std::string& path) const;

private:
    void release() noexcept;

    std::uintptr_t handle_{0};
};

/// Generates a process-unique staging suffix.
[[nodiscard]] std::string staging_suffix();

/// Identifier of the current operating-system process.
[[nodiscard]] unsigned long current_process_id() noexcept;

}  // namespace liquidcooling::detail
