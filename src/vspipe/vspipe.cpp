/*
* Copyright (c) 2013-2026 Fredrik Mellbin
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

#include "VapourSynth4.h"
#include "VSHelper4.h"
#include "VSScript4.h"
#include "../core/version.h"
#include "printgraph.h"
#include "vsjson.h"
#include "nut.h"
#include <array>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <memory>
#include <set>
#include "../common/wave.h"
#ifdef VS_TARGET_OS_WINDOWS
#include <io.h>
#include <fcntl.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#define __STDC_FORMAT_MACROS
#include <cstdio>
#include <cinttypes>

// fixme, add a more verbose graph mode with filter times included

// Needed so windows doesn't drool on itself when ctrl-c is pressed
#ifdef VS_TARGET_OS_WINDOWS
#include <windows.h>
static BOOL WINAPI HandlerRoutine(DWORD dwCtrlType) {
    switch (dwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        _exit(1);
    default:
        return FALSE;
    }
}
#else
static void HandlerRoutine(int sig) {
    _exit(1);
}
#endif

using namespace vsh;

static FILE *OpenFile(const std::filesystem::path &Path) {
#ifdef VS_TARGET_OS_WINDOWS
    return _wfopen(Path.c_str(), L"wb");
#else
    return fopen(Path.c_str(), "wb");
#endif
}

/////////////////////////////////////////////

enum class VSPipeMode {
    Output,
    PrintVersion,
    PrintHelp,
    PrintInfo,
    PrintSimpleGraph,
    PrintFullGraph
};

enum class VSPipeHeaders {
    None,
    Y4M,
    NUT,
    WAVE,
    WAVE64
};

// Struct used to return the parsed command line options
struct VSPipeOptions {
    VSPipeMode mode = VSPipeMode::Output;
    VSPipeHeaders outputHeaders = VSPipeHeaders::None;
    int64_t startPos = 0;
    int64_t endPos = -1;
    int outputIndex = 0;
    int audioOutputIndex = -1;
    std::vector<int> outputIndices;
    std::vector<int> audioOutputIndices;
    bool outputIndexExplicit = false;
    bool audioOutputIndexExplicit = false;
    int requests = 0;
    bool printProgress = false;
    bool frameRefDebug = false;
    bool printFilterTime = false;
    bool noDepWarn = false;
    std::filesystem::path scriptFilename;
    std::filesystem::path outputFilename;
    std::filesystem::path timecodesFilename;
    std::filesystem::path jsonFilename;
    std::filesystem::path filterTimeGraphFilename;
    std::map<std::string, std::string> scriptArgs;
};

// All state used for outputting frames

struct VSPipeOutputData {
    /* Core fields */
    const VSAPI *vsapi = nullptr;
    VSPipeHeaders outputHeaders = VSPipeHeaders::None;
    FILE *outFile = nullptr;
    VSNode *node = nullptr;
    VSNode *alphaNode = nullptr;
    VSNode *audioNode = nullptr;

    /* Total number of frames and samples */
    int totalFrames = -1;
    int64_t totalSamples = -1;

    /* Fields used for keeping track of how many frames have been requested and completed and how to reorder them */
    int outputFrames = 0;
    int requestedFrames = 0;
    int completedFrames = 0;
    int completedAlphaFrames = 0;
    std::map<int, std::pair<const VSFrame *, const VSFrame *>> reorderMap;

    /* Error reporting */
    bool outputError = false;
    std::string errorMessage;

    /* Only used to keep the main thread waiting during processing */
    std::condition_variable condition;
    std::mutex mutex;

    /* Buffer used to interleave audio or to pack together video where the rowsize isn't the same as pitch due to multiple calls to stdout being very slow */
    std::vector<uint8_t> buffer;

    /* Statistics */
    bool printProgress = false;
    std::chrono::time_point<std::chrono::steady_clock> startTime;
    std::chrono::time_point<std::chrono::steady_clock> lastFPSReportTime;

    /* Timecode output */
    FILE *timecodesFile = nullptr;
    std::map<std::pair<int64_t, int64_t>, int64_t> currentTimecode;
    double getCurrentTimecode() {        
        double sum = 0;
        for (const auto &iter : currentTimecode)
            sum += iter.second * (static_cast<double>(iter.first.first) / iter.first.second);
        return sum;
    }

    /* JSON output */
    FILE *jsonFile = nullptr;

    /* NUT output */
    std::unique_ptr<VSPipeNUTWriter> nutWriter;
    size_t nutFrameSize = 0;
    int64_t nutCurrentPts = 0;
    int64_t nutTimebaseNum = 1;
    int64_t nutTimebaseDen = 1000000;
    int64_t nutFallbackDurationTicks = 1;
    int64_t nutAudioSamplesWritten = 0;
};

/////////////////////////////////////////////

static std::string channelMaskToName(uint64_t v) {
    std::string s;
    auto checkConstant = [&s, v](uint64_t c, const char *name) {
        if ((static_cast<uint64_t>(1) << c) & v) {
            if (!s.empty())
                s += ", ";
            s += name;
        }
    };

    checkConstant(acFrontLeft, "Front Left");
    checkConstant(acFrontRight, "Front Right");
    checkConstant(acFrontCenter, "Center");
    checkConstant(acLowFrequency, "LFE");
    checkConstant(acBackLeft, "Back Left");
    checkConstant(acBackRight, "Back Right");
    checkConstant(acFrontLeftOFCenter, "Front Left of Center");
    checkConstant(acFrontRightOFCenter, "Front Right of Center");
    checkConstant(acBackCenter, "Back Center");
    checkConstant(acSideLeft, "Side Left");
    checkConstant(acSideRight, "Side Right");
    checkConstant(acTopCenter, "Top Center");
    checkConstant(acTopFrontLeft, "Top Front Left");
    checkConstant(acTopFrontCenter, "Top Front Center");
    checkConstant(acTopFrontRight, "Top Front Right");
    checkConstant(acTopBackLeft, "Top Back Left");
    checkConstant(acTopBackCenter, "Top Back Center");
    checkConstant(acTopBackRight, "Top Back Right");
    checkConstant(acStereoLeft, "Stereo Left");
    checkConstant(acStereoRight, "Stereo Right");
    checkConstant(acWideLeft, "Wide Left");
    checkConstant(acWideRight, "Wide Right");
    checkConstant(acSurroundDirectLeft, "Surround Direct Left");
    checkConstant(acSurroundDirectRight, "Surround Direct Right");
    checkConstant(acLowFrequency2, "LFE2");

    return s;
}

static const char *messageTypeToString(int msgType) {
    switch (msgType) {
        case mtDebug: return "Debug";
        case mtInformation: return "Information";
        case mtWarning: return "Warning";
        case mtCritical: return "Critical";
        case mtFatal: return "Fatal";
        default: return "";
    }
}

static void VS_CC logMessageHandler(int msgType, const char *msg, void *userData) {
    const VSPipeOptions *opts = static_cast<const VSPipeOptions *>(userData);
    if (opts && opts->noDepWarn && msgType == mtWarning && msg && std::string_view(msg).find("API3 which is deprecated and will be removed") != std::string_view::npos)
        return;
#ifdef NDEBUG
    if (msgType >= mtInformation)
#else
    if (msgType >= mtDebug)
#endif
        fprintf(stderr, "%s: %s\n", messageTypeToString(msgType), msg);
}

static bool isCompletedFrame(const std::pair<const VSFrame *, const VSFrame *> &f, bool hasAlpha) {
    return (f.first && (!hasAlpha || f.second));
}

static void outputFrame(const VSFrame *frame, VSPipeOutputData *data) {
    if (!data->outputError && data->outFile) {
        if (data->vsapi->getFrameType(frame) == mtVideo) {
            const VSVideoFormat *fi = data->vsapi->getVideoFrameFormat(frame);
            const int rgbRemap[] = { 1, 2, 0 };
            for (int rp = 0; rp < fi->numPlanes; rp++) {
                int p = (fi->colorFamily == cfRGB) ? rgbRemap[rp] : rp;
                ptrdiff_t stride = data->vsapi->getStride(frame, p);
                const uint8_t *readPtr = data->vsapi->getReadPtr(frame, p);
                int rowSize = data->vsapi->getFrameWidth(frame, p) * fi->bytesPerSample;
                int height = data->vsapi->getFrameHeight(frame, p);

                if (rowSize != stride) {
                    bitblt(data->buffer.data(), rowSize, readPtr, stride, rowSize, height);
                    readPtr = data->buffer.data();
                }

                if (fwrite(readPtr, 1, rowSize * height, data->outFile) != static_cast<size_t>(rowSize * height)) {
                    if (data->errorMessage.empty())
                        data->errorMessage = "Error: fwrite() call failed when writing frame: " + std::to_string(data->outputFrames) + ", plane: " + std::to_string(p) +
                        ", errno: " + std::to_string(errno);
                    data->totalFrames = data->requestedFrames;
                    data->outputError = true;
                    break;
                }
            }
        } else if (data->vsapi->getFrameType(frame) == mtAudio) {
            const VSAudioFormat *fi = data->vsapi->getAudioFrameFormat(frame);

            int numChannels = fi->numChannels;
            int numSamples = data->vsapi->getFrameLength(frame);
            size_t bytesPerOutputSample = (fi->bitsPerSample + 7) / 8;
            size_t toOutput = bytesPerOutputSample * numSamples * numChannels;

            std::vector<const uint8_t *> srcPtrs;
            srcPtrs.reserve(numChannels);
            for (int channel = 0; channel < numChannels; channel++)
                srcPtrs.push_back(data->vsapi->getReadPtr(frame, channel));
            
            if (bytesPerOutputSample == 2)
                PackChannels16to16le(srcPtrs.data(), data->buffer.data(), numSamples, numChannels);
            else if (bytesPerOutputSample == 3)
                PackChannels32to24le(srcPtrs.data(), data->buffer.data(), numSamples, numChannels);
            else if (bytesPerOutputSample == 4)
                PackChannels32to32le(srcPtrs.data(), data->buffer.data(), numSamples, numChannels);

            if (fwrite(data->buffer.data(), 1, toOutput, data->outFile) != toOutput) {
                if (data->errorMessage.empty())
                    data->errorMessage = "Error: fwrite() call failed when writing frame: " + std::to_string(data->outputFrames) + ", errno: " + std::to_string(errno);
                data->totalFrames = data->requestedFrames;
                data->outputError = true;
            }
        }
    }
}

