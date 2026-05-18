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

#include "nut.h"

#include <cerrno>

namespace {

constexpr uint64_t kMainStartcode = 0x7A561F5F04ADULL + (((uint64_t)('N' << 8) + 'M') << 48);
constexpr uint64_t kStreamStartcode = 0x11405BF2F9DBULL + (((uint64_t)('N' << 8) + 'S') << 48);
constexpr uint64_t kInfoStartcode = 0xAB68B596BA78ULL + (((uint64_t)('N' << 8) + 'I') << 48);
constexpr uint64_t kSyncpointStartcode = 0xE4ADEECA4569ULL + (((uint64_t)('N' << 8) + 'K') << 48);
constexpr uint8_t kNutIdString[] = "nut/multimedia container";
constexpr uint64_t kNutVersion = 3;
constexpr uint64_t kMaxDistance = 32768;
constexpr uint64_t kVideoClass = 0;
constexpr uint64_t kAudioClass = 1;
constexpr uint64_t kStreamFlagsNone = 0;
constexpr uint64_t kMaxPtsDistance = 1000000;
constexpr uint64_t kFlagKey = 1;
constexpr uint64_t kFlagCodedPts = 8;
constexpr uint64_t kFlagStreamId = 16;
constexpr uint64_t kFlagSizeMsb = 32;
constexpr uint64_t kFlagChecksum = 64;
constexpr uint64_t kFlagInvalid = 8192;

}

bool VSPipeNUTWriter::initialize(FILE *file, const std::vector<VSPipeNUTStreamInfo> &streams, const VSPipeNUTWriterOptions &initOptions, std::string &errorMessage) {
    if (streams.empty()) {
        errorMessage = "Error: NUT stream list is empty";
        return false;
    }

    outFile = file;
    options = initOptions;
    if (options.timebaseNum <= 0 || options.timebaseDen <= 0) {
        options.timebaseNum = 1;
        options.timebaseDen = 1000000;
    }
    bytesWritten = 0;
    lastSyncpointPosition = -1;

    if (!writeBuffer(std::vector<uint8_t>(kNutIdString, kNutIdString + sizeof(kNutIdString)), "initial NUT identifier", errorMessage))
        return false;
    if (!writeMainHeader(streams, errorMessage))
        return false;
    for (size_t i = 0; i < streams.size(); i++) {
        if (!writeStreamHeader(static_cast<int>(i), streams[i], errorMessage))
            return false;
    }
    for (size_t i = 0; i < streams.size(); i++) {
        if (!writeStreamInfo(static_cast<int>(i), streams[i], errorMessage))
            return false;
    }
    if (!writeInitialSyncpoint(errorMessage))
        return false;
    return true;
}

bool VSPipeNUTWriter::writeFrameHeader(int streamId, int64_t pts, size_t frameSize, bool keyFrame, std::string &errorMessage) {
    if (streamId < 0) {
        errorMessage = "Error: invalid negative NUT stream id";
        return false;
    }

    if (options.syncpointMode == VSPipeNUTSyncpointMode::PerFrame) {
        if (!writeSyncpoint(pts, errorMessage))
            return false;
    } else {
        constexpr uint64_t kFrameHeaderReserve = 32;
        uint64_t syncpointDistance = 0;
        if (lastSyncpointPosition >= 0 && bytesWritten >= lastSyncpointPosition)
            syncpointDistance = static_cast<uint64_t>(bytesWritten - lastSyncpointPosition);
        if (syncpointDistance + frameSize + kFrameHeaderReserve > kMaxDistance) {
            if (!writeSyncpoint(pts, errorMessage))
                return false;
        }
    }

    std::vector<uint8_t> frameHeader;
    frameHeader.reserve(64);

    frameHeader.push_back(keyFrame ? 1 : 2);
    appendV(frameHeader, static_cast<uint64_t>(streamId));
    appendV(frameHeader, static_cast<uint64_t>(pts) + (1ULL << msbPtsShift));
    appendV(frameHeader, static_cast<uint64_t>(frameSize));
    appendU32(frameHeader, crc32(frameHeader.data(), frameHeader.size()));

    return writeBuffer(frameHeader, "NUT frame header", errorMessage);
}

