/*! \file guess_all_gui.cpp
 *  \brief Enter description here.
 *  \author Georgi Gerganov
 */

#include "constants.h"
#include "audio_logger.h"

#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"

#include <SDL.h>
#include <GL/gl3w.h>

#include <map>
#include <mutex>
#include <cmath>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <deque>
#include <fstream>

#define MY_DEBUG
#define OUTPUT_WAVEFORMS

// constants

constexpr float kTrainBufferSize_s = 0.075f;
constexpr float kPredictBufferSize_s = 0.200f;
constexpr uint64_t kSampleRate = 24000;

constexpr uint64_t kRingBufferSize = 128*1024;
constexpr int bkgrStep_samples = 1;
//constexpr int keyDuration_samples = 0.005f*kSampleRate;
constexpr int keyDuration_samples = 1;

constexpr uint64_t kTrainBufferSize_frames = 2*AudioLogger::getBufferSize_frames(kSampleRate, kTrainBufferSize_s) - 1;
constexpr uint64_t kPredictBufferSize_frames = 2*AudioLogger::getBufferSize_frames(kSampleRate, kPredictBufferSize_s) - 1;

constexpr auto kSamplesPerFrame = AudioLogger::kSamplesPerFrame;
constexpr auto kSamplesPerWaveform = kSamplesPerFrame*kTrainBufferSize_frames;

