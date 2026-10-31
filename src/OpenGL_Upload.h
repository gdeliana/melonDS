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

#ifndef MELONDS_OPENGL_UPLOAD_H
#define MELONDS_OPENGL_UPLOAD_H

#include <cstring>
#include <vector>
#include "PlatformOGL.h"

namespace melonDS::OpenGL
{
// Each cache owns all uploads to one buffer. Consumers read only the supplied
// prefix. Clearing the cache is required when its GL storage is recreated.
class BufferUploadCache
{
public:
    void Clear() { Bytes.clear(); Buffer = 0; }
    bool Upload(GLenum target, GLuint buffer, const void* data, size_t size)
    {
        if (!size) return false;
        if (Buffer == buffer && Bytes.size() >= size &&
            std::memcmp(Bytes.data(), data, size) == 0) return false;
        glBindBuffer(target, buffer);
        glBufferSubData(target, 0, size, data);
        const auto* begin = static_cast<const unsigned char*>(data);
        Bytes.assign(begin, begin + size);
        Buffer = buffer;
        return true;
    }
private:
    GLuint Buffer = 0;
    std::vector<unsigned char> Bytes;
};
}
#endif