bool VSPipeNUTWriter::writeSyncpoint(int64_t pts, std::string &errorMessage) {
    int64_t syncpointPos = bytesWritten;

    uint64_t backPtrDiv16 = 0;
    if (!options.forceZeroBackPointers && lastSyncpointPosition >= 0 && syncpointPos > lastSyncpointPosition)
        backPtrDiv16 = static_cast<uint64_t>(syncpointPos - lastSyncpointPosition) / 16;

    std::vector<uint8_t> payload;
    payload.reserve(16);
    appendV(payload, static_cast<uint64_t>(pts));
    appendV(payload, backPtrDiv16);
    if (!writePacket(kSyncpointStartcode, payload, errorMessage))
        return false;
    lastSyncpointPosition = syncpointPos;
    return true;
}

void VSPipeNUTWriter::notePayloadWritten(size_t bytes) {
    bytesWritten += static_cast<int64_t>(bytes);
}

bool VSPipeNUTWriter::getVideoFourCC(const VSVideoFormat &format, std::array<uint8_t, 4> &fourCC) {
    if (format.colorFamily == cfRGB) {
        if (format.subSamplingW != 0 || format.subSamplingH != 0)
            return false;

        uint8_t formatCode = 0;
        if (format.sampleType == stInteger) {
            switch (format.bitsPerSample) {
            case 8:
                formatCode = 8;
                break;
            case 9:
                formatCode = 9;
                break;
            case 10:
                formatCode = 10;
                break;
            case 12:
                formatCode = 12;
                break;
            case 14:
                formatCode = 14;
                break;
            case 16:
                formatCode = 16;
                break;
            default:
                return false;
            }
        } else if (format.sampleType == stFloat) {
            if (format.bitsPerSample == 16)
                formatCode = 17;
            else if (format.bitsPerSample == 32)
                formatCode = 33;
            else
                return false;
        } else {
            return false;
        }

        fourCC = { 'G', '3', 0, formatCode };
        return true;
    }

    if (format.sampleType != stInteger)
        return false;

    uint8_t bitsPerSample = 0;
    switch (format.bitsPerSample) {
    case 8:
    case 9:
    case 10:
    case 12:
    case 14:
    case 16:
        bitsPerSample = static_cast<uint8_t>(format.bitsPerSample);
        break;
    default:
        return false;
    }

    if (format.colorFamily == cfGray) {
        if (format.subSamplingW != 0 || format.subSamplingH != 0)
            return false;
        if (bitsPerSample == 8) {
            fourCC = { 'Y', '8', '0', '0' };
            return true;
        }
        fourCC = { 'Y', '1', 0, bitsPerSample };
        return true;
    }

    if (format.colorFamily == cfYUV) {
        bool is420 = format.subSamplingW == 1 && format.subSamplingH == 1;
        bool is422 = format.subSamplingW == 1 && format.subSamplingH == 0;
        bool is444 = format.subSamplingW == 0 && format.subSamplingH == 0;
        bool is410 = format.subSamplingW == 2 && format.subSamplingH == 2;
        bool is411 = format.subSamplingW == 2 && format.subSamplingH == 0;
        bool is440 = format.subSamplingW == 0 && format.subSamplingH == 1;
        uint8_t subSamplingCode = 0;
        if (is420)
            subSamplingCode = 11;
        else if (is422)
            subSamplingCode = 10;
        else if (is444)
            subSamplingCode = 0;
        else if (is410)
            subSamplingCode = 22;
        else if (is411)
            subSamplingCode = 20;
        else if (is440)
            subSamplingCode = 1;
        else
            return false;

        if (bitsPerSample == 8) {
            if (is420)
                fourCC = { 'I', '4', '2', '0' };
            else if (is422)
                fourCC = { '4', '2', '2', 'P' };
            else if (is410)
                fourCC = { 'Y', 'U', 'V', '9' };
            else if (is411)
                fourCC = { '4', '1', '1', 'P' };
            else if (is440)
                fourCC = { '4', '4', '0', 'P' };
            else
                fourCC = { '4', '4', '4', 'P' };
            return true;
        }

        fourCC = { 'Y', '3', subSamplingCode, bitsPerSample };
        return true;
    }

    return false;
}