static std::string doubleToString(double v) {
    char buffer[100];
    auto res = std::to_chars(buffer, buffer + sizeof(buffer), v, std::chars_format::fixed);
    return std::string(buffer, res.ptr - buffer);
}

static int64_t convertDurationToNutTicks(int64_t durationNum, int64_t durationDen, int64_t timebaseNum, int64_t timebaseDen) {
    long double num = static_cast<long double>(durationNum) * static_cast<long double>(timebaseDen);
    long double den = static_cast<long double>(durationDen) * static_cast<long double>(timebaseNum);
    long double ticks = num / den;
    return std::max<int64_t>(1, static_cast<int64_t>(ticks + 0.5L));
}

static int64_t getNutFramePts(const VSFrame *frame, const VSAPI *vsapi, int64_t fallbackPts, int64_t timebaseNum, int64_t timebaseDen) {
    int64_t pts = fallbackPts;
    const VSMap *props = vsapi->getFramePropertiesRO(frame);
    int err = 0;
    double absoluteTime = vsapi->mapGetFloat(props, "_AbsoluteTime", 0, &err);
    if (!err && absoluteTime >= 0.0) {
        long double scaled = static_cast<long double>(absoluteTime) * static_cast<long double>(timebaseDen) / static_cast<long double>(timebaseNum);
        pts = std::max<int64_t>(0, static_cast<int64_t>(scaled + 0.5L));
    }
    return pts;
}

static int64_t getNutFrameDurationTicks(const VSFrame *frame, const VSAPI *vsapi, int64_t fallbackDurationTicks, int64_t timebaseNum, int64_t timebaseDen) {
    int64_t durationTicks = fallbackDurationTicks;
    const VSMap *props = vsapi->getFramePropertiesRO(frame);
    int errNum = 0;
    int errDen = 0;
    int64_t durationNum = vsapi->mapGetInt(props, "_DurationNum", 0, &errNum);
    int64_t durationDen = vsapi->mapGetInt(props, "_DurationDen", 0, &errDen);
    if (!errNum && !errDen && durationNum > 0 && durationDen > 0)
        durationTicks = convertDurationToNutTicks(durationNum, durationDen, timebaseNum, timebaseDen);
    return std::max<int64_t>(1, durationTicks);
}

static size_t getNutVideoFrameSize(const VSVideoInfo *vi) {
    size_t frameSize = 0;
    for (int p = 0; p < vi->format.numPlanes; p++) {
        int width = vi->width >> ((p == 1 || p == 2) ? vi->format.subSamplingW : 0);
        int height = vi->height >> ((p == 1 || p == 2) ? vi->format.subSamplingH : 0);
        frameSize += static_cast<size_t>(width) * static_cast<size_t>(height) * vi->format.bytesPerSample;
    }
    return frameSize;
}

static size_t getNutAudioFrameSize(const VSFrame *frame, const VSAPI *vsapi) {
    const VSAudioFormat *fi = vsapi->getAudioFrameFormat(frame);
    size_t bytesPerOutputSample = (fi->bitsPerSample + 7) / 8;
    size_t numSamples = static_cast<size_t>(vsapi->getFrameLength(frame));
    return bytesPerOutputSample * numSamples * static_cast<size_t>(fi->numChannels);
}

static int64_t convertAudioSamplesToNutTicks(int64_t samples, int sampleRate, int64_t timebaseNum, int64_t timebaseDen) {
    long double num = static_cast<long double>(samples) * static_cast<long double>(timebaseDen);
    long double den = static_cast<long double>(sampleRate) * static_cast<long double>(timebaseNum);
    long double ticks = num / den;
    return std::max<int64_t>(0, static_cast<int64_t>(ticks + 0.5L));
}

struct VSPipeNUTMuxStreamContext {
    VSNode *node = nullptr;
    int selectedIndex = -1;
    int streamId = -1;
    bool isVideo = false;
    const VSVideoInfo *videoInfo = nullptr;
    const VSAudioInfo *audioInfo = nullptr;
    size_t videoFrameSize = 0;
    int frameIndex = 0;
    int framesWritten = 0;
    int64_t audioSamplesWritten = 0;
    int64_t videoCurrentPts = 0;
    int64_t videoFallbackDurationTicks = 1;
    const VSFrame *pendingFrame = nullptr;
    int64_t pendingPts = 0;
};

static bool fetchNutStreamFrame(VSPipeNUTMuxStreamContext &stream, VSPipeOutputData *data) {
    int maxFrames = stream.isVideo ? stream.videoInfo->numFrames : stream.audioInfo->numFrames;
    if (stream.frameIndex >= maxFrames)
        return true;

    std::array<char, 1024> errorBuffer{};
    stream.pendingFrame = data->vsapi->getFrame(stream.frameIndex, stream.node, errorBuffer.data(), static_cast<int>(errorBuffer.size()));
    if (!stream.pendingFrame) {
        if (!errorBuffer[0])
            data->errorMessage = "Error: failed to retrieve frame " + std::to_string(stream.frameIndex) + " from selected output index " + std::to_string(stream.selectedIndex);
        else
            data->errorMessage = "Error: failed to retrieve frame " + std::to_string(stream.frameIndex) + " from selected output index " + std::to_string(stream.selectedIndex) + " with error: " + errorBuffer.data();
        data->outputError = true;
        return false;
    }

    if (stream.isVideo) {
        stream.pendingPts = getNutFramePts(stream.pendingFrame, data->vsapi, stream.videoCurrentPts, data->nutTimebaseNum, data->nutTimebaseDen);
    } else {
        stream.pendingPts = convertAudioSamplesToNutTicks(stream.audioSamplesWritten, stream.audioInfo->sampleRate, data->nutTimebaseNum, data->nutTimebaseDen);
    }

    return true;
}

static int selectNextNutStream(const std::vector<VSPipeNUTMuxStreamContext> &streams) {
    int selected = -1;

    for (size_t i = 0; i < streams.size(); i++) {
        if (!streams[i].pendingFrame)
            continue;

        if (selected < 0) {
            selected = static_cast<int>(i);
            continue;
        }

        if (streams[i].pendingPts < streams[selected].pendingPts || (streams[i].pendingPts == streams[selected].pendingPts && streams[i].streamId < streams[selected].streamId))
            selected = static_cast<int>(i);
    }

    return selected;
}

static bool outputNutStreamsNode(const VSPipeOptions &opts, VSPipeOutputData *data, std::vector<VSPipeNUTMuxStreamContext> &streams, int64_t totalVideoFramesExpected, int64_t totalAudioSamplesExpected) {
    data->outputFrames = 0;
    data->startTime = std::chrono::steady_clock::now();
    data->lastFPSReportTime = std::chrono::steady_clock::now();

    for (auto &stream : streams) {
        if (!fetchNutStreamFrame(stream, data))
            break;
    }

    while (!data->outputError) {
        int nextStreamIndex = selectNextNutStream(streams);
        if (nextStreamIndex < 0)
            break;

        VSPipeNUTMuxStreamContext &stream = streams[nextStreamIndex];
        size_t frameSize = stream.isVideo ? stream.videoFrameSize : getNutAudioFrameSize(stream.pendingFrame, data->vsapi);

        if (!data->nutWriter->writeFrameHeader(stream.streamId, stream.pendingPts, frameSize, true, data->errorMessage)) {
            data->outputError = true;
            break;
        }

        outputFrame(stream.pendingFrame, data);
        if (data->outputError)
            break;

        if (stream.isVideo) {
            stream.videoCurrentPts = stream.pendingPts + getNutFrameDurationTicks(stream.pendingFrame, data->vsapi, stream.videoFallbackDurationTicks, data->nutTimebaseNum, data->nutTimebaseDen);
        } else {
            stream.audioSamplesWritten += data->vsapi->getFrameLength(stream.pendingFrame);
        }

        data->vsapi->freeFrame(stream.pendingFrame);
        stream.pendingFrame = nullptr;
        stream.frameIndex++;
        stream.framesWritten++;
        data->outputFrames++;

        if (!fetchNutStreamFrame(stream, data))
            break;

        if (opts.printProgress) {
            std::chrono::time_point<std::chrono::steady_clock> currentTime(std::chrono::steady_clock::now());
            std::chrono::duration<double> elapsedSeconds = currentTime - data->lastFPSReportTime;
            if (elapsedSeconds.count() > .5) {
                int64_t videoFramesWritten = 0;
                int64_t audioSamplesWritten = 0;
                for (const auto &iter : streams) {
                    if (iter.isVideo)
                        videoFramesWritten += iter.framesWritten;
                    else
                        audioSamplesWritten += iter.audioSamplesWritten;
                }
                data->lastFPSReportTime = currentTime;
                fprintf(stderr, "Video frame: %" PRId64 "/%" PRId64 " Audio sample: %" PRId64 "/%" PRId64 "\r", videoFramesWritten, totalVideoFramesExpected, audioSamplesWritten, totalAudioSamplesExpected);
            }
        }
    }

    for (auto &stream : streams) {
        if (stream.pendingFrame) {
            data->vsapi->freeFrame(stream.pendingFrame);
            stream.pendingFrame = nullptr;
        }
    }

    if (data->outputError && !data->errorMessage.empty())
        fprintf(stderr, "%s\n", data->errorMessage.c_str());

    int64_t totalVideoFramesWritten = 0;
    int64_t totalAudioSamplesWritten = 0;
    for (const auto &stream : streams) {
        if (stream.isVideo)
            totalVideoFramesWritten += stream.framesWritten;
        else
            totalAudioSamplesWritten += stream.audioSamplesWritten;
    }

    data->totalFrames = static_cast<int>(totalVideoFramesWritten);
    data->totalSamples = totalAudioSamplesWritten;

    return data->outputError;
}

