// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// fileio.h — files named by UTF-8 paths on every system, and saving without ever losing the old file
//
// Paths in Moritz are UTF-8 strings. On macOS and Linux they go to the system as they are; on Windows a
// narrow std::ifstream or fopen would read them in the ANSI code page, so names with accents or other
// scripts would fail: every file is opened here, through the wide-character API there.
//
// writeAtomic() is how a document is saved: the text goes into a temporary file in the same folder,
// which is flushed, synced and closed with every step checked, and only then renamed over the old file.
// A full disk, a quota or a crash in the middle leaves the old file as it was.
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace moritz {
namespace fileio {

#ifdef _WIN32
inline std::wstring wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
#endif

// fopen with a UTF-8 path
inline std::FILE* open(const std::string& path, const char* mode) {
#ifdef _WIN32
    return _wfopen(wide(path).c_str(), wide(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// the whole file, as bytes
inline bool read(const std::string& path, std::string& out) {
    std::FILE* f = open(path, "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

// a file or a folder by that name
inline bool exists(const std::string& path) {
#ifdef _WIN32
    return GetFileAttributesW(wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
#endif
}

// a regular file (not a folder)
inline bool isFile(const std::string& path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

inline bool remove(const std::string& path) {
#ifdef _WIN32
    return DeleteFileW(wide(path).c_str()) != 0;
#else
    return ::unlink(path.c_str()) == 0;
#endif
}

// Writes `data` into `path` through a temporary file in the same folder, renamed over `path` once it is
// complete on the disk. On failure `path` is untouched, the temporary file is removed and err says why.
// A symbolic link is followed: the file it points to is replaced, the link stays.
inline bool writeAtomic(const std::string& pathIn, const std::string& data, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    std::string path = pathIn;
#ifndef _WIN32
    {
        char real[PATH_MAX];
        struct stat lst;
        if (::lstat(path.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode) && ::realpath(path.c_str(), real)) path = real;
    }
#endif
    size_t slash = path.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash == 0 ? 1 : slash);
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::random_device rd;
    std::string tmp;
    std::FILE* f = nullptr;
    for (int attempt = 0; attempt < 20 && !f; ++attempt) {
        char suffix[16];
        std::snprintf(suffix, sizeof suffix, "%08x", (unsigned)rd());
        tmp = dir + "/." + name + "." + suffix + ".tmp";
#ifdef _WIN32
        HANDLE h = CreateFileW(wide(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return fail("can't write in " + dir);
        }
        int fd = _open_osfhandle((intptr_t)h, 0);
        if (fd < 0) { CloseHandle(h); return fail("can't write in " + dir); }
        f = _fdopen(fd, "wb");
        if (!f) { _close(fd); fileio::remove(tmp); return fail("can't write in " + dir); }
#else
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);   // the umask applies
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return fail("can't write in " + dir + ": " + std::strerror(errno));
        }
        struct stat st;                                 // the saved file keeps the old one's permissions
        if (::stat(path.c_str(), &st) == 0) (void)::fchmod(fd, st.st_mode & 07777);
        f = ::fdopen(fd, "wb");
        if (!f) { ::close(fd); fileio::remove(tmp); return fail("can't write in " + dir); }
#endif
    }
    if (!f) return fail("can't make a temporary file in " + dir);
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    int why = ok ? 0 : errno;
    ok = std::fflush(f) == 0 && ok;
    if (!ok && !why) why = errno;
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && ::fsync(fileno(f)) == 0;
#endif
    if (!ok && !why) why = errno;
    if (std::fclose(f) != 0 && ok) { ok = false; why = errno; }
    if (!ok) {
        fileio::remove(tmp);
        return fail("can't write " + path + (why ? std::string(": ") + std::strerror(why) : std::string()));
    }
#ifdef _WIN32
    if (!MoveFileExW(wide(tmp).c_str(), wide(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fileio::remove(tmp);
        return fail("can't replace " + path);
    }
#else
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        int e = errno;
        fileio::remove(tmp);
        return fail("can't replace " + path + ": " + std::strerror(e));
    }
#endif
    return true;
}

// an absolute path (the current folder in front of a relative one), with forward slashes on Windows
inline std::string absolute(const std::string& path) {
    if (path.empty()) return path;
#ifdef _WIN32
    std::wstring w = wide(path);
    wchar_t buf[4096];
    DWORD n = GetFullPathNameW(w.c_str(), 4096, buf, nullptr);
    if (n == 0 || n >= 4096) return path;
    std::string s = narrow(std::wstring(buf, n));
    for (auto& c : s) if (c == '\\') c = '/';
    return s;
#else
    if (path[0] == '/') return path;
    char cwd[PATH_MAX];
    if (!::getcwd(cwd, sizeof cwd)) return path;
    std::string p = path;
    while (p.rfind("./", 0) == 0) p = p.substr(2);
    return std::string(cwd) + "/" + p;
#endif
}

}  // namespace fileio
}  // namespace moritz