bool VSPipeNUTWriter::getAudioFourCC(const VSAudioFormat &format, std::array<uint8_t, 4> &fourCC) {
    if (format.sampleType == stInteger) {
        if (format.bitsPerSample == 16 || format.bitsPerSample == 24 || format.bitsPerSample == 32) {
            fourCC = { 'P', 'S', 'D', static_cast<uint8_t>(format.bitsPerSample) };
            return true;
        }
        return false;
    }

    if (format.sampleType == stFloat && format.bitsPerSample == 32) {
        fourCC = { 'P', 'F', 'D', 32 };
        return true;
    }

    return false;
}

uint32_t VSPipeNUTWriter::crc32(const uint8_t *buf, size_t len) {
    static constexpr uint32_t table[16] = {
        0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9,
        0x130476DC, 0x17C56B6B, 0x1A864DB2, 0x1E475005,
        0x2608EDB8, 0x22C9F00F, 0x2F8AD6D6, 0x2B4BCB61,
        0x350C9B64, 0x31CD86D3, 0x3C8EA00A, 0x384FBDBD
    };

    uint32_t crc = 0;
    while (len--) {
        crc ^= static_cast<uint32_t>(*buf++) << 24;
        crc = (crc << 4) ^ table[crc >> 28];
        crc = (crc << 4) ^ table[crc >> 28];
    }

    return crc;
}

void VSPipeNUTWriter::appendV(std::vector<uint8_t> &dst, uint64_t value) {
    value &= 0x7FFFFFFFFFFFFFFFULL;

    int groups = 1;
    while (value >> (groups * 7))
        groups++;

    for (int shift = (groups - 1) * 7; shift > 0; shift -= 7)
        dst.push_back(static_cast<uint8_t>(0x80 | ((value >> shift) & 0x7F)));
    dst.push_back(static_cast<uint8_t>(value & 0x7F));
}

void VSPipeNUTWriter::appendS(std::vector<uint8_t> &dst, int64_t value) {
    if (value <= 0)
        appendV(dst, static_cast<uint64_t>(-2 * value));
    else
        appendV(dst, static_cast<uint64_t>(2 * value - 1));
}

void VSPipeNUTWriter::appendU32(std::vector<uint8_t> &dst, uint32_t value) {
    dst.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    dst.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    dst.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    dst.push_back(static_cast<uint8_t>(value & 0xFF));
}

void VSPipeNUTWriter::appendU64(std::vector<uint8_t> &dst, uint64_t value) {
    dst.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
    dst.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
    dst.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
    dst.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
    dst.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    dst.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    dst.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    dst.push_back(static_cast<uint8_t>(value & 0xFF));
}

void VSPipeNUTWriter::appendVB(std::vector<uint8_t> &dst, const uint8_t *data, size_t len) {
    appendV(dst, static_cast<uint64_t>(len));
    dst.insert(dst.end(), data, data + len);
}

bool VSPipeNUTWriter::writeBuffer(const std::vector<uint8_t> &buffer, const char *context, std::string &errorMessage) {
    if (!outFile || buffer.empty())
        return true;

    if (fwrite(buffer.data(), 1, buffer.size(), outFile) != buffer.size()) {
        errorMessage = std::string("Error: fwrite() call failed when writing ") + context + ", errno: " + std::to_string(errno);
        return false;
    }

    bytesWritten += static_cast<int64_t>(buffer.size());
    return true;
}

bool VSPipeNUTWriter::writePacket(uint64_t startcode, const std::vector<uint8_t> &payload, std::string &errorMessage) {
    std::vector<uint8_t> packet;
    packet.reserve(32 + payload.size());

    appendU64(packet, startcode);
    const uint64_t forwardPtr = payload.size() + 4;
    appendV(packet, forwardPtr);
    if (forwardPtr > 4096)
        appendU32(packet, crc32(packet.data(), packet.size()));

    packet.insert(packet.end(), payload.begin(), payload.end());
    appendU32(packet, crc32(payload.data(), payload.size()));

    return writeBuffer(packet, "NUT packet", errorMessage);
}