static const std::vector<float> kRowOffset = { 0.0f, 1.5f, 1.8f, 2.1f, 5.5f };
static const std::vector<std::vector<int>> kKeyboard = {
    { '`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 127},
    { 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\', },
    { 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '\n' },
    { 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', },
    { ' ', },
};

// types

using TSum = double;
using TSum2 = double;
using TConfidence = float;
using TValueCC = double;
using TOffset = int;

using TKey = int;
using TKeyWaveform = std::vector<AudioLogger::Sample>;
using TKeyHistory = std::vector<TKeyWaveform>;
using TKeyConfidenceMap = std::map<TKey, TConfidence>;

struct TrainStats {
    int nWaveformsUsed = 0;
    int nWaveformsTotal = 0;
    TValueCC averageCC = 0.0f;
};

// helpers

std::tuple<TSum, TSum2> calcSum(const TKeyWaveform & waveform, int is0, int is1) {
    TSum sum = 0.0f;
    TSum2 sum2 = 0.0f;
    for (int is = is0; is < is1; ++is) {
        auto a0 = waveform[is];
        sum += a0;
        sum2 += a0*a0;
    }

    return { sum, sum2 };
}

TValueCC calcCC(
    const TKeyWaveform & waveform0,
    const TKeyWaveform & waveform1,
    TSum sum0, TSum2 sum02,
    int is00, int is0, int is1) {
    TValueCC cc = -1.0f;

    TSum sum1 = 0.0f;
    TSum2 sum12 = 0.0f;
    TSum2 sum01 = 0.0f;
    for (int is = 0; is < is1 - is0; ++is) {
        auto a0 = waveform0[is00 + is];
        auto a1 = waveform1[is0 + is];

#ifdef MY_DEBUG
        if (is00 + is < 0 || is00 + is >= waveform0.size()) printf("BUG 0\n");
        if (is0 + is < 0 || is0 + is >= waveform1.size()) {
            printf("BUG 1\n");
            printf("%d %d %d\n", is0, is, (int) waveform1.size());
        }
#endif

        sum1 += a1;
        sum12 += a1*a1;
        sum01 += a0*a1;
    }

    int ncc = (is1 - is0);
    {
        double nom = sum01*ncc - sum0*sum1;
        double den2a = sum02*ncc - sum0*sum0;
        double den2b = sum12*ncc - sum1*sum1;
        cc = (nom)/(sqrt(den2a*den2b));
    }

    return cc;
}

std::tuple<TValueCC, TOffset> findBestCC(
    const TKeyWaveform & waveform0,
    const TKeyWaveform & waveform1,
    int is0, int is1,
    int alignWindow) {
    TOffset besto = -1;
    TValueCC bestcc = -1.0f;

    int is00 = waveform0.size()/2 - (is1 - is0)/2;
    auto [sum0, sum02] = calcSum(waveform0, is00, is00 + is1 - is0);

    int nWorkers = std::thread::hardware_concurrency();
    std::mutex mutex;
    std::vector<std::thread> workers(nWorkers);
    for (int i = 0; i < workers.size(); ++i) {
        auto & worker = workers[i];
        worker = std::thread([&, sum0 = sum0, sum02 = sum02, i]() {
            TOffset cbesto = -1;
            TValueCC cbestcc = -1.0f;

            for (int o = -alignWindow + i; o < alignWindow; o += nWorkers) {
                auto cc = calcCC(waveform0, waveform1, sum0, sum02, is00, is0 + o, is1 + o);
                if (cc > cbestcc) {
                    cbesto = o;
                    cbestcc = cc;
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                if (cbestcc > bestcc) {
                    bestcc = cbestcc;
                    besto = cbesto;
                }
            }
        });
    }
    for (auto & worker : workers) worker.join();

    return { bestcc, besto };
}

// globals

static bool g_isInitialized = false;

static std::function<int()> g_init;
static std::function<void()> g_update;
static std::function<void(int)> g_handleKey;

int init() {
    if (g_isInitialized) return 1;

    return g_init();
}

void update() {
    if (g_isInitialized == false) return;

    g_update();
}

// JS interface
extern "C" {
    int doInit() {
        return init();
    }

    void keyPressedCallback(int key) {
        g_handleKey(key);
    }
}

int main(int argc, char ** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s input.kbd [input2.kbd ...]\n", argv[0]);
        return -127;
    }

    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER) != 0) {
        printf("Error: %s\n", SDL_GetError());
        return -1;
    }

    int windowSizeX = 600;
    int windowSizeY = 600;

#if __APPLE__
    // GL 3.2 Core + GLSL 150
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_DisplayMode current;
    SDL_GetCurrentDisplayMode(0, &current);
    SDL_Window* window = SDL_CreateWindow("Keytap", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, windowSizeX, windowSizeY, SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    // Initialize OpenGL loader
    bool err = gl3wInit() != 0;
    if (err) {
        fprintf(stderr, "Failed to initialize OpenGL loader!\n");
        return 1;
    }

    // Setup Dear ImGui binding
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Setup style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsClassic();

    ImFontConfig fontConfig;
    //fontConfig.SizePixels = 14.0f;
    ImGui::GetIO().Fonts->AddFontDefault(&fontConfig);

    std::ifstream frecord;
    std::map<int, std::ifstream> fins;
    for (int i = 0; i < argc - 1; ++i) {
        printf("Opening file '%s'\n", argv[i + 1]);
        fins[i] = std::ifstream(argv[i + 1], std::ios::binary);
        if (fins[i].good() == false) {
            printf("Failed to open input file: '%s'\n", argv[i + 1]);
            return -2;
        }

        {
            int bufferSize_frames = 1;
            fins[i].read((char *)(&bufferSize_frames), sizeof(bufferSize_frames));
            if (bufferSize_frames != kTrainBufferSize_frames) {
                printf("Buffer size in file (%d) does not match the expected one (%d)\n", bufferSize_frames, (int) kTrainBufferSize_frames);
                return -1;
            }
        }
    }

    TKey keyPressed = -1;
    TKeyConfidenceMap keyConfidence;
    TKeyConfidenceMap keyConfidenceDisplay;
    std::map<TKey, TKeyHistory> keySoundHistoryAmpl;
    std::map<TKey, TKeyWaveform> keySoundAverageAmpl;

    bool doRecord = false;
    bool printStatus = true;
    bool isReadyToPredict = false;
    bool processingInput = true;
    bool processingRecord = false;
    bool finishApp = false;
    bool waitForQueueDuringPlayback = true;

    int curFile = 0;

    int predictedKey = -1;
    TKeyWaveform predictedAmpl(kSamplesPerWaveform, 0);
    int predictedHistoryBegin = 0;
    std::array<std::vector<int>, 24> predictedHistory;
    predictedHistory.fill({});
    std::map<TKey, TrainStats> trainStats;

    float amplMin = 0.0f;
    float amplMax = 0.0f;
    float thresholdCC = 0.5f;
    float thresholdBackground = 10.0f;
    TValueCC predictedCC = -1.0f;
    auto tLastDetectedKeyStroke = std::chrono::high_resolution_clock::now();

    // ring buffer
    int rbBegin = 0;
    double rbAverage = 0.0f;
    std::array<double, kRingBufferSize> rbSamples;
    rbSamples.fill(0.0f);

    AudioLogger audioLogger;

    struct WorkData {
        TKeyWaveform ampl;
        std::vector<int> positionsToPredict;
    };

    std::mutex mutex;
    std::deque<WorkData> workQueue;
    std::thread worker([&]() {
        int lastkey = -1;
        double lastcc = -1.0f;

        while (finishApp == false) {
            bool process = false;
            WorkData workData;
            {
                std::lock_guard<std::mutex> lock(mutex);
                while (workQueue.size() > 30 && processingRecord == false) {
                    workQueue.pop_front();
                    printf("pop\n");
                }

                if (workQueue.size() > 0) {
                    workData = std::move(workQueue.front());
                    workQueue.pop_front();
                    process = true;
                }
            }
            if (process) {
                const auto & ampl = workData.ampl;
                const auto & positionsToPredict = workData.positionsToPredict;

                int alignWindow = kSamplesPerFrame/2;

                for (int ipos = 0; ipos < positionsToPredict.size() ; ++ipos) {
                    auto curPos = positionsToPredict[ipos];
                    int scmp0 = curPos - kSamplesPerFrame;
                    int scmp1 = curPos + kSamplesPerFrame;

                    char res = -1;
                    TValueCC maxcc = -1.0f;
                    TOffset offs = 0;
                    TKeyConfidenceMap keyConfidenceTmp;
                    for (const auto & ka : keySoundAverageAmpl) {
                        auto [bestcc, bestoffset] = findBestCC(keySoundAverageAmpl[ka.first], ampl, scmp0, scmp1, alignWindow);
                        if (bestcc > maxcc) {
                            res = ka.first;
                            maxcc = bestcc;
                            offs = bestoffset;
                        }
                        keyConfidenceTmp[ka.first] = bestcc;
                    }

                    if (maxcc > thresholdCC) {
                        if (lastkey != res || lastcc != maxcc) {
                            printf("    Prediction: '%c'        (%8.5g)\n", res, maxcc);
                            predictedKey = res;
                            predictedCC = maxcc;
                            predictedHistory[predictedHistoryBegin].clear();
                            predictedHistory[predictedHistoryBegin].push_back(predictedKey);
                            for (auto & c : keyConfidenceTmp) {
                                keyConfidence[c.first] = c.second/maxcc;
                                keyConfidenceDisplay[c.first] = std::pow(c.second/maxcc, 4.0f);
                                if (c.first != predictedKey && c.second/maxcc > 0.9f) {
                                    predictedHistory[predictedHistoryBegin].push_back(c.first);
                                }
                            }
                            if (++predictedHistoryBegin >= predictedHistory.size()) predictedHistoryBegin = 0;
                            for (int i = 0; i < kSamplesPerWaveform; ++i) {
                                int idx = curPos + offs - kSamplesPerWaveform/2 + i;
                                if (idx < 0 || idx >= (int) ampl.size()) continue;
                                predictedAmpl[i] = ampl[idx];
                            }
                        }
                        lastkey = res;
                        lastcc = maxcc;
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });

    AudioLogger::Callback cbAudio = [&](const AudioLogger::Record & frames) {
        if (frames.size() != kTrainBufferSize_frames && isReadyToPredict == false) {
            printf("Unexpected number of frames - %d, expected - %d. Should never happen\n",
                   (int) frames.size(), (int) kTrainBufferSize_frames);
            return;
        }

        const int nFrames = frames.size();

        if (isReadyToPredict) {

            std::vector<int> positionsToPredict;

            {
                float amax = 0.0f;
                for (int f = 0; f < frames.size(); ++f) {
                    for (int s = 0; s < frames[f].size(); s += bkgrStep_samples) {
                        rbAverage *= rbSamples.size();
                        rbAverage -= rbSamples[rbBegin];
                        auto acur = std::abs(frames[f][s]);
                        rbSamples[rbBegin] = acur;
                        if (acur > amax) amax = acur;
                        rbAverage += acur;
                        rbAverage /= rbSamples.size();
                        if (++rbBegin >= rbSamples.size()) rbBegin = 0;
                    }
                }

                int nFrames = frames.size();
                int nFrames2 = std::max(1, nFrames/2);

                auto _acc = [](const AudioLogger::Record & r, int id) { return std::abs(r[id/kSamplesPerFrame][id%kSamplesPerFrame]); };

                int k = kSamplesPerFrame;
                std::deque<int> que(k);
                for (int i = 0; i < nFrames*kSamplesPerFrame; ++i) {
                    if (i < k) {
                        while((!que.empty()) && _acc(frames, i) >= _acc(frames, que.back())) {
                            que.pop_back();
                        }
                        que.push_back(i);
                    } else {
                        while((!que.empty()) && que.front() <= i - k) {
                            que.pop_front();
                        }

                        while((!que.empty()) && _acc(frames, i) >= _acc(frames, que.back())) {
                            que.pop_back();
                        }

                        que.push_back(i);

                        int itest = i - k/2;
                        if (itest >= 2*kSamplesPerFrame && itest < (nFrames - 2)*kSamplesPerFrame && que.front() == itest) {
                            auto acur = _acc(frames, itest);
                            if (acur > thresholdBackground*rbAverage){
                                positionsToPredict.push_back(itest);
                                tLastDetectedKeyStroke = std::chrono::high_resolution_clock::now();
                            }
                        }
                    }
                }
            }

            if (positionsToPredict.size() > 0) {
                WorkData workData;
                auto & ampl = workData.ampl;
                ampl.resize(nFrames*kSamplesPerFrame);
                for (int k = 0; k < nFrames; ++k) {
                    std::copy(frames[k].begin(), frames[k].end(), ampl.begin() + k*kSamplesPerFrame);
                }
                workData.positionsToPredict = positionsToPredict;

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    workQueue.push_back(std::move(workData));
                }
            }

            doRecord = true;
        } else {
            auto & history = keySoundHistoryAmpl[keyPressed];
            history.push_back(TKeyWaveform());
            auto & ampl = history.back();
            ampl.resize(nFrames*kSamplesPerFrame);
            for (int k = 0; k < nFrames; ++k) {
                std::copy(frames[k].begin(), frames[k].end(), ampl.begin() + k*kSamplesPerFrame);
            }
        }

        keyPressed = -1;
    };

    g_init = [&]() {
        if (audioLogger.install(kSampleRate, cbAudio) == false) {
            fprintf(stderr, "Failed to install audio logger\n");
            return -1;
        }

        audioLogger.pause();

        printf("[+] Collecting training data\n");
        g_isInitialized = true;
        return 0;
    };

    g_handleKey = [&](int key) {
        if (keyPressed == -1 && isReadyToPredict == false) {
            predictedKey = -1;
            keyPressed = key;
            audioLogger.record(kTrainBufferSize_s);
        }
    };

    g_update = [&]() {
        if (processingInput) {
            if (keyPressed == -1) {
                AudioLogger::Frame frame;
                AudioLogger::Record record;
                fins[curFile].read((char *)(&keyPressed), sizeof(keyPressed));
                if (fins[curFile].eof()) {
                    ++curFile;
                    if (curFile >= fins.size()) {
                        processingInput = false;
                    }
                } else {
                    printf("%c", keyPressed);
                    fflush(stdout);
                    for (int i = 0; i < kTrainBufferSize_frames; ++i) {
                        fins[curFile].read((char *)(frame.data()), sizeof(AudioLogger::Sample)*frame.size());
                        record.push_back(frame);
                    }
                    cbAudio(record);
                }
            }
            return;
        }

        if (processingRecord) {
            if (frecord.eof()) {
                if (workQueue.size() == 0) {
                    printf("[+] Done. Continuing capturing microphone audio \n");
                    processingRecord = false;
                    audioLogger.resume();
                }

                return;
            }
            if (keyPressed == -1 && ((waitForQueueDuringPlayback == false) || (waitForQueueDuringPlayback && workQueue.size() < 3))) {
                AudioLogger::Frame frame;
                static AudioLogger::Record record;
                keyPressed = 32;
                int nRead = kPredictBufferSize_frames;
                if (record.size() > 5) {
                    record.erase(record.begin(), record.end() - 5);
                    nRead -= 5;
                }
                for (int i = 0; i < nRead; ++i) {
                    frecord.read((char *)(frame.data()), sizeof(AudioLogger::Sample)*frame.size());
                    if (frecord.eof()) {
                        printf("[+] Waiting for work queue to get processed. Remaining jobs = %d \n", (int) workQueue.size());
                        record.clear();
                        break;
                    } else {
                        record.push_back(frame);
                    }
                }
                if (record.size() == kPredictBufferSize_frames) {
                    cbAudio(record);
                } else {
                    printf("    Skipping partial buffer of size %d frames\n", (int) record.size());
                }
            }
            return;
        }

        if (isReadyToPredict == false) {
            printf("[+] Training\n");

            std::vector<TKey> failedToTrain;

            auto trainKey = [&](TKey key) {
                auto & history = keySoundHistoryAmpl[key];

                int nWaveforms = history.size();
                int nFramesPerWaveform = kTrainBufferSize_frames;

                printf("    - Training key '%c'\n", key);
                printf("    - History size = %d key waveforms\n", nWaveforms);
                printf("    - Frames per key waveform   = %d\n", nFramesPerWaveform);
                printf("    - Total frames available    = %d\n", nWaveforms*nFramesPerWaveform);
                printf("    - Samples per frame         = %d\n", (int) kSamplesPerFrame);
                printf("    - Total samples available   = %d\n", (int) (nWaveforms*nFramesPerWaveform*kSamplesPerFrame));

                printf("    - Estimating waveform peaks ...\n");
                std::vector<int> peakSum;
                std::vector<int> peakMax;

                peakSum.clear();
                peakMax.clear();
                for (int iwaveform = 0; iwaveform < nWaveforms; ++iwaveform) {
                    int isum = -1;
                    double asum = 0.0f;
                    double aisum = 0.0f;

                    int imax = -1;
                    double amax = 0.0f;

                    const auto & waveform = history[iwaveform];

                    for (int icur = 0; icur < kSamplesPerWaveform; ++icur) {
                        double acur = std::abs(waveform[icur]);
                        double acur2 = acur*acur;

                        asum += acur2;
                        aisum += acur2*icur;

                        if (acur > amax) {
                            amax = acur;
                            imax = icur;
                        }
                    }

                    isum = aisum/asum;

                    peakSum.push_back(isum);
                    peakMax.push_back(imax);
                    //printf("        Estimated peak: %d (method - sum), %d (method - max)\n", isum, imax);
                }

                auto calcStdev = [](const std::vector<int> & data) {
                    double sum = 0.0f;
                    double sum2 = 0.0f;
                    for (const auto & p : data) {
                        sum += p;
                        sum2 += p*p;
                    }
                    sum /= data.size();
                    sum2 /= data.size();
                    return sqrt(sum2 - sum*sum);
                };

                double stdevSum = calcStdev(peakSum);
                double stdevMax = calcStdev(peakMax);

                printf("    - Stdev of estimated peaks: %g (sum) vs %g (max)\n", stdevSum, stdevMax);

                const auto & peakUsed = peakMax;
                printf("    - Using 'max' estimation\n");

                int centerSample = kSamplesPerWaveform/2;

                printf("    - Centering waveforms at sample %d\n", centerSample);
                for (int iwaveform = 0; iwaveform < nWaveforms; ++iwaveform) {
                    int offset = peakUsed[iwaveform] - centerSample;
                    //printf("        Offset for waveform %-4d = %-4d\n", iwaveform, offset);

                    auto newWaveform = TKeyWaveform();
                    newWaveform.resize(kSamplesPerWaveform);
                    auto & waveform = history[iwaveform];
                    for (int icur = 0; icur < kSamplesPerWaveform; ++icur) {
                        int iorg = icur + offset;

                        if (iorg >= 0 && iorg < kSamplesPerWaveform) {
                            newWaveform[icur] = waveform[iorg];
                        } else {
                            newWaveform[icur] = 0.0f;
                        }
                    }

                    waveform = std::move(newWaveform);
                }

                int alignWindow = 64;
                printf("    - Calculating CC pairs\n");
                printf("      Align window = %d\n", alignWindow);

                int bestw = -1;
                int ntrain = 0;
                double bestccsum = -1.0f;
                double bestosum = 1e10;
                std::map<int, std::map<int, std::tuple<TValueCC, TOffset>>> ccs;

                for (int alignToWaveform = 0; alignToWaveform < nWaveforms; ++alignToWaveform) {
                    ccs[alignToWaveform][alignToWaveform] = { 1.0f, 0 };

                    int is0 = centerSample - kSamplesPerFrame;
                    int is1 = centerSample + kSamplesPerFrame;

                    const auto & waveform0 = history[alignToWaveform];

                    for (int iwaveform = alignToWaveform + 1; iwaveform < nWaveforms; ++iwaveform) {
                        const auto & waveform1 = history[iwaveform];
                        auto [bestcc, bestoffset] = findBestCC(waveform0, waveform1, is0, is1, alignWindow);

                        ccs[iwaveform][alignToWaveform] = { bestcc, bestoffset };
                        ccs[alignToWaveform][iwaveform] = { bestcc, -bestoffset };
                    }

                    int curntrain = 0;
                    double curccsum = 0.0;
                    double curosum = 0.0;
                    for (int iwaveform = 0; iwaveform < nWaveforms; ++iwaveform) {
                        auto [cc, offset] = ccs[iwaveform][alignToWaveform];
                        if (std::abs(offset) > 50) continue;
                        ++curntrain;
                        curccsum += cc*cc;
                        curosum += offset*offset;
                    }

                    if (curccsum > bestccsum) {
                    //if (curosum < bestosum) {
                        ntrain = curntrain;
                        bestw = alignToWaveform;
                        bestccsum = curccsum;
                        bestosum = curosum;
                    }
                }
                bestccsum = sqrt(bestccsum/ntrain);

                trainStats[key].nWaveformsUsed = ntrain;
                trainStats[key].nWaveformsTotal = nWaveforms;
                trainStats[key].averageCC = bestccsum;

                printf("    - Aligning all waveforms to waveform %d, (cost = %g)\n", bestw, bestccsum);
#ifdef OUTPUT_WAVEFORMS
                std::ofstream fout(std::string("waveform_one_") + std::to_string(key) + ".plot");
                for (auto & v : history[bestw]) fout << v << std::endl;
                fout << std::endl;
#endif
                for (int iwaveform = 0; iwaveform < nWaveforms; ++iwaveform) {
                    if (iwaveform == bestw) continue;

                    auto & waveform1 = history[iwaveform];
                    auto [cc, offset] = ccs[iwaveform][bestw];

                    auto newWaveform = TKeyWaveform();
                    newWaveform.resize(kSamplesPerWaveform);
                    for (int icur = 0; icur < kSamplesPerWaveform; ++icur) {
                        int iorg = icur + offset;

                        if (iorg >= 0 && iorg < kSamplesPerWaveform) {
                            newWaveform[icur] = waveform1[iorg];
                        } else {
                            newWaveform[icur] = 0.0f;
                        }
                    }

                    waveform1 = std::move(newWaveform);
#ifdef OUTPUT_WAVEFORMS
                    for (auto & v : waveform1) fout << v << std::endl;
                    fout << std::endl;
#endif
                }

                printf("    - Calculating average waveform\n");
                double ccsum = 0.0f;
                double norm = 0.0f;
                auto & avgWaveform = keySoundAverageAmpl[key];
                avgWaveform.resize(kSamplesPerWaveform);
                std::fill(avgWaveform.begin(), avgWaveform.end(), 0.0f);
                for (int iwaveform = 0; iwaveform < nWaveforms; ++iwaveform) {
                    auto [cc, offset] = ccs[iwaveform][bestw];
                    //if (std::abs(offset) > 5) continue;
                    printf("        Adding waveform %d - cc = %g, offset = %d\n", iwaveform, cc, offset);
                    ccsum += cc*cc;
                    norm += cc*cc;
                    auto & waveform = history[iwaveform];
                    for (int is = 0; is < kSamplesPerWaveform; ++is) {
                        avgWaveform[is] += cc*cc*waveform[is];
                    }
                }

                norm = 1.0f/(norm);
                for (int is = 0; is < kSamplesPerWaveform; ++is) {
                    avgWaveform[is] *= norm;
                    if (avgWaveform[is] > amplMax) amplMax = avgWaveform[is];
                    if (avgWaveform[is] < amplMin) amplMin = avgWaveform[is];
                }

#ifdef OUTPUT_WAVEFORMS
                {
                    std::ofstream fout(std::string("waveform_avg_") + std::to_string(key) + ".plot");
                    for (auto & v : avgWaveform) fout << v << std::endl;
                }
#endif

                if (ccsum*norm < 0.50f || (1.0f/norm < nWaveforms/3)) {
                    failedToTrain.push_back(key);
                }

                printf("\n");
            };

            for (const auto & kh : keySoundHistoryAmpl) {
                if (kh.second.size() > 2) {
                    trainKey(kh.first);
                } else {
                    failedToTrain.push_back(kh.first);
                }
            }
            printf("Failed to train the following keys: ");
            for (auto & k : failedToTrain) printf("'%c' ", k);
            printf("\n");
            isReadyToPredict = true;
            doRecord = true;

            amplMax = std::max(amplMax, -amplMin);
            amplMin = -std::max(amplMax, -amplMin);

            for (auto & kh : keySoundAverageAmpl) {
                float curAmplMax = 0.0f;
                for (const auto & v : kh.second) if (std::abs(v) > curAmplMax) curAmplMax = std::abs(v);
                for (auto & v : kh.second) v = (v/curAmplMax)*amplMax;
            }

            audioLogger.resume();

            printf("[+] Ready to predict. Keep pressing keys and the program will guess which key was pressed\n");
            printf("    based on the captured audio from the microphone.\n");
            printf("[+] Predicting\n");
        }

        if (doRecord) {
            doRecord = false;
            audioLogger.recordSym(kPredictBufferSize_s);
        }
    };

    init();
    while (finishApp == false) {
        update();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            switch (event.type) {
                case SDL_QUIT:
                    finishApp = true;
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) finishApp = true;
                    break;
            };
        }

        SDL_GetWindowSize(window, &windowSizeX, &windowSizeY);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame(window);
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(windowSizeX, windowSizeY));
        ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        if (isReadyToPredict == false) {
            ImGui::Text("Training ... Please wait");
        } else {
            {
                static char inp[128] = { "record.kbd" };
                ImGui::InputText("Audio file", inp, 128);
                ImGui::SameLine();
                if (ImGui::Button("Load")) {
                    printf("[+] Replaying audio from file '%s' ...\n", inp);
                    frecord = std::ifstream(inp, std::ios::binary);
                    if (frecord.good()) {
                        audioLogger.pause();
                        processingRecord = true;
                    }
                }
                ImGui::SameLine();
                ImGui::Checkbox("##waitForQueue", &waitForQueueDuringPlayback);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("If selected - wait for old playback data to get processed before pushing new data");
                    ImGui::EndTooltip();
                }
            }
            ImGui::Text("Last predicted key:       %s (%8.6f)\n", kKeyText.at(predictedKey), predictedCC);
            ImGui::SliderFloat("Threshold CC", &thresholdCC, 0.1f, 1.0f);
            auto tNow = std::chrono::high_resolution_clock::now();
            ImGui::Text("Last detected key stroke: %5.3f seconds ago\n",
                        (float)(std::chrono::duration_cast<std::chrono::milliseconds>(tNow - tLastDetectedKeyStroke).count()/1000.0f));
            ImGui::Text("Average background level: %16.13f\n", rbAverage);
            ImGui::SliderFloat("Threshold background", &thresholdBackground, 0.1f, 300.0f);
            ImGui::Text("Tasks in queue: %d\n", (int) workQueue.size());
            ImGui::Text("\n");

            static bool displayConfidence = false;
            ImGui::Checkbox("Display confidence", &displayConfidence);

            auto drawList = ImGui::GetWindowDrawList();

            auto p0 = ImGui::GetCursorScreenPos();
            auto p1 = p0;
            float ox = 0.0f;
            float oy = p0.y;
            float bx = 32.0f;
            float by = 32.0f;

            for (int rid = 0; rid < kKeyboard.size(); ++rid) {
                const auto & row = kKeyboard[rid];
                ox = p0.x + kRowOffset[rid]*bx;
                for (const auto & button : row) {
                    int key = button;
                    auto & confDisplay = keyConfidenceDisplay[key];
                    p1.x = ox;
                    p1.y = oy;
                    auto p2 = p1;
                    auto tw = ImGui::CalcTextSize(kKeyText.at(key));
                    p2.x += bx + tw.x;
                    p2.y += by;
                    ox += bx + tw.x;
                    drawList->AddRectFilled(p1, p2, ImGui::ColorConvertFloat4ToU32({ 0.2f, 1.0f, 0.2f, confDisplay }), 0.0f, 0);
                    drawList->AddRect(p1, p2, ImGui::ColorConvertFloat4ToU32({ 1.0f, 1.0f, 1.0f, 1.0f }), 0.0f, 0, 1.0f);
                    if (displayConfidence) {
                        ImGui::SetCursorScreenPos({0.5f*(p1.x + p2.x - tw.x), 0.5f*(p1.y + p2.y - tw.y) - 0.5f*tw.y});
                    } else {
                        ImGui::SetCursorScreenPos({0.5f*(p1.x + p2.x - tw.x), 0.5f*(p1.y + p2.y - tw.y)});
                    }
                    if (key == predictedKey) {
                        ImGui::TextColored({1.0f, 0.0f, 0.0f, 1.0f}, "%s", kKeyText.at(key));
                    } else {
                        if (keySoundAverageAmpl.find(key) == keySoundAverageAmpl.end()) {
                            ImGui::TextDisabled("%s", kKeyText.at(key));
                        } else {
                            ImGui::Text("%s", kKeyText.at(key));
                        }
                    }
                    if (displayConfidence) {
                        ImGui::SetWindowFontScale(0.90f);
                        static auto tcw = ImGui::CalcTextSize("0.123");
                        ImGui::SetCursorScreenPos({0.5f*(p1.x + p2.x - tcw.x), 0.5f*(p1.y + p2.y - tw.y) + 0.5f*tw.y});
                        ImGui::Text("%4.3f", keyConfidence[key]);
                        ImGui::SetWindowFontScale(1.0f);
                    }
                    confDisplay *= 0.99f;
                }
                oy += by;
            }

            ImGui::SetCursorScreenPos({p0.x, oy + ImGui::GetTextLineHeightWithSpacing()});

            ImGui::TextDisabled("Last %d predicted keys:", (int) predictedHistory.size());
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                predictedHistory.fill({});
            }
            int nBestPredictions = 3;
            for (int ip = 0; ip < nBestPredictions; ++ip) {
                for (int i = 0; i < (int) predictedHistory.size(); ++i) {
                    int idx = (predictedHistoryBegin + i)%predictedHistory.size();
                    int maxLen = 1;
                    for (auto l : predictedHistory[idx]) if (strlen(kKeyText.at(l)) > maxLen) maxLen = strlen(kKeyText.at(l));
                    static std::map<int, const char *> kws = {
                        {0,  ""},
                        {1,  " "},
                        {2,  "  "},
                        {3,  "   "},
                        {4,  "    "},
                        {5,  "     "},
                        {6,  "      "},
                        {7,  "       "},
                        {8,  "        "},
                        {9,  "         "},
                        {10, "          "},
                    };
                    if (predictedHistory[idx].size() > ip) {
                        auto t = kKeyText.at(predictedHistory[idx][ip]);
                        ImGui::Text("%s%s", t, kws[maxLen - strlen(t)]);
                        ImGui::SameLine();
                    } else {
                        ImGui::Text("%s", kws.at(maxLen)); ImGui::SameLine();
                    }
                }
                ImGui::Text("\n");
            }
            ImGui::Text("\n\n");

            if (ImGui::CollapsingHeader("Last prediction", 0)) {
                if (predictedKey != -1) {
                    auto savePos = ImGui::GetCursorScreenPos();
                    {
                        const auto & ampl = keySoundAverageAmpl[predictedKey];
                        ImGui::PlotLines((std::string("##") + kKeyText.at(predictedKey)).c_str(), ampl.data(), ampl.size(), 0, kKeyText.at(predictedKey), amplMin, amplMax, { ImGui::GetContentRegionAvailWidth(), 400.0f });
                    }
                    ImGui::SetCursorScreenPos(savePos);
                    {
                        ImGui::PushStyleColor(ImGuiCol_PlotLines, {1.0f, 0.0f, 0.0f, 0.5f});
                        ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.0f, 0.0f, 0.0f, 0.0f});
                        ImGui::PlotLines((std::string("##") + kKeyText.at(predictedKey)).c_str(), predictedAmpl.data(), predictedAmpl.size(), 0, kKeyText.at(predictedKey), amplMin, amplMax, { ImGui::GetContentRegionAvailWidth(), 400.0f });
                        ImGui::PopStyleColor(2);
                    }
                }
            }

            if (ImGui::CollapsingHeader("Average key sound", 0)) {
                for (const auto & key : keySoundAverageAmpl) {
                    ImGui::PlotLines((std::string("##") + kKeyText.at(key.first)).c_str(), key.second.data(), key.second.size(), 0, kKeyText.at(key.first), amplMin, amplMax, { ImGui::GetContentRegionAvailWidth(), 100.0f });
                }
            }

            if (ImGui::CollapsingHeader("Training statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (const auto & key : trainStats) {
                    if (key.second.nWaveformsUsed < 0.75*key.second.nWaveformsTotal) {
                        ImGui::TextColored({1.0f, 1.0f, 0.0f, (float) key.second.averageCC},
                                           "Key: %8s   Average CC: %8.6f   Waveforms: %3d / %3d",
                                           kKeyText.at(key.first), key.second.averageCC,
                                           key.second.nWaveformsUsed, key.second.nWaveformsTotal);
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::Text("Predictions for this key might not be very accurate. Provide more training data");
                            ImGui::EndTooltip();
                        }
                    } else {
                        ImGui::TextColored({1.0f, 1.0f, 1.0f, (float) key.second.averageCC},
                                           "Key: %8s   Average CC: %8.6f   Waveforms: %3d / %3d",
                                           kKeyText.at(key.first), key.second.averageCC,
                                           key.second.nWaveformsUsed, key.second.nWaveformsTotal);
                    }
                }
            }
        }

        ImGui::End();

        ImGui::Render();
        SDL_GL_MakeCurrent(window, gl_context);
        glViewport(0, 0, (int) io.DisplaySize.x, (int) io.DisplaySize.y);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    worker.join();

    printf("[+] Terminated");

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}