static void VS_CC frameDoneCallback(void *userData, const VSFrame *f, int n, VSNode *rnode, const char *errorMsg) {
    VSPipeOutputData *data = reinterpret_cast<VSPipeOutputData *>(userData);

    bool printToConsole = false;
    bool hasMeaningfulFPS = false;
    double fps = 0;

    if (data->printProgress) {
        printToConsole = (n == 0);

        std::chrono::time_point<std::chrono::steady_clock> currentTime(std::chrono::steady_clock::now());
        std::chrono::duration<double> elapsedSeconds = currentTime - data->lastFPSReportTime;
        std::chrono::duration<double> elapsedSecondsFromStart = currentTime - data->startTime;

        if (elapsedSeconds.count() > .5) {
            printToConsole = true;
            data->lastFPSReportTime = currentTime;
        }

        if (elapsedSecondsFromStart.count() > 8) {
            hasMeaningfulFPS = true;
            fps = data->completedFrames / elapsedSecondsFromStart.count();
        }
    }

    // completed frames simply correspond to how many times the completion callback is called
    if (rnode == data->node) {
        data->completedFrames++;
        if (!data->alphaNode)
            data->completedAlphaFrames++;
    } else {
        data->completedAlphaFrames++;
    }

    if (f) {
        if (rnode == data->node)
            data->reorderMap[n].first = f;
        else
            data->reorderMap[n].second = f;

        bool completed = isCompletedFrame(data->reorderMap[n], !!data->alphaNode);

        if (completed && data->requestedFrames < data->totalFrames) {
            data->vsapi->getFrameAsync(data->requestedFrames, data->node, frameDoneCallback, userData);
            if (data->alphaNode)
                data->vsapi->getFrameAsync(data->requestedFrames, data->alphaNode, frameDoneCallback, userData);
            data->requestedFrames++;
        }

        while (data->reorderMap.count(data->outputFrames) && isCompletedFrame(data->reorderMap[data->outputFrames], !!data->alphaNode)) {
            const VSFrame *frame = data->reorderMap[data->outputFrames].first;
            const VSFrame *alphaFrame = data->reorderMap[data->outputFrames].second;
            data->reorderMap.erase(data->outputFrames);
            if (!data->outputError) {
                if (data->outputHeaders == VSPipeHeaders::Y4M && data->outFile) {
                    if (fwrite("FRAME\n", 1, 6, data->outFile) != 6) {
                        if (data->errorMessage.empty())
                            data->errorMessage = "Error: fwrite() call failed when writing header, errno: " + std::to_string(errno);
                        data->totalFrames = data->requestedFrames;
                        data->outputError = true;
                    }
                } else if (data->outputHeaders == VSPipeHeaders::NUT && data->nutWriter) {
                    if (data->vsapi->getFrameType(frame) == mtVideo) {
                        int64_t framePts = getNutFramePts(frame, data->vsapi, data->nutCurrentPts, data->nutTimebaseNum, data->nutTimebaseDen);
                        if (!data->nutWriter->writeFrameHeader(0, framePts, data->nutFrameSize, true, data->errorMessage)) {
                            data->totalFrames = data->requestedFrames;
                            data->outputError = true;
                        }
                        data->nutCurrentPts = framePts + getNutFrameDurationTicks(frame, data->vsapi, data->nutFallbackDurationTicks, data->nutTimebaseNum, data->nutTimebaseDen);
                    } else if (data->vsapi->getFrameType(frame) == mtAudio) {
                        int64_t framePts = convertAudioSamplesToNutTicks(data->nutAudioSamplesWritten, data->vsapi->getAudioInfo(data->node)->sampleRate, data->nutTimebaseNum, data->nutTimebaseDen);
                        size_t frameSize = getNutAudioFrameSize(frame, data->vsapi);
                        if (!data->nutWriter->writeFrameHeader(0, framePts, frameSize, true, data->errorMessage)) {
                            data->totalFrames = data->requestedFrames;
                            data->outputError = true;
                        }
                    }
                }

                outputFrame(frame, data);
                if (data->outputHeaders == VSPipeHeaders::NUT && data->vsapi->getFrameType(frame) == mtAudio)
                    data->nutAudioSamplesWritten += data->vsapi->getFrameLength(frame);
                if (alphaFrame)
                    outputFrame(alphaFrame, data);

                if (data->timecodesFile && !data->outputError) {

                    if (fprintf(data->timecodesFile, "%s\n", doubleToString(data->getCurrentTimecode() * 1000).c_str()) < 0) {
                        if (data->errorMessage.empty())
                            data->errorMessage = "Error: failed to write timecode for frame " + std::to_string(data->outputFrames) + ". errno: " + std::to_string(errno);
                        data->totalFrames = data->requestedFrames;
                        data->outputError = true;
                    } else {
                        const VSMap *props = data->vsapi->getFramePropertiesRO(frame);
                        int err_num, err_den;
                        int64_t duration_num = data->vsapi->mapGetInt(props, "_DurationNum", 0, &err_num);
                        int64_t duration_den = data->vsapi->mapGetInt(props, "_DurationDen", 0, &err_den);

                        if (err_num || err_den || duration_den <= 0 || duration_num <= 0) {
                            if (data->errorMessage.empty()) {
                                if (err_num || err_den)
                                    data->errorMessage = "Error: missing duration at frame ";
                                else if (duration_num <= 0)
                                    data->errorMessage = "Error: duration numerator is zero or negative at frame ";
                                else if (duration_den <= 0)
                                    data->errorMessage = "Error: duration denominator is zero or negative at frame ";
                                data->errorMessage += std::to_string(data->outputFrames);
                            }

                            data->totalFrames = data->requestedFrames;
                            data->outputError = true;
                        } else {
                            ++data->currentTimecode[std::make_pair(duration_num, duration_den)];
                        }
                    }
                }

                if (data->jsonFile && !data->outputError) {
                    if (fprintf(data->jsonFile, "\t%s%s\n", convertVSMapToJSON(data->vsapi->getFramePropertiesRO(frame), data->vsapi).c_str(), (data->totalFrames - 1 != data->outputFrames) ? "," : "") < 0) {
                        if (data->errorMessage.empty())
                            data->errorMessage = "Error: failed to write JSON for frame " + std::to_string(data->outputFrames) + ". errno: " + std::to_string(errno);
                        data->totalFrames = data->requestedFrames;
                        data->outputError = true;
                    }
                }
            }
            data->vsapi->freeFrame(frame);
            data->vsapi->freeFrame(alphaFrame);
            data->outputFrames++;
        }
    } else {
        data->outputError = true;
        data->totalFrames = data->requestedFrames;
        if (data->errorMessage.empty()) {
            if (errorMsg)
                data->errorMessage = "Error: Failed to retrieve frame " + std::to_string(n) + " with error: " + errorMsg;
            else
                data->errorMessage = "Error: Failed to retrieve frame " + std::to_string(n);
        }
    }

    if (printToConsole && !data->outputError) {
        if (data->vsapi->getNodeType(rnode) == mtVideo) {
            if (hasMeaningfulFPS)
                fprintf(stderr, "Frame: %d/%d (%.2f fps)\r", data->completedFrames, data->totalFrames, fps);
            else
                fprintf(stderr, "Frame: %d/%d\r", data->completedFrames, data->totalFrames);
        } else {
            if (hasMeaningfulFPS)
                fprintf(stderr, "Sample: %" PRId64 "/%" PRId64 " (%.2f sps)\r", static_cast<int64_t>(data->completedFrames * VS_AUDIO_FRAME_SAMPLES), static_cast<int64_t>(data->totalFrames * VS_AUDIO_FRAME_SAMPLES), fps);
            else
                fprintf(stderr, "Sample: %" PRId64 "/%" PRId64 "\r", static_cast<int64_t>(data->completedFrames * VS_AUDIO_FRAME_SAMPLES), static_cast<int64_t>(data->totalFrames * VS_AUDIO_FRAME_SAMPLES));
        }
    }

    if (data->totalFrames == data->completedFrames && data->totalFrames == data->completedAlphaFrames) {
        std::lock_guard<std::mutex> lock(data->mutex);
        data->condition.notify_one();
    }
}

static std::string floatBitsToLetter(int bits) {
    switch (bits) {
    case 16:
        return "h";
    case 32:
        return "s";
    case 64:
        return "d";
    default:
        assert(false);
        return "u";
    }
}

