/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "ArchiveUtil.h"

#include <algorithm>
#include <new>

using namespace melonDS;

namespace Archive
{
namespace
{
using Reader = std::unique_ptr<archive, decltype(&archive_read_free)>;

Reader OpenArchive(const QString& path)
{
    Reader reader(archive_read_new(), archive_read_free);
    if (!reader) return reader;

    archive_read_support_filter_all(reader.get());
    archive_read_support_format_all(reader.get());
#ifdef __WIN32__
    const int result = archive_read_open_filename_w(reader.get(),
        reinterpret_cast<const wchar_t*>(path.utf16()), 64 * 1024);
#else
    const int result = archive_read_open_filename(reader.get(),
        path.toUtf8().constData(), 64 * 1024);
#endif
    if (result != ARCHIVE_OK) reader.reset();
    return reader;
}
}

QVector<QString> ListArchive(QString path)
{
    auto reader = OpenArchive(path);
    if (!reader) return {"Err"};

    QVector<QString> files;
    archive_entry* entry = nullptr;
    int result;
    while ((result = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK)
    {
        if (archive_entry_filetype(entry) == AE_IFREG)
        {
            const char* name = archive_entry_pathname_utf8(entry);
            if (!name) return {"Err"};
            files.push_back(QString::fromUtf8(name));
        }
        if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) return {"Err"};
    }
    if (result != ARCHIVE_EOF) return {"Err"};

    std::stable_sort(files.begin(), files.end(), [](const QString& a, const QString& b) {
        return a.toLower() < b.toLower();
    });
    files.prepend("OK");
    return files;
}

s32 ExtractFileFromArchive(QString path, QString wantedFile, std::unique_ptr<u8[]>& filedata, u32* filesize)
{
    // Publish a ROM only after its complete contents and end-of-entry checks
    // have succeeded. Every failure releases both the reader and partial ROM.
    filedata.reset();
    if (filesize) *filesize = 0;
    auto reader = OpenArchive(path);
    if (!reader) return -1;

    archive_entry* entry = nullptr;
    while (archive_read_next_header(reader.get(), &entry) == ARCHIVE_OK)
    {
        const char* name = archive_entry_pathname_utf8(entry);
        if (!name) return -1;
        if (wantedFile != QString::fromUtf8(name))
        {
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) return -1;
            continue;
        }

        if (archive_entry_filetype(entry) != AE_IFREG ||
            !archive_entry_size_is_set(entry)) return -1;
        const la_int64_t size = archive_entry_size(entry);
        if (size <= 0 || size > MaxROMSize) return -1;

        // libarchive streams directly from the archive into the final ROM
        // allocation. No temporary file or second decompressed buffer is used.
        std::unique_ptr<u8[]> data(new (std::nothrow) u8[static_cast<size_t>(size)]);
        if (!data) return -1;
        size_t offset = 0;
        while (offset < static_cast<size_t>(size))
        {
            const size_t chunk = std::min<size_t>(64 * 1024, size - offset);
            const la_ssize_t count = archive_read_data(reader.get(), data.get() + offset, chunk);
            if (count <= 0) return -1;
            offset += static_cast<size_t>(count);
        }

        // Reading through EOF also checks the ZIP CRC and rejects entries whose
        // actual decompressed data exceeds their advertised size.
        u8 extra;
        if (archive_read_data(reader.get(), &extra, 1) != 0) return -1;

        filedata = std::move(data);
        if (filesize) *filesize = static_cast<u32>(size);
        return static_cast<s32>(size);
    }
    return -1;
}

}
