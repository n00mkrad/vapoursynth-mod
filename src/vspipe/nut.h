/*
* Copyright (c) 2026 Fredrik Mellbin
*
* This file is part of VapourSynth.
*
* VapourSynth is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* VapourSynth is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with VapourSynth; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#ifndef VSPIPE_NUT_H
#define VSPIPE_NUT_H

#include "VapourSynth4.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class VSPipeNUTWriter {
public:
    bool initialize(FILE *outFile, const VSVideoInfo *vi, std::string &errorMessage);
    bool writeFrameHeader(int64_t pts, size_t frameSize, bool keyFrame, std::string &errorMessage);

    static bool getVideoFourCC(const VSVideoFormat &format, std::array<uint8_t, 4> &fourCC);

private:
    static uint32_t crc32(const uint8_t *buf, size_t len);
    static void appendV(std::vector<uint8_t> &dst, uint64_t value);
    static void appendS(std::vector<uint8_t> &dst, int64_t value);
    static void appendU32(std::vector<uint8_t> &dst, uint32_t value);
    static void appendU64(std::vector<uint8_t> &dst, uint64_t value);
    static void appendVB(std::vector<uint8_t> &dst, const uint8_t *data, size_t len);

    bool writeBuffer(const std::vector<uint8_t> &buffer, const char *context, std::string &errorMessage) const;
    bool writePacket(uint64_t startcode, const std::vector<uint8_t> &payload, std::string &errorMessage) const;
    bool writeSyncpoint(int64_t pts, std::string &errorMessage) const;
    bool writeMainHeader(const VSVideoInfo *vi, std::string &errorMessage) const;
    bool writeStreamHeader(const VSVideoInfo *vi, const std::array<uint8_t, 4> &fourCC, std::string &errorMessage) const;
    bool writeInitialSyncpoint(std::string &errorMessage) const;

    FILE *outFile = nullptr;
    int msbPtsShift = 8;
};

#endif
