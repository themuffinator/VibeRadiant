/*
    Copyright (C) 1996-1997  Id Software, Inc.
    Copyright (C) 1997       Greg Lewis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

#include "common/cmdlib.hh"
#include "common/fs.hh"
#include "common/log.hh"
#include <algorithm>
#include <fstream>
#include <memory>
#include <array>
#include <list>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

namespace fs
{
namespace
{
bool range_within_file(uint64_t offset, uint64_t length, uint64_t file_size)
{
    return offset <= file_size && length <= (file_size - offset);
}

std::optional<uint64_t> stream_size(std::ifstream &stream)
{
    stream.clear();
    stream.seekg(0, std::ios_base::end);
    const std::streampos end = stream.tellg();
    if (end < 0) {
        return std::nullopt;
    }

    stream.seekg(0, std::ios_base::beg);
    if (!stream) {
        return std::nullopt;
    }

    return static_cast<uint64_t>(end);
}

data read_range(std::ifstream &stream, uint64_t offset, uint64_t length, uint64_t file_size)
{
    if (!range_within_file(offset, length, file_size) ||
        offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::nullopt;
    }

    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios_base::beg);
    if (!stream) {
        return std::nullopt;
    }

    std::vector<uint8_t> result(static_cast<size_t>(length));
    if (length != 0) {
        const auto requested = static_cast<std::streamsize>(length);
        stream.read(reinterpret_cast<char *>(result.data()), requested);
        if (stream.gcount() != requested || !stream) {
            return std::nullopt;
        }
    }

    return result;
}
} // namespace

struct directory_archive : archive_like
{
    using archive_like::archive_like;

    bool contains(const path &filename) override
    {
        std::error_code ec;
        return is_regular_file(!pathname.empty() ? (pathname / filename) : filename, ec);
    }

    data load(const path &filename) override
    {
        path p = !pathname.empty() ? (pathname / filename) : filename;

        std::error_code ec;
        if (!is_regular_file(p, ec)) {
            return std::nullopt;
        }

        std::ifstream stream(p, std::ios_base::in | std::ios_base::binary);
        if (!stream) {
            logging::funcprint("WARNING: unable to open '{}'\n", p);
            return std::nullopt;
        }

        const auto size = stream_size(stream);
        if (!size) {
            logging::funcprint("WARNING: unable to determine size of '{}'\n", p);
            return std::nullopt;
        }

        return read_range(stream, 0, *size, *size);
    }
};

struct pak_archive : archive_like
{
    std::ifstream pakstream;
    std::mutex stream_mutex;
    uint64_t file_size = 0;

    struct pak_header
    {
        std::array<char, 4> magic;
        uint32_t offset;
        uint32_t size;

        auto stream_data() { return std::tie(magic, offset, size); }
    };

    struct pak_file
    {
        std::array<char, 56> name;
        uint32_t offset;
        uint32_t size;

        auto stream_data() { return std::tie(name, offset, size); }
    };

    std::unordered_map<std::string, std::tuple<uint32_t, uint32_t>, case_insensitive_hash, case_insensitive_equal>
        files;

    inline pak_archive(const path &pathname, bool external)
        : archive_like(pathname, external),
          pakstream(pathname, std::ios_base::in | std::ios_base::binary)
    {
        if (!pakstream) {
            throw std::runtime_error("Unable to open PAK");
        }

        const auto size = stream_size(pakstream);
        if (!size) {
            throw std::runtime_error("Unable to determine PAK size");
        }
        file_size = *size;

        pakstream >> endianness<std::endian::little>;

        pak_header header{};

        pakstream >= header;

        if (!pakstream) {
            throw std::runtime_error("Truncated PAK header");
        }

        if (header.magic != std::array<char, 4>{'P', 'A', 'C', 'K'}) {
            throw std::runtime_error("Bad magic");
        }

        if ((header.size % sizeof(pak_file)) != 0) {
            throw std::runtime_error("PAK directory size is not a multiple of its entry size");
        }
        if (!range_within_file(header.offset, header.size, file_size)) {
            throw std::runtime_error("PAK directory is outside the file");
        }

        size_t totalFiles = header.size / sizeof(pak_file);

        files.reserve(totalFiles);

        pakstream.seekg(header.offset);
        if (!pakstream) {
            throw std::runtime_error("Unable to seek to PAK directory");
        }

        for (size_t i = 0; i < totalFiles; i++) {
            pak_file file{};

            pakstream >= file;

            if (!pakstream) {
                throw std::runtime_error("Truncated PAK directory");
            }
            if (!range_within_file(file.offset, file.size, file_size)) {
                throw std::runtime_error("PAK entry is outside the file");
            }

            const auto name_end = std::find(file.name.begin(), file.name.end(), '\0');
            const std::string name(file.name.begin(), name_end);
            files[name] = std::make_tuple(file.offset, file.size);
        }
    }

    bool contains(const path &filename) override { return files.find(filename.generic_string()) != files.end(); }

    data load(const path &filename) override
    {
        auto it = files.find(filename.generic_string());

        if (it == files.end()) {
            return std::nullopt;
        }

        std::lock_guard lock(stream_mutex);
        return read_range(pakstream, std::get<0>(it->second), std::get<1>(it->second), file_size);
    }
};

struct wad_archive : archive_like
{
    std::ifstream wadstream;
    std::mutex stream_mutex;
    uint64_t file_size = 0;

    // WAD Format
    struct wad_header
    {
        std::array<char, 4> identification;
        uint32_t numlumps;
        uint32_t infotableofs;

        auto stream_data() { return std::tie(identification, numlumps, infotableofs); }
    };

    static constexpr std::array<char, 4> wad2_ident = {'W', 'A', 'D', '2'};
    static constexpr std::array<char, 4> wad3_ident = {'W', 'A', 'D', '3'};

    struct wad_lump_header
    {
        uint32_t filepos;
        uint32_t disksize;
        uint32_t size; // uncompressed
        uint8_t type;
        uint8_t compression;
        padding<2> pad;
        // Fixed-width name. Textures using all 16 bytes without a terminator
        // exist in the wild, e.g. openquartzmirror in free_wad.wad.
        std::array<char, 16> name;

        auto stream_data() { return std::tie(filepos, disksize, size, type, compression, pad, name); }

        std::string name_as_string() const
        {
            size_t length = 0;

            // count the number of leading non-null characters
            for (int i = 0; i < 16; ++i) {
                if (name[i] != 0)
                    ++length;
                else
                    break;
            }

            return std::string(name.data(), length);
        }
    };

    std::unordered_map<std::string, std::tuple<uint32_t, uint32_t>, case_insensitive_hash, case_insensitive_equal>
        files;

    inline wad_archive(const path &pathname, bool external)
        : archive_like(pathname, external),
          wadstream(pathname, std::ios_base::in | std::ios_base::binary)
    {
        if (!wadstream) {
            throw std::runtime_error("Unable to open WAD");
        }

        const auto size = stream_size(wadstream);
        if (!size) {
            throw std::runtime_error("Unable to determine WAD size");
        }
        file_size = *size;

        wadstream >> endianness<std::endian::little>;

        wad_header header{};

        wadstream >= header;

        if (!wadstream) {
            throw std::runtime_error("Truncated WAD header");
        }

        if (header.identification != wad2_ident && header.identification != wad3_ident) {
            throw std::runtime_error("Bad magic");
        }

        const uint64_t directory_size = static_cast<uint64_t>(header.numlumps) * sizeof(wad_lump_header);
        if (!range_within_file(header.infotableofs, directory_size, file_size)) {
            throw std::runtime_error("WAD directory is outside the file");
        }

        files.reserve(header.numlumps);

        wadstream.seekg(header.infotableofs);
        if (!wadstream) {
            throw std::runtime_error("Unable to seek to WAD directory");
        }

        for (size_t i = 0; i < header.numlumps; i++) {
            wad_lump_header file{};

            wadstream >= file;

            if (!wadstream) {
                throw std::runtime_error("Truncated WAD directory");
            }
            if (!range_within_file(file.filepos, file.disksize, file_size)) {
                throw std::runtime_error("WAD lump is outside the file");
            }

            std::string tex_name = file.name_as_string();
            if (file.compression != 0) {
                logging::print("WARNING: skipping compressed WAD lump '{}' in '{}'\n", tex_name, pathname);
                continue;
            }
            files[tex_name] = std::make_tuple(file.filepos, file.disksize);
        }
    }

    bool contains(const path &filename) override { return files.find(filename.generic_string()) != files.end(); }

    data load(const path &filename) override
    {
        auto it = files.find(filename.generic_string());

        if (it == files.end()) {
            return std::nullopt;
        }

        std::lock_guard lock(stream_mutex);
        return read_range(wadstream, std::get<0>(it->second), std::get<1>(it->second), file_size);
    }
};

static std::shared_ptr<directory_archive> absrel_dir = std::make_shared<directory_archive>("", false);
std::list<std::shared_ptr<archive_like>> archives, directories;

/** It's possible to compile quake 1/hexen 2 maps without a qdir */
void clear()
{
    archives.clear();
    directories.clear();
}

