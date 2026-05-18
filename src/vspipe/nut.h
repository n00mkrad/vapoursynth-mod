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

enum class VSPipeNUTStreamType {
    Video,
    Audio
};

struct VSPipeNUTStreamInfo {
    VSPipeNUTStreamType type = VSPipeNUTStreamType::Video;
    std::array<uint8_t, 4> fourCC{};
    int width = 0;
    int height = 0;
    int sampleWidth = 1;
    int sampleHeight = 1;
    int colorspaceType = 0;
    int sampleRateNum = 0;
    int sampleRateDen = 1;
    int channelCount = 0;
    bool hasRFrameRate = false;
    int64_t rFrameRateNum = 0;
    int64_t rFrameRateDen = 1;
};

enum class VSPipeNUTSyncpointMode {
    PerFrame,
    MaxDistance
};

struct VSPipeNUTWriterOptions {
    VSPipeNUTSyncpointMode syncpointMode = VSPipeNUTSyncpointMode::PerFrame;
    bool forceZeroBackPointers = false;
    int64_t timebaseNum = 1;
    int64_t timebaseDen = 1000000;
};

class VSPipeNUTWriter {
public:
    bool initialize(FILE *outFile, const std::vector<VSPipeNUTStreamInfo> &streams, const VSPipeNUTWriterOptions &options, std::string &errorMessage);
    bool writeFrameHeader(int streamId, int64_t pts, size_t frameSize, bool keyFrame, std::string &errorMessage);
    void notePayloadWritten(size_t bytes);

    static bool getVideoFourCC(const VSVideoFormat &format, std::array<uint8_t, 4> &fourCC);
    static bool getAudioFourCC(const VSAudioFormat &format, std::array<uint8_t, 4> &fourCC);

private:
    static uint32_t crc32(const uint8_t *buf, size_t len);
    static void appendV(std::vector<uint8_t> &dst, uint64_t value);
    static void appendS(std::vector<uint8_t> &dst, int64_t value);
    static void appendU32(std::vector<uint8_t> &dst, uint32_t value);
    static void appendU64(std::vector<uint8_t> &dst, uint64_t value);
    static void appendVB(std::vector<uint8_t> &dst, const uint8_t *data, size_t len);

    bool writeBuffer(const std::vector<uint8_t> &buffer, const char *context, std::string &errorMessage);
    bool writePacket(uint64_t startcode, const std::vector<uint8_t> &payload, std::string &errorMessage);
    bool writeSyncpoint(int64_t pts, std::string &errorMessage);
    bool writeMainHeader(const std::vector<VSPipeNUTStreamInfo> &streams, std::string &errorMessage);
    bool writeStreamHeader(int streamId, const VSPipeNUTStreamInfo &stream, std::string &errorMessage);
    bool writeInfoPacketUTF8(int streamId, const std::string &name, const std::string &value, std::string &errorMessage);
    bool writeStreamInfo(int streamId, const VSPipeNUTStreamInfo &stream, std::string &errorMessage);
    bool writeInitialSyncpoint(std::string &errorMessage);

    FILE *outFile = nullptr;
    VSPipeNUTWriterOptions options{};
    int msbPtsShift = 8;
    int64_t bytesWritten = 0;
    int64_t lastSyncpointPosition = -1;
};

#endif