static bool initializeVideoOutput(VSPipeOutputData *data) {
    if (data->outputHeaders != VSPipeHeaders::None && data->outputHeaders != VSPipeHeaders::Y4M && data->outputHeaders != VSPipeHeaders::NUT) {
        fprintf(stderr, "Error: can't apply selected header type to video\n");
        return false;
    }

    const VSVideoInfo *vi = data->vsapi->getVideoInfo(data->node);

    if (data->outputHeaders == VSPipeHeaders::Y4M && ((vi->format.colorFamily != cfGray && vi->format.colorFamily != cfYUV) || data->alphaNode)) {
        fprintf(stderr, "Error: can only apply y4m headers to YUV and Gray format clips without alpha\n");
        return false;
    }

    std::string y4mFormat;

    if (data->outputHeaders == VSPipeHeaders::Y4M) {
        if (vi->format.colorFamily == cfGray) {
            y4mFormat = "mono";
            if (vi->format.bitsPerSample > 8)
                y4mFormat = y4mFormat + std::to_string(vi->format.bitsPerSample);
        } else if (vi->format.colorFamily == cfYUV) {
            if (vi->format.subSamplingW == 1 && vi->format.subSamplingH == 1)
                y4mFormat = "420";
            else if (vi->format.subSamplingW == 1 && vi->format.subSamplingH == 0)
                y4mFormat = "422";
            else if (vi->format.subSamplingW == 0 && vi->format.subSamplingH == 0)
                y4mFormat = "444";
            else if (vi->format.subSamplingW == 2 && vi->format.subSamplingH == 2)
                y4mFormat = "410";
            else if (vi->format.subSamplingW == 2 && vi->format.subSamplingH == 0)
                y4mFormat = "411";
            else if (vi->format.subSamplingW == 0 && vi->format.subSamplingH == 1)
                y4mFormat = "440";
            else {
                fprintf(stderr, "Error: no y4m identifier exists for current format\n");
                return false;
            }

            if (vi->format.bitsPerSample > 8 && vi->format.sampleType == stInteger)
                y4mFormat += "p" + std::to_string(vi->format.bitsPerSample);
            else if (vi->format.sampleType == stFloat)
                y4mFormat += "p" + floatBitsToLetter(vi->format.bitsPerSample);
        } else {
            fprintf(stderr, "Error: no y4m identifier exists for current format\n");
            return false;
        }

        if (!y4mFormat.empty())
            y4mFormat = " C" + y4mFormat;

        std::string header = "YUV4MPEG2" + y4mFormat
            + " W" + std::to_string(vi->width)
            + " H" + std::to_string(vi->height)
            + " F" + std::to_string(vi->fpsNum) + ":" + std::to_string(vi->fpsDen)
            + " Ip A0:0"
            + " XLENGTH=" + std::to_string(vi->numFrames) + "\n";

        if (data->outFile) {
            if (fwrite(header.c_str(), 1, header.size(), data->outFile) != header.size()) {
                fprintf(stderr, "Error: fwrite() call failed when writing initial header, errno: %d\n", errno);
                return false;
            }
        }
    } else if (data->outputHeaders == VSPipeHeaders::NUT) {
        std::array<uint8_t, 4> fourCC{};
        if (!VSPipeNUTWriter::getVideoFourCC(vi->format, fourCC)) {
            fprintf(stderr, "Error: no supported NUT fourcc exists for current video format\n");
            return false;
        }

        data->nutFrameSize = getNutVideoFrameSize(vi);
        data->nutCurrentPts = 0;
        data->nutFallbackDurationTicks = 1;
        if (vi->fpsNum > 0 && vi->fpsDen > 0)
            data->nutFallbackDurationTicks = convertDurationToNutTicks(vi->fpsDen, vi->fpsNum, data->nutTimebaseNum, data->nutTimebaseDen);

        data->nutWriter.reset(new VSPipeNUTWriter());
        VSPipeNUTStreamInfo streamInfo;
        streamInfo.type = VSPipeNUTStreamType::Video;
        streamInfo.fourCC = fourCC;
        streamInfo.width = vi->width;
        streamInfo.height = vi->height;
        std::vector<VSPipeNUTStreamInfo> streams;
        streams.push_back(streamInfo);
        if (!data->nutWriter->initialize(data->outFile, streams, data->errorMessage)) {
            if (!data->errorMessage.empty())
                fprintf(stderr, "%s\n", data->errorMessage.c_str());
            else
                fprintf(stderr, "Error: failed to initialize NUT output\n");
            return false;
        }
    }

    if (data->timecodesFile && !data->outputError) {
        if (fprintf(data->timecodesFile, "# timecode format v2\n") < 0) {
            fprintf(stderr, "Error: failed to write timecodes file header, errno: %d\n", errno);
            return false;
        }
    }

    if (data->jsonFile && !data->outputError) {
        if (fprintf(data->jsonFile, "%s", "[\n") < 0) {
            fprintf(stderr, "Error: failed to write JSON file header, errno: %d\n", errno);
            return false;
        }
    }

    data->buffer.resize(vi->width * vi->height * vi->format.bytesPerSample);
    return true;
}

static bool finalizeVideoOutput(VSPipeOutputData *data) {
    if (data->jsonFile && !data->outputError) {
        if (fprintf(data->jsonFile, "%s", "]\n") < 0) {
            fprintf(stderr, "Error: failed to finalize JSON file, errno: %d\n", errno);
            return false;
        }
    }

    return true;
}

static bool initializeAudioOutput(VSPipeOutputData *data) {
    if (data->outputHeaders != VSPipeHeaders::None && data->outputHeaders != VSPipeHeaders::WAVE && data->outputHeaders != VSPipeHeaders::WAVE64 && data->outputHeaders != VSPipeHeaders::NUT) {
        fprintf(stderr, "Error: can't apply selected header type to audio\n");
        return false;
    }

    const VSAudioInfo *ai = data->vsapi->getAudioInfo(data->node);

    if (data->outputHeaders == VSPipeHeaders::WAVE64) {
        Wave64Header header;
        if (!CreateWave64Header(header, ai->format.sampleType == stFloat, ai->format.bitsPerSample, ai->sampleRate, ai->format.channelLayout, ai->numSamples)) {
            fprintf(stderr, "Error: cannot create valid w64 header\n");
            return false;
        }
        if (data->outFile) {
            if (fwrite(&header, 1, sizeof(header), data->outFile) != sizeof(header)) {
                fprintf(stderr, "Error: fwrite() call failed when writing initial header, errno: %d\n", errno);
                return false;
            }
        }
    } else if (data->outputHeaders == VSPipeHeaders::WAVE) {
        WaveHeader header;
        if (!CreateWaveHeader(header, ai->format.sampleType == stFloat, ai->format.bitsPerSample, ai->sampleRate, ai->format.channelLayout, ai->numSamples)) {
            fprintf(stderr, "Error: cannot create valid wav header\n");
            return false;
        }

        if (data->outFile) {
            if (fwrite(&header, 1, sizeof(header), data->outFile) != sizeof(header)) {
                fprintf(stderr, "Error: fwrite() call failed when writing initial header, errno: %d\n", errno);
                return false;
            }
        }
    } else if (data->outputHeaders == VSPipeHeaders::NUT) {
        std::array<uint8_t, 4> fourCC{};
        if (!VSPipeNUTWriter::getAudioFourCC(ai->format, fourCC)) {
            fprintf(stderr, "Error: no supported NUT fourcc exists for current audio format\n");
            return false;
        }
        if (ai->sampleRate <= 0) {
            fprintf(stderr, "Error: NUT output requires a positive audio sample rate\n");
            return false;
        }
        if (ai->format.numChannels <= 0) {
            fprintf(stderr, "Error: NUT output requires at least one audio channel\n");
            return false;
        }

        data->nutWriter.reset(new VSPipeNUTWriter());
        VSPipeNUTStreamInfo streamInfo;
        streamInfo.type = VSPipeNUTStreamType::Audio;
        streamInfo.fourCC = fourCC;
        streamInfo.sampleRateNum = ai->sampleRate;
        streamInfo.sampleRateDen = 1;
        streamInfo.channelCount = ai->format.numChannels;
        std::vector<VSPipeNUTStreamInfo> streams;
        streams.push_back(streamInfo);
        if (!data->nutWriter->initialize(data->outFile, streams, data->errorMessage)) {
            if (!data->errorMessage.empty())
                fprintf(stderr, "%s\n", data->errorMessage.c_str());
            else
                fprintf(stderr, "Error: failed to initialize NUT output\n");
            return false;
        }
        data->nutAudioSamplesWritten = 0;
    }

    data->buffer.resize(ai->format.numChannels * VS_AUDIO_FRAME_SAMPLES * ai->format.bytesPerSample);
    return true;
}

static bool outputNode(const VSPipeOptions &opts, VSPipeOutputData *data, VSCore *core) {
    int requests = opts.requests;
    if (requests < 1) {
        VSCoreInfo info;
        data->vsapi->getCoreInfo(core, &info);
        requests = info.numThreads;
    }

    data->startTime = std::chrono::steady_clock::now();
    data->lastFPSReportTime = std::chrono::steady_clock::now();

    std::unique_lock<std::mutex> lock(data->mutex);

    int intitalRequestSize = std::min(requests, data->totalFrames);
    data->requestedFrames = intitalRequestSize;
    for (int n = 0; n < intitalRequestSize; n++) {
        data->vsapi->getFrameAsync(n, data->node, frameDoneCallback, data);
        if (data->alphaNode)
            data->vsapi->getFrameAsync(n, data->alphaNode, frameDoneCallback, data);
    }

    data->condition.wait(lock, [&]() { return data->totalFrames == data->completedFrames && data->totalFrames == data->completedAlphaFrames; });

    if (data->outputError) {
        for (auto &iter : data->reorderMap) {
            data->vsapi->freeFrame(iter.second.first);
            data->vsapi->freeFrame(iter.second.second);
        }
        fprintf(stderr, "%s\n", data->errorMessage.c_str());
    }

    return data->outputError;
}

static const char *colorFamilyToString(int colorFamily) {
    switch (colorFamily) {
    case cfGray: return "Gray";
    case cfRGB: return "RGB";
    case cfYUV: return "YUV";
    }
    return "Error";
}

static bool svToInt64(const std::string_view &s, int64_t &result) {
    auto res = std::from_chars(s.data(), s.data() + s.size(), result);
    return (res.ptr == s.data() + s.size());
}

static bool svToInt(const std::string_view &s, int &result) {
    auto res = std::from_chars(s.data(), s.data() + s.size(), result);
    return (res.ptr == s.data() + s.size());
}

static std::string_view trimStringView(const std::string_view &s) {
    size_t start = 0;
    size_t end = s.size();

    while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
        start++;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        end--;

    return s.substr(start, end - start);
}