bool VSPipeNUTWriter::writeMainHeader(const std::vector<VSPipeNUTStreamInfo> &streams, std::string &errorMessage) {
    std::vector<uint8_t> payload;
    payload.reserve(128);

    appendV(payload, kNutVersion);
    appendV(payload, static_cast<uint64_t>(streams.size()));
    appendV(payload, kMaxDistance);

    appendV(payload, 1);
    appendV(payload, static_cast<uint64_t>(options.timebaseNum));
    appendV(payload, static_cast<uint64_t>(options.timebaseDen));

    appendV(payload, kFlagInvalid);
    appendV(payload, 0);

    appendV(payload, kFlagKey | kFlagCodedPts | kFlagStreamId | kFlagSizeMsb | kFlagChecksum);
    appendV(payload, 4);
    appendS(payload, 0);
    appendV(payload, 1);
    appendV(payload, 0);
    appendV(payload, 0);

    appendV(payload, kFlagCodedPts | kFlagStreamId | kFlagSizeMsb | kFlagChecksum);
    appendV(payload, 0);

    appendV(payload, kFlagInvalid);
    appendV(payload, 2);
    appendS(payload, 0);
    appendV(payload, 252);

    appendV(payload, 0);
    appendV(payload, 0);

    return writePacket(kMainStartcode, payload, errorMessage);
}

bool VSPipeNUTWriter::writeStreamHeader(int streamId, const VSPipeNUTStreamInfo &stream, std::string &errorMessage) {
    if (streamId < 0) {
        errorMessage = "Error: invalid negative NUT stream id";
        return false;
    }

    std::vector<uint8_t> payload;
    payload.reserve(128);

    appendV(payload, static_cast<uint64_t>(streamId));
    appendV(payload, stream.type == VSPipeNUTStreamType::Video ? kVideoClass : kAudioClass);
    appendVB(payload, stream.fourCC.data(), stream.fourCC.size());
    appendV(payload, 0);
    appendV(payload, msbPtsShift);
    appendV(payload, kMaxPtsDistance);
    appendV(payload, 0);
    appendV(payload, kStreamFlagsNone);
    appendV(payload, 0);

    if (stream.type == VSPipeNUTStreamType::Video) {
        appendV(payload, static_cast<uint64_t>(stream.width));
        appendV(payload, static_cast<uint64_t>(stream.height));
        appendV(payload, static_cast<uint64_t>(stream.sampleWidth));
        appendV(payload, static_cast<uint64_t>(stream.sampleHeight));
        appendV(payload, static_cast<uint64_t>(stream.colorspaceType));
    } else {
        appendV(payload, static_cast<uint64_t>(stream.sampleRateNum));
        appendV(payload, static_cast<uint64_t>(stream.sampleRateDen));
        appendV(payload, static_cast<uint64_t>(stream.channelCount));
    }

    return writePacket(kStreamStartcode, payload, errorMessage);
}

bool VSPipeNUTWriter::writeInfoPacketUTF8(int streamId, const std::string &name, const std::string &value, std::string &errorMessage) {
    if (streamId < 0) {
        errorMessage = "Error: invalid negative NUT stream id";
        return false;
    }

    std::vector<uint8_t> payload;
    payload.reserve(96);

    appendV(payload, static_cast<uint64_t>(streamId + 1));
    appendS(payload, 0);
    appendV(payload, 0);
    appendV(payload, 0);
    appendV(payload, 1);
    appendVB(payload, reinterpret_cast<const uint8_t *>(name.data()), name.size());
    appendS(payload, -1);
    appendVB(payload, reinterpret_cast<const uint8_t *>(value.data()), value.size());

    return writePacket(kInfoStartcode, payload, errorMessage);
}

bool VSPipeNUTWriter::writeStreamInfo(int streamId, const VSPipeNUTStreamInfo &stream, std::string &errorMessage) {
    if (stream.type != VSPipeNUTStreamType::Video)
        return true;
    if (!stream.hasRFrameRate || stream.rFrameRateNum <= 0 || stream.rFrameRateDen <= 0)
        return true;

    return writeInfoPacketUTF8(streamId, "r_frame_rate", std::to_string(stream.rFrameRateNum) + "/" + std::to_string(stream.rFrameRateDen), errorMessage);
}

bool VSPipeNUTWriter::writeInitialSyncpoint(std::string &errorMessage) {
    return writeSyncpoint(0, errorMessage);
}