inline std::shared_ptr<archive_like> addArchiveInternal(const path &p, bool external)
{
    std::error_code type_error;
    const bool directory = is_directory(p, type_error);
    if (type_error) {
        logging::funcprint("WARNING: unable to inspect archive '{}': {}\n", p, type_error.message());
        return nullptr;
    }

    if (directory) {
        for (auto &dir : directories) {
            std::error_code ec;
            if (equivalent(dir->pathname, p, ec)) {
                return dir;
            }
        }

        auto &arch = directories.emplace_front(std::make_shared<directory_archive>(p, external));
        logging::print(logging::flag::VERBOSE, "Added directory '{}'\n", p);
        return arch;
    } else {
        for (auto &arch : archives) {
            std::error_code ec;
            if (equivalent(arch->pathname, p, ec)) {
                return arch;
            }
        }

        auto ext = p.extension();

        try {
            if (string_iequals(ext.generic_string(), ".pak")) {
                auto pak = std::make_shared<pak_archive>(p, external);
                archives.emplace_front(pak);
                logging::print(logging::flag::VERBOSE, "Added pak '{}' with {} files\n", p, pak->files.size());
                return pak;
            } else if (string_iequals(ext.generic_string(), ".wad")) {
                auto wad = std::make_shared<wad_archive>(p, external);
                archives.emplace_front(wad);
                logging::print(logging::flag::VERBOSE, "Added wad '{}' with {} lumps\n", p, wad->files.size());
                return wad;
            } else {
                logging::funcprint("WARNING: no idea what to do with archive '{}'\n", p);
            }
        } catch (const std::exception &e) {
            logging::funcprint("WARNING: unable to load archive '{}': {}\n", p, e.what());
        }
    }

    return nullptr;
}