static bool svToIntList(const std::string_view &s, std::vector<int> &result) {
    result.clear();

    if (s.empty())
        return false;

    size_t start = 0;
    while (start <= s.size()) {
        size_t commaPos = s.find(',', start);
        size_t tokenEnd = (commaPos == std::string_view::npos) ? s.size() : commaPos;
        std::string_view token = trimStringView(s.substr(start, tokenEnd - start));

        if (token.empty())
            return false;

        int value = 0;
        if (!svToInt(token, value) || value < 0)
            return false;
        result.push_back(value);

        if (commaPos == std::string_view::npos)
            break;
        start = commaPos + 1;
    }

    return !result.empty();
}

static bool printVersion(const VSAPI *vsapi) {
    VSCore *core = vsapi->createCore(0);
    if (!core) {
        fprintf(stderr, "Failed to create core\n");
        return false;
    }

    VSCoreInfo info;
    vsapi->getCoreInfo(core, &info);
    printf("%s", info.versionString);
    vsapi->freeCore(core);
    return true;
}

static void printHelp() {
    fprintf(stderr,
        "VSPipe R" XSTR(VAPOURSYNTH_CORE_VERSION) " usage:\n"
        "  vspipe [options] <script> <outfile>\n"
        "\n"
        "Available options:\n"
        "  -a, --arg key=value               Argument to pass to the script environment\n"
        "  -s, --start N                     Set output frame/sample range start\n"
        "  -e, --end N                       Set output frame/sample range end (inclusive)\n"
        "  -o, --outputindex N[,M,...]       Select output index or comma-separated output indices\n"
        "  -u, --audio-outputindex N[,M,...] Select secondary audio output index or comma-separated audio indices for NUT muxing\n"
        "  -r, --requests N                  Set number of concurrent frame requests\n"
        "  -c, --container <y4m/nut/wav/w64> Add headers for the specified format to the output (use nut to carry multiple streams)\n"
        "  -t, --timecodes FILE              Write timecodes v2 file\n"
        "  -j, --json FILE                   Write properties of output frames in json format to file\n"
        "  -p, --progress                    Print progress to stderr\n"
        "      --filter-time                 Print time spent in individual filters to stderr after processing\n"
        "      --filter-time-graph FILE      Write output node's filter graph in dot format with time information after processing\n"
        "      --no-dep-warn                 Hide API3 deprecation plugin warnings\n"
        "  -i, --info                        Print all set output node info to <outfile> and exit\n"
        "  -g  --graph <simple/full>         Print output node's filter graph in dot format to <outfile> and exit\n"
        "      --frame-ref-debug             Print frame allocation debug information\n"
        "  -v, --version                     Show version info and exit\n"
        "\n"
        "Special output options for <outfile>:\n"
        "  -                                 Write to stdout\n"
        "  --                                No output\n"
        "\n"
        "Examples:\n"
        "  Show script info:\n"
        "    vspipe --info script.vpy\n"
        "  Write to stdout:\n"
        "    vspipe [options] script.vpy -\n"
#ifdef _WIN32
        "  Write to a named pipe (Windows only):\n"
        "    vspipe [options] script.vpy \"\\\\.\\pipe\\<pipename>\"\n"
#endif
        "  Request all frames but don't output them:\n"
        "    vspipe [options] script.vpy --\n"
        "  Write frames 5-100 to file:\n"
        "    vspipe --start 5 --end 100 script.vpy output.raw\n"
        "  Pass values to a script:\n"
        "    vspipe --arg deinterlace=yes --arg \"message=fluffy kittens\" script.vpy output.raw\n"
        "  Pipe to x264 and write timecodes file:\n"
        "    vspipe script.vpy - -c y4m --timecodes timecodes.txt | x264 --demuxer y4m -o script.mkv -\n"
        );
}

static int parseOptions(VSPipeOptions &opts, int argc, char **argv) {
    for (int arg = 1; arg < argc; arg++) {
        std::string_view argString = argv[arg];
        if (argString == "-v" || argString == "--version") {
            if (argc > 2) {
                fprintf(stderr, "Cannot combine version information with other options\n");
                return 1;
            }

            opts.mode = VSPipeMode::PrintVersion;
        } else if (argString == "-c" || argString == "--container") {
            if (argc <= arg + 1) {
                fprintf(stderr, "No container type specified\n");
                return 1;
            }

            std::string_view optString = argv[arg + 1];
            if (optString == "y4m") {
                opts.outputHeaders = VSPipeHeaders::Y4M;
            } else if (optString == "nut") {
                opts.outputHeaders = VSPipeHeaders::NUT;
            } else if (optString == "wav") {
                opts.outputHeaders = VSPipeHeaders::WAVE;
            } else if (optString == "w64") {
                opts.outputHeaders = VSPipeHeaders::WAVE64;
            } else {
                fprintf(stderr, "Unknown container type specified: %s\n", argv[arg + 1]);
                return 1;
            }

            arg++;
        } else if (argString == "-p" || argString == "--progress") {
            opts.printProgress = true;
        } else if (argString == "--frame-ref-debug") {
            opts.frameRefDebug = true;
        } else if (argString == "--no-dep-warn") {
            opts.noDepWarn = true;
        } else if (argString == "--filter-time") {
            opts.printFilterTime = true;
        } else if (argString == "--filter-time-graph") {
            if (argc <= arg + 1) {
                fprintf(stderr, "No filter time graph file specified\n");
                return 1;
            }

            opts.filterTimeGraphFilename = std::filesystem::u8path(argv[arg + 1]);

            arg++;
        } else if (argString == "-i" || argString == "--info") {
            if (opts.mode == VSPipeMode::PrintSimpleGraph || opts.mode == VSPipeMode::PrintFullGraph) {
                fprintf(stderr, "Cannot combine graph and info arguments\n");
                return 1;
            }

            opts.mode = VSPipeMode::PrintInfo;
        } else if (argString == "-g" || argString == "--graph") {
            if (opts.mode == VSPipeMode::PrintInfo) {
                fprintf(stderr, "Cannot combine graph and info arguments\n");
                return 1;
            }

            if (argc <= arg + 1) {
                fprintf(stderr, "No graph type specified\n");
                return 1;
            }

            std::string_view optString = argv[arg + 1];
            if (optString == "simple") {
                opts.mode = VSPipeMode::PrintSimpleGraph;
            } else if (optString == "full") {
                opts.mode = VSPipeMode::PrintFullGraph;
            } else {
                fprintf(stderr, "Unknown graph type specified: %s\n", argv[arg + 1]);
                return 1;
            }

            arg++;
        } else if (argString == "-h" || argString == "--help") {
            if (argc > 2) {
                fprintf(stderr, "Cannot combine help with other options\n");
                return 1;
            }

            opts.mode = VSPipeMode::PrintHelp;
        } else if (argString == "-s" || argString == "--start") {
            if (argc <= arg + 1) {
                fprintf(stderr, "No start frame specified\n");
                return 1;
            }

            if (!svToInt64(argv[arg + 1], opts.startPos)) {
                fprintf(stderr, "Couldn't convert %s to an integer (start)\n", argv[arg + 1]);
                return 1;
            }

            if (opts.startPos < 0) {
                fprintf(stderr, "Negative start position specified\n");
                return 1;
            }

            arg++;
        } else if (argString == "-e" || argString == "--end") {
            if (argc <= arg + 1) {
                fprintf(stderr, "No end frame specified\n");
                return 1;
            }

            if (!svToInt64(argv[arg + 1], opts.endPos)) {
                fprintf(stderr, "Couldn't convert %s to an integer (end)\n", argv[arg + 1]);
                return 1;
            }

            if (opts.endPos < 0) {
                fprintf(stderr, "Negative end frame specified\n");
                return 1;
            }

            arg++;
        } else if (argString == "-o" || argString == "--outputindex") {
            if (argc <= arg + 1) {
                fprintf(stderr, "No output index specified\n");
                return 1;
            }

            std::vector<int> parsedIndices;
            if (!svToIntList(argv[arg + 1], parsedIndices)) {
                fprintf(stderr, "Couldn't parse %s as a comma-separated index list\n", argv[arg + 1]);
                return 1;
            }
            opts.outputIndices = std::move(parsedIndices);
            opts.outputIndex = opts.outputIndices.front();
            opts.outputIndexExplicit = true;

            arg++;
        } else if (argString == "-u" || argString == "--audio-outputindex") {
            if (argc <= arg + 1) {
                fprintf(stderr, "No audio output index specified\n");
                return 1;
            }

            std::vector<int> parsedIndices;
            if (!svToIntList(argv[arg + 1], parsedIndices)) {
                fprintf(stderr, "Couldn't parse %s as a comma-separated audio index list\n", argv[arg + 1]);
                return 1;
            }
            opts.audioOutputIndices = std::move(parsedIndices);
            opts.audioOutputIndex = opts.audioOutputIndices.front();
            opts.audioOutputIndexExplicit = true;

            arg++;
        } else if (argString == "-r" || argString == "--requests") {
            if (argc <= arg + 1) {
                fprintf(stderr, "Number of requests not specified\n");
                return 1;
            }

            if (!svToInt(argv[arg + 1], opts.requests)) {
                fprintf(stderr, "Couldn't convert %s to an integer (requests)\n", argv[arg + 1]);
                return 1;
            }

            arg++;
        } else if (argString == "-a" || argString == "--arg") {
            if (argc <= arg + 1) {
                fprintf(stderr, "No argument specified\n");
                return 1;
            }

            std::string_view optString = argv[arg + 1];
            size_t equalsPos = optString.find("=");
            if (equalsPos == std::string::npos) {
                fprintf(stderr, "No value specified for argument: %s\n", argv[arg + 1]);
                return 1;
            }

            opts.scriptArgs[std::string(optString.substr(0, equalsPos))] = optString.substr(equalsPos + 1);

            arg++;
        } else if (argString == "-t" || argString == "--timecodes") {
            if (argc <= arg + 1) {
                fprintf(stderr, "No timecodes file specified\n");
                return 1;
            }

            opts.timecodesFilename = std::filesystem::u8path(argv[arg + 1]);

            arg++;
        } else if (argString == "-j" || argString == "--json") {
            if (argc <= arg + 1) {
                fprintf(stderr, "No JSON file specified\n");
                return 1;
            }

            opts.jsonFilename = std::filesystem::u8path(argv[arg + 1]);

            arg++;
        } else if (opts.scriptFilename.empty() && !argString.empty() && argString.substr(0, 1) != "-") {
            opts.scriptFilename = std::filesystem::u8path(argString);
        } else if (opts.outputFilename.empty() && !argString.empty() && (argString == "-" || argString == "--" || (argString.substr(0, 1) != "-"))) {
            opts.outputFilename = std::filesystem::u8path(argString);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[arg]);
            return 1;
        }
    }

    // Print help if no options provided
    if (argc <= 1)
        opts.mode = VSPipeMode::PrintHelp;

    if ((opts.mode == VSPipeMode::Output || opts.mode == VSPipeMode::PrintInfo || opts.mode == VSPipeMode::PrintSimpleGraph || opts.mode == VSPipeMode::PrintFullGraph) && opts.scriptFilename.empty()) {
        fprintf(stderr, "No script file specified\n");
        return 1;
    } else if (opts.mode == VSPipeMode::Output && opts.outputFilename.empty()) {
        fprintf(stderr, "No output file specified\n");
        return 1;
    }

    if (!opts.outputIndexExplicit)
        opts.outputIndices.push_back(opts.outputIndex);

    return 0;
}

#ifdef VS_TARGET_OS_WINDOWS
static std::string utf16_to_utf8(const std::wstring &wstr) {
    int required_size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string buffer;
    buffer.resize(required_size - 1);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &buffer[0], required_size, nullptr, nullptr);
    return buffer;
}

static int main8(int argc, char **argv);

int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> argv8storage;
    std::vector<char *> argv8;
    for (int i = 0; i < argc; i++)
        argv8storage.push_back(utf16_to_utf8(argv[i]));
    for (int i = 0; i < argc; i++)
        argv8.push_back(const_cast<char *>(argv8storage[i].c_str()));
    return main8(argc, argv8.data());
}

static int main8(int argc, char **argv) {
    if (GetConsoleOutputCP() != CP_UTF8 && !SetConsoleOutputCP(CP_UTF8))
        fprintf(stderr, "Failed to set UTF-8 console codepage, some characters may not be correctly displayed\n");
#else
int main(int argc, char **argv) {
#endif
    const VSSCRIPTAPI *vssapi = getVSScriptAPI(VSSCRIPT_API_VERSION);
    if (!vssapi) {
        fprintf(stderr, "Failed to initialize VSScript. VSScript reported error: %s\n", getVSScriptAPILastError());
        return 1;
    }

    const VSAPI *vsapi = vssapi->getVSAPI(VAPOURSYNTH_API_VERSION);
    if (!vsapi) {
        fprintf(stderr, "Failed to get VapourSynth API pointer\n");
        return 1;
    }

    // Set the signal handler AFTER python initialization because it handles signals even if you tell it not to
#ifdef VS_TARGET_OS_WINDOWS
    if (!SetConsoleCtrlHandler(HandlerRoutine, TRUE))
        fprintf(stderr, "Failed to register signal handler\n");
#else
    struct sigaction sa{};
    sa.sa_handler = HandlerRoutine;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, nullptr) != 0)
        fprintf(stderr, "Failed to register signal handler\n");
#endif

    VSPipeOptions opts{};
    int parseResult = parseOptions(opts, argc, argv);
    if (parseResult)
        return parseResult;
    if (opts.mode == VSPipeMode::Output && !opts.audioOutputIndices.empty() && opts.outputHeaders != VSPipeHeaders::NUT) {
        fprintf(stderr, "Error: --audio-outputindex can only be used together with -c nut\n");
        return 1;
    }

    if (opts.mode == VSPipeMode::PrintVersion) {
        return printVersion(vsapi) ? 0 : 1;
    } else if (opts.mode == VSPipeMode::PrintHelp) {
        printHelp();
        return 0;
    }

    FILE *outFile = nullptr;
    bool closeOutFile = false;

    if (opts.outputFilename.empty() || opts.outputFilename == "-") {
        outFile = stdout;
    } else if (opts.outputFilename == "." || opts.outputFilename == "--") {
        // do nothing
#ifdef _WIN32
    } else if (opts.outputFilename.u8string().substr(0, 9) == "\\\\.\\pipe\\") {
        if (opts.outputFilename.u8string().length() <= 9) {
            fprintf(stderr, "Pipe name can't be empty\n");
            return 1;
        }

        HANDLE outFile2 = CreateNamedPipeW(opts.outputFilename.c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_ACCEPT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 1024 * 1024, 0, 0, nullptr);
        if (outFile2 == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "Failed to create pipe \"%s\"\n", opts.outputFilename.u8string().c_str());
            return 1;
        }

        fprintf(stderr, "Waiting for client to connect to named pipe...\n");
        if (ConnectNamedPipe(outFile2, nullptr) ? true : (GetLastError() == ERROR_PIPE_CONNECTED)) {
            fprintf(stderr, "Client connected to named pipe\n");
        } else {
            fprintf(stderr, "Client failed to connect to pipe, error code: %d\n", static_cast<int>(GetLastError()));
            return 1;
        }

        outFile = _fdopen(_open_osfhandle(reinterpret_cast<intptr_t>(outFile2), 0), "wb");