std::shared_ptr<archive_like> addArchive(const path &p, bool external)
{
    if (p.empty()) {
        logging::funcprint("WARNING: can't add empty archive path\n");
        return nullptr;
    }

    std::error_code existence_error;
    const bool path_exists = exists(p, existence_error);
    if (existence_error) {
        logging::funcprint("WARNING: unable to inspect archive '{}': {}\n", p, existence_error.message());
        return nullptr;
    }

    if (!path_exists) {
        // check relative
        path filename = p.filename();

        existence_error.clear();
        const bool filename_exists = exists(filename, existence_error);
        if (existence_error) {
            logging::funcprint("WARNING: unable to inspect archive '{}': {}\n", filename, existence_error.message());
            return nullptr;
        }
        if (!filename_exists) {
            logging::funcprint("WARNING: archive '{}' not found\n", p);
            return nullptr;
        }

        return addArchiveInternal(filename, external);
    }

    return addArchiveInternal(p, external);
}

resolve_result where(const path &p, bool prefer_loose)
{
    // check direct archive loading first; it can't ever
    // be loose, so there's no sense for it to be in the
    // loop below
    if (auto paths = splitArchivePath(p)) {
        if (auto arch = addArchive(paths.archive)) {
            return {arch, paths.filename};
        }
    }

    for (int32_t pass = 0; pass < 2; pass++) {
        if (prefer_loose != !!pass) {
            // check absolute + relative

            // !is_directory() is a hack to avoid picking up a dir called "light"
            // when requesting a texture called "light" (was happening on CI)
            if (exists(p) && !is_directory(p)) {
                return {absrel_dir, p};
            }
        } else if (!p.is_absolute()) { // absolute doesn't make sense for other load types
            for (int32_t archive_pass = 0; archive_pass < 2; archive_pass++) {
                // check directories & archives, depending on whether
                // we want loose first or not
                for (auto &dir : (prefer_loose != !!archive_pass) ? directories : archives) {
                    if (dir->contains(p)) {
                        return {dir, p};
                    }
                }
            }
        }
    }

    return {};
}

data load(const resolve_result &pos)
{
    if (!pos) {
        return std::nullopt;
    }

    logging::print(logging::flag::VERBOSE, "Loaded '{}' from archive '{}'\n", pos.filename, pos.archive->pathname);

    return pos.archive->load(pos.filename);
}

data load(const path &p, bool prefer_loose)
{
    return load(where(p, prefer_loose));
}

archive_components splitArchivePath(const path &source)
{
    // check direct archive loading
    // this is a bit complex, but we check the whole
    // path to see if any piece of it that isn't
    // the last piece matches a file
    for (path archive = source.parent_path(); archive.has_relative_path(); archive = archive.parent_path()) {
        if (is_regular_file(archive)) {
            return {archive, source.lexically_relative(archive)};
        }
    }

    return {};
}

path resolveArchivePath(const path &source)
{
    if (auto paths = splitArchivePath(source)) {
        return paths.archive.parent_path() / paths.filename;
    }

    return source;
}
} // namespace fs

fs::path DefaultExtension(const fs::path &path, const fs::path &extension)
{
    if (path.has_extension())
        return path;

    return fs::path(path).replace_extension(extension);
}