#endif
    } else {
        outFile = OpenFile(opts.outputFilename);
        if (!outFile) {
            fprintf(stderr, "Failed to open output for writing\n");
            return 1;
        }
        closeOutFile = true;
    }

    FILE *timecodesFile = nullptr;
    if (opts.mode == VSPipeMode::Output && !opts.timecodesFilename.empty()) {
        timecodesFile = OpenFile(opts.timecodesFilename);
        if (!timecodesFile) {
            fprintf(stderr, "Failed to open timecodes file for writing\n");
            return 1;
        }
    }

    FILE *jsonFile = nullptr;
    if (opts.mode == VSPipeMode::Output && !opts.jsonFilename.empty()) {
        jsonFile = OpenFile(opts.jsonFilename);
        if (!jsonFile) {
            fprintf(stderr, "Failed to open JSON file for writing\n");
            return 1;
        }
    }

    FILE *filterTimeGraphFile = nullptr;
    if (opts.mode == VSPipeMode::Output && !opts.filterTimeGraphFilename.empty()) {
        filterTimeGraphFile = OpenFile(opts.filterTimeGraphFilename);
        if (!filterTimeGraphFile) {
            fprintf(stderr, "Failed to open filter time graph file for writing\n");
            return 1;
        }
    }

    vsapi = vssapi->getVSAPI(VAPOURSYNTH_API_VERSION);
    if (!vsapi) {
        fprintf(stderr, "Failed to get VapourSynth API pointer\n");
        return 1;
    }

    std::chrono::time_point<std::chrono::steady_clock> scriptEvaluationStart = std::chrono::steady_clock::now();

    int creationFlags = 0;
    if (opts.frameRefDebug)
        creationFlags |= ccfEnableFrameRefDebug;
    if (opts.mode == VSPipeMode::PrintSimpleGraph || opts.mode == VSPipeMode::PrintFullGraph || filterTimeGraphFile)
        creationFlags |= ccfEnableGraphInspection;
    VSCore *core = vsapi->createCore(creationFlags);
    vsapi->addLogHandler(logMessageHandler, nullptr, &opts, core);
    vsapi->setCoreNodeTiming(core, opts.printFilterTime || filterTimeGraphFile);
    VSScript *se = vssapi->createScript(core);
    vssapi->evalSetWorkingDir(se, 1);
    if (!opts.scriptArgs.empty()) {
        VSMap *foldedArgs = vsapi->createMap();
        for (const auto &iter : opts.scriptArgs)
            vsapi->mapSetData(foldedArgs, iter.first.c_str(), iter.second.c_str(), static_cast<int>(iter.second.size()), dtUtf8, maAppend);
        vssapi->setVariables(se, foldedArgs);
        vsapi->freeMap(foldedArgs);
    }
    vssapi->evaluateFile(se, opts.scriptFilename.u8string().c_str());

    if (vssapi->getError(se)) {
        int code = vssapi->getExitCode(se);
        if (code == 0) code = 1;
        fprintf(stderr, "Script evaluation failed:\n%s\n", vssapi->getError(se));
        vssapi->freeScript(se);
        return code;
    }

    std::chrono::duration<double> scriptEvaluationTime = std::chrono::steady_clock::now() - scriptEvaluationStart;
    if (opts.printProgress)
        fprintf(stderr, "Script evaluation done in %.2f seconds\n", scriptEvaluationTime.count());

    if (opts.mode == VSPipeMode::PrintInfo) {
        int numOutputs = vssapi->getAvailableOutputNodes(se, 0, nullptr);
        std::vector<int> setIdx;
        setIdx.resize(numOutputs);
        vssapi->getAvailableOutputNodes(se, numOutputs, setIdx.data());
        bool first = true;
        for (const auto &iter : setIdx) {
            VSNode *mainNode = vssapi->getOutputNode(se, iter);
            assert(mainNode);

            if (!first && outFile)
                fprintf(outFile, "\n");
            first = false;

            if (vsapi->getNodeType(mainNode) == mtVideo) {
                VSNode *alphaNode = vssapi->getOutputAlphaNode(se, iter);
                const VSVideoInfo *vi = vsapi->getVideoInfo(mainNode);

                if (outFile) {
                    fprintf(outFile, "Output Index: %d\n", iter);
                    fprintf(outFile, "Type: Video\n");

                    if (vi->width && vi->height) {
                        fprintf(outFile, "Width: %d\n", vi->width);
                        fprintf(outFile, "Height: %d\n", vi->height);
                    } else {
                        fprintf(outFile, "Width: Variable\n");
                        fprintf(outFile, "Height: Variable\n");
                    }
                    fprintf(outFile, "Frames: %d\n", vi->numFrames);
                    if (vi->fpsNum && vi->fpsDen)
                        fprintf(outFile, "FPS: %" PRId64 "/%" PRId64 " (%.3f fps)\n", vi->fpsNum, vi->fpsDen, vi->fpsNum / static_cast<double>(vi->fpsDen));
                    else
                        fprintf(outFile, "FPS: Variable\n");

                    if (vi->format.colorFamily != cfUndefined) {
                        char nameBuffer[32];
                        vsapi->getVideoFormatName(&vi->format, nameBuffer);
                        fprintf(outFile, "Format Name: %s\n", nameBuffer);
                        fprintf(outFile, "Color Family: %s\n", colorFamilyToString(vi->format.colorFamily));
                        fprintf(outFile, "Alpha: %s\n", alphaNode ? "Yes" : "No");
                        fprintf(outFile, "Sample Type: %s\n", (vi->format.sampleType == stInteger) ? "Integer" : "Float");
                        fprintf(outFile, "Bits: %d\n", vi->format.bitsPerSample);
                        fprintf(outFile, "SubSampling W: %d\n", vi->format.subSamplingW);
                        fprintf(outFile, "SubSampling H: %d\n", vi->format.subSamplingH);
                    } else {
                        fprintf(outFile, "Format Name: Variable\n");
                    }
                }

                vsapi->freeNode(alphaNode);
            } else {
                if (outFile) {
                    fprintf(outFile, "Output Index: %d\n", iter);
                    fprintf(outFile, "Type: Audio\n");

                    const VSAudioInfo *ai = vsapi->getAudioInfo(mainNode);

                    char nameBuffer[32];
                    vsapi->getAudioFormatName(&ai->format, nameBuffer);
                    fprintf(outFile, "Samples: %" PRId64 "\n", ai->numSamples);
                    fprintf(outFile, "Sample Rate: %d\n", ai->sampleRate);
                    fprintf(outFile, "Format Name: %s\n", nameBuffer);
                    fprintf(outFile, "Sample Type: %s\n", (ai->format.sampleType == stInteger) ? "Integer" : "Float");
                    fprintf(outFile, "Bits: %d\n", ai->format.bitsPerSample);
                    fprintf(outFile, "Channels: %d\n", ai->format.numChannels);
                    fprintf(outFile, "Layout: %s\n", channelMaskToName(ai->format.channelLayout).c_str());
                }
            }

            vsapi->freeNode(mainNode);
        }
        vssapi->freeScript(se);
        return 0;
    }

    std::vector<int> selectedVideoIndices = opts.outputIndices;
    std::vector<int> selectedAudioIndices = opts.audioOutputIndices;
    bool nutOutputMode = opts.mode == VSPipeMode::Output && opts.outputHeaders == VSPipeHeaders::NUT;
    if (nutOutputMode && !opts.outputIndexExplicit && !selectedAudioIndices.empty())
        selectedVideoIndices.clear();

    int primaryOutputIndex = opts.outputIndex;
    if (nutOutputMode) {
        if (!selectedVideoIndices.empty())
            primaryOutputIndex = selectedVideoIndices.front();
        else if (!selectedAudioIndices.empty())
            primaryOutputIndex = selectedAudioIndices.front();
    }

    VSNode *node = vssapi->getOutputNode(se, primaryOutputIndex);
    if (!node) {
        fprintf(stderr, "Failed to retrieve output node. Invalid index specified?\n");
        vssapi->freeScript(se);
        return 1;
    }

    VSNode *alphaNode = vssapi->getOutputAlphaNode(se, primaryOutputIndex);
    VSNode *audioNode = nullptr;
    std::vector<VSNode *> nutExtraNodes;
    bool nutMultiStreamMode = nutOutputMode && (selectedVideoIndices.size() + selectedAudioIndices.size() > 1);

    if (nutMultiStreamMode && (opts.startPos != 0 || opts.endPos != -1)) {
        fprintf(stderr, "Error: --start/--end are not supported in NUT multi-stream mode, trim in script instead\n");
        vsapi->freeNode(node);
        vsapi->freeNode(alphaNode);
        vssapi->freeScript(se);
        return 1;
    }
    if (nutMultiStreamMode && (!opts.timecodesFilename.empty() || !opts.jsonFilename.empty())) {
        fprintf(stderr, "Error: --timecodes and --json are not supported in NUT multi-stream mode\n");
        vsapi->freeNode(node);
        vsapi->freeNode(alphaNode);
        vssapi->freeScript(se);
        return 1;
    }

    if (nutOutputMode && alphaNode) {
        vsapi->freeNode(alphaNode);
        alphaNode = nullptr;
    }

    // disable cache since no frame is ever requested twice
    vsapi->setCacheMode(node, cmForceDisable);
    if (alphaNode)
        vsapi->setCacheMode(alphaNode, cmForceDisable);

    bool success = true;

    if (opts.mode == VSPipeMode::PrintSimpleGraph) {
        std::string graph = printNodeGraph(NodePrintMode::Simple, node, 0, vsapi);
        if (outFile)
            fprintf(outFile, "%s\n", graph.c_str());
    } else if (opts.mode == VSPipeMode::PrintFullGraph) {
        std::string graph = printNodeGraph(NodePrintMode::Full, node, 0, vsapi);
        if (outFile)
            fprintf(outFile, "%s\n", graph.c_str());
    } else {
#ifdef VS_TARGET_OS_WINDOWS
        if (outFile == stdout) {
            if (_setmode(_fileno(stdout), _O_BINARY) == -1)
                fprintf(stderr, "Failed to set stdout to binary mode\n");
        }
#endif

        int nodeType = vsapi->getNodeType(node);

        if (!(nutOutputMode && nutMultiStreamMode) && (opts.startPos != 0 || opts.endPos != -1)) {
            VSMap *args = vsapi->createMap();
            vsapi->mapSetNode(args, "clip", node, maAppend);
            if (opts.startPos != 0)
                vsapi->mapSetInt(args, "first", opts.startPos, maAppend);
            if (opts.endPos > -1)
                vsapi->mapSetInt(args, "last", opts.endPos, maAppend);

            VSPlugin *stdPlugin = vsapi->getPluginByID(VSH_STD_PLUGIN_ID, vssapi->getCore(se));
            VSMap *result = vsapi->invoke(stdPlugin, (nodeType == mtVideo) ? "Trim" : "AudioTrim", args);

            VSMap *alphaResult = nullptr;
            if (alphaNode) {
                vsapi->mapSetNode(args, "clip", alphaNode, maReplace);
                alphaResult = vsapi->invoke(stdPlugin, "Trim", args);
            }

            vsapi->freeMap(args);

            VSMap *mapError = nullptr;
            if (vsapi->mapGetError(result)) {
                mapError = result;
            } else if (alphaResult && vsapi->mapGetError(alphaResult)) {
                mapError = alphaResult;
            }

            if (mapError) {
                fprintf(stderr, "%s\n", vsapi->mapGetError(mapError));
                vsapi->freeMap(mapError);
                vsapi->freeMap(result);
                vsapi->freeMap(alphaResult);
                vsapi->freeNode(node);
                vsapi->freeNode(alphaNode);
                vssapi->freeScript(se);
                return 1;
            } else {
                vsapi->freeNode(node);
                node = vsapi->mapGetNode(result, "clip", 0, nullptr);
                if (alphaResult)
                    alphaNode = vsapi->mapGetNode(alphaResult, "clip", 0, nullptr);
                vsapi->freeMap(result);
                vsapi->freeMap(alphaResult);
            }
        }

        std::unique_ptr<VSPipeOutputData> data(new VSPipeOutputData());

        data->vsapi = vsapi;
        data->outputHeaders = opts.outputHeaders;
        data->printProgress = opts.printProgress;
        data->node = node;
        data->alphaNode = alphaNode;
        data->audioNode = audioNode;
        data->outFile = outFile;
        data->timecodesFile = timecodesFile;
        data->jsonFile = jsonFile;

        if (nutOutputMode) {
            data->alphaNode = nullptr;
            data->audioNode = nullptr;

            std::set<int> usedIndices;
            bool hasDuplicate = false;
            for (const auto &idx : selectedVideoIndices) {
                if (!usedIndices.insert(idx).second) {
                    hasDuplicate = true;
                    break;
                }
            }
            for (const auto &idx : selectedAudioIndices) {
                if (!usedIndices.insert(idx).second) {
                    hasDuplicate = true;
                    break;
                }
            }
            if (hasDuplicate) {
                fprintf(stderr, "Error: duplicate output indices are not allowed in NUT stream selection\n");
                success = false;
            }

            std::map<int, VSNode *> selectedNodes;
            selectedNodes[primaryOutputIndex] = node;

            auto getSelectedNode = [&](int selectedIndex, VSNode *&selectedNode) -> bool {
                auto found = selectedNodes.find(selectedIndex);
                if (found != selectedNodes.end()) {
                    selectedNode = found->second;
                    return true;
                }

                selectedNode = vssapi->getOutputNode(se, selectedIndex);
                if (!selectedNode) {
                    data->errorMessage = "Error: failed to retrieve output node for selected index " + std::to_string(selectedIndex);
                    data->outputError = true;
                    return false;
                }

                vsapi->setCacheMode(selectedNode, cmForceDisable);
                selectedNodes[selectedIndex] = selectedNode;
                nutExtraNodes.push_back(selectedNode);
                return true;
            };

            std::vector<VSPipeNUTMuxStreamContext> streamContexts;
            std::vector<VSPipeNUTStreamInfo> streamInfos;
            streamContexts.reserve(selectedVideoIndices.size() + selectedAudioIndices.size());
            streamInfos.reserve(selectedVideoIndices.size() + selectedAudioIndices.size());

            int streamId = 0;
            auto appendStream = [&](int selectedIndex, bool fromVideoList) -> bool {
                VSNode *selectedNode = nullptr;
                if (!getSelectedNode(selectedIndex, selectedNode))
                    return false;

                int selectedNodeType = vsapi->getNodeType(selectedNode);
                bool allowLegacyAudioOnPrimaryList = !nutMultiStreamMode && selectedAudioIndices.empty() && selectedVideoIndices.size() == 1;

                if (fromVideoList) {
                    if (selectedNodeType != mtVideo && !(allowLegacyAudioOnPrimaryList && selectedNodeType == mtAudio)) {
                        data->errorMessage = "Error: --outputindex entry " + std::to_string(selectedIndex) + " is not a video output";
                        data->outputError = true;
                        return false;
                    }
                } else {
                    if (selectedNodeType != mtAudio) {
                        data->errorMessage = "Error: --audio-outputindex entry " + std::to_string(selectedIndex) + " is not an audio output";
                        data->outputError = true;
                        return false;
                    }
                }

                if (selectedNodeType == mtVideo) {
                    const VSVideoInfo *vi = vsapi->getVideoInfo(selectedNode);
                    std::array<uint8_t, 4> fourCC{};

                    if (!isConstantVideoFormat(vi)) {
                        data->errorMessage = "Error: output index " + std::to_string(selectedIndex) + " has varying dimensions";
                        data->outputError = true;
                        return false;
                    }
                    if (!VSPipeNUTWriter::getVideoFourCC(vi->format, fourCC)) {
                        data->errorMessage = "Error: no supported NUT fourcc exists for video output index " + std::to_string(selectedIndex);
                        data->outputError = true;
                        return false;
                    }

                    VSPipeNUTStreamInfo streamInfo;
                    streamInfo.type = VSPipeNUTStreamType::Video;
                    streamInfo.fourCC = fourCC;
                    streamInfo.width = vi->width;
                    streamInfo.height = vi->height;
                    streamInfos.push_back(streamInfo);

                    VSPipeNUTMuxStreamContext context;
                    context.node = selectedNode;
                    context.selectedIndex = selectedIndex;
                    context.streamId = streamId++;
                    context.isVideo = true;
                    context.videoInfo = vi;
                    context.videoFrameSize = getNutVideoFrameSize(vi);
                    context.videoCurrentPts = 0;
                    context.videoFallbackDurationTicks = 1;
                    if (vi->fpsNum > 0 && vi->fpsDen > 0)
                        context.videoFallbackDurationTicks = convertDurationToNutTicks(vi->fpsDen, vi->fpsNum, data->nutTimebaseNum, data->nutTimebaseDen);
                    streamContexts.push_back(context);
                } else {
                    const VSAudioInfo *ai = vsapi->getAudioInfo(selectedNode);
                    std::array<uint8_t, 4> fourCC{};

                    if (!VSPipeNUTWriter::getAudioFourCC(ai->format, fourCC)) {
                        data->errorMessage = "Error: no supported NUT fourcc exists for audio output index " + std::to_string(selectedIndex);
                        data->outputError = true;
                        return false;
                    }
                    if (ai->sampleRate <= 0) {
                        data->errorMessage = "Error: audio output index " + std::to_string(selectedIndex) + " has a non-positive sample rate";
                        data->outputError = true;
                        return false;
                    }
                    if (ai->format.numChannels <= 0) {
                        data->errorMessage = "Error: audio output index " + std::to_string(selectedIndex) + " has no channels";
                        data->outputError = true;
                        return false;
                    }

                    VSPipeNUTStreamInfo streamInfo;
                    streamInfo.type = VSPipeNUTStreamType::Audio;
                    streamInfo.fourCC = fourCC;
                    streamInfo.sampleRateNum = ai->sampleRate;
                    streamInfo.sampleRateDen = 1;
                    streamInfo.channelCount = ai->format.numChannels;
                    streamInfos.push_back(streamInfo);

                    VSPipeNUTMuxStreamContext context;
                    context.node = selectedNode;
                    context.selectedIndex = selectedIndex;
                    context.streamId = streamId++;
                    context.isVideo = false;
                    context.audioInfo = ai;
                    streamContexts.push_back(context);
                }

                return true;
            };

            if (success) {
                for (const auto &selectedIndex : selectedVideoIndices) {
                    if (!appendStream(selectedIndex, true)) {
                        success = false;
                        break;
                    }
                }
            }

            if (success) {
                for (const auto &selectedIndex : selectedAudioIndices) {
                    if (!appendStream(selectedIndex, false)) {
                        success = false;
                        break;
                    }
                }
            }

            if (success && streamContexts.empty()) {
                fprintf(stderr, "Error: no streams selected for NUT output\n");
                success = false;
            }

            if (!success && data->outputError && !data->errorMessage.empty())
                fprintf(stderr, "%s\n", data->errorMessage.c_str());

            if (success) {
                data->nutWriter.reset(new VSPipeNUTWriter());
                if (!data->nutWriter->initialize(data->outFile, streamInfos, data->errorMessage)) {
                    if (!data->errorMessage.empty())
                        fprintf(stderr, "%s\n", data->errorMessage.c_str());
                    else
                        fprintf(stderr, "Error: failed to initialize NUT output\n");
                    success = false;
                }
            }

            if (success) {
                size_t maxBufferSize = 0;
                int64_t totalVideoFramesExpected = 0;
                int64_t totalAudioSamplesExpected = 0;

                for (const auto &context : streamContexts) {
                    if (context.isVideo) {
                        size_t videoBufferSize = static_cast<size_t>(context.videoInfo->width) * static_cast<size_t>(context.videoInfo->height) * static_cast<size_t>(context.videoInfo->format.bytesPerSample);
                        maxBufferSize = std::max(maxBufferSize, videoBufferSize);
                        totalVideoFramesExpected += context.videoInfo->numFrames;
                    } else {
                        size_t audioBufferSize = static_cast<size_t>(context.audioInfo->format.numChannels) * static_cast<size_t>(VS_AUDIO_FRAME_SAMPLES) * static_cast<size_t>(context.audioInfo->format.bytesPerSample);
                        maxBufferSize = std::max(maxBufferSize, audioBufferSize);
                        totalAudioSamplesExpected += context.audioInfo->numSamples;
                    }
                }

                data->buffer.resize(maxBufferSize);
                success = !outputNutStreamsNode(opts, data.get(), streamContexts, totalVideoFramesExpected, totalAudioSamplesExpected);
            }
        } else if (nodeType == mtVideo) {

            const VSVideoInfo *vi = vsapi->getVideoInfo(node);

            if (!isConstantVideoFormat(vi)) {
                fprintf(stderr, "Cannot output clips with varying dimensions\n");
                vsapi->freeNode(node);
                vsapi->freeNode(alphaNode);
                vssapi->freeScript(se);
                return 1;
            }

            data->totalFrames = vi->numFrames;

            success = initializeVideoOutput(data.get());
            if (success) {
                data->lastFPSReportTime = std::chrono::steady_clock::now();
                success = !outputNode(opts, data.get(), vssapi->getCore(se));
            }
            if (success)
                success = finalizeVideoOutput(data.get());
            
        } else if (nodeType == mtAudio) {

            const VSAudioInfo *ai = vsapi->getAudioInfo(node);

            data->totalFrames = ai->numFrames;
            data->totalSamples = ai->numSamples;

            success = initializeAudioOutput(data.get());
            if (success)                
                success = !outputNode(opts, data.get(), vssapi->getCore(se));
        }

        if (outFile)
            fflush(outFile);
        if (timecodesFile)
            fflush(timecodesFile);
        if (jsonFile)
            fflush(jsonFile);

        std::chrono::duration<double> elapsedSeconds = std::chrono::steady_clock::now() - data->startTime;
        if (opts.mode == VSPipeMode::Output) {
            if (nutOutputMode)
                fprintf(stderr, "Output %d video frames and %" PRId64 " audio samples in %.2f seconds\n", data->totalFrames, data->totalSamples, elapsedSeconds.count());
            else if (vsapi->getNodeType(node) == mtVideo)
                fprintf(stderr, "Output %d frames in %.2f seconds (%.2f fps)\n", data->totalFrames, elapsedSeconds.count(), data->totalFrames / elapsedSeconds.count());
            else
                fprintf(stderr, "Output %" PRId64 " samples in %.2f seconds (%.2f sps)\n", data->totalSamples, elapsedSeconds.count(), (data->totalFrames / elapsedSeconds.count()) * VS_AUDIO_FRAME_SAMPLES);
        }

        if (opts.printFilterTime)
            fprintf(stderr, "%s", printNodeTimes(node, elapsedSeconds.count(), vsapi->getFreedNodeProcessingTime(core, 0), vsapi).c_str());
        if (filterTimeGraphFile) {
            std::string graph = printNodeGraph(NodePrintMode::FullWithTimes, node, elapsedSeconds.count(), vsapi);
            fprintf(filterTimeGraphFile, "%s", graph.c_str());
        }
    }

    if (outFile && closeOutFile)
        fclose(outFile);
    if (timecodesFile)
        fclose(timecodesFile);
    if (jsonFile)
        fclose(jsonFile);
    if (filterTimeGraphFile)
        fclose(filterTimeGraphFile);

    for (auto &extraNode : nutExtraNodes)
        vsapi->freeNode(extraNode);

    vsapi->freeNode(node);
    vsapi->freeNode(alphaNode);
    vsapi->freeNode(audioNode);
    vssapi->freeScript(se);

    return success ? 0 : 1;
}
