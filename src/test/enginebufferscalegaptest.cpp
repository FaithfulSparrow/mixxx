// Does a one-shot input silence gap on ONE deck cause a PERMANENT divergence
// from an otherwise-identical deck? (Issue #16542 follow-up.)
//
// Context: a cache miss makes ReadAheadManager emit silence for the missing
// audio while advancing the file position by the same span
// (readaheadmanager.cpp:147-177). So the missed content is replaced by silence
// and the position stays locked to that (silence-substituted) content -- a
// single deck does not run "ahead" of itself. The open question for the reported
// sweeping flanger is whether the STATEFUL time-stretcher turns an asymmetric
// silence gap (one deck misses, the other does not) into a LASTING phase offset
// between the two decks.
//
// This test answers that at the scaler layer, with no engine, no sync, no
// threads: drive two RubberBand instances at the same fixed rate with
// byte-identical sine input, except instance B gets a short silence gap (one
// deck's cache miss). Then feed both identical sine again and measure the output
// difference long after the gap.
//
// RESULT (asserted below): the divergence is TRANSIENT. RubberBand re-converges
// to bit-identical output within a fraction of a second. A single dropped chunk
// does NOT cause a permanent inter-deck offset at the scaler layer -- so the
// simple "two decks drop different audio -> slide apart -> flanger" theory does
// not hold here. If #16542's persistent drift is real, its cause is elsewhere
// (e.g. per-deck file-position divergence, which has no self-healing path).

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QtDebug>
#include <cmath>
#include <vector>

#include "engine/readaheadmanager.h"
#include "test/mixxxtest.h"
#include "util/sample.h"
#include "util/types.h"
#ifdef __RUBBERBAND__
#include "engine/bufferscalers/enginebufferscalerubberband.h"
#include "engine/bufferscalers/rubberbandworkerpool.h"
#endif

using ::testing::_;
using ::testing::Invoke;
using ::testing::StrictMock;

namespace {

constexpr mixxx::audio::SampleRate kSampleRate = mixxx::audio::SampleRate(44100);
constexpr mixxx::audio::ChannelCount kChannelCount =
        mixxx::audio::ChannelCount::stereo();
constexpr SINT kOutputFrames = 1024;
constexpr SINT kOutputSamples = kOutputFrames * 2;

// A ReadAheadManager mock that serves a 440 Hz sine but emits SILENCE for a
// configurable window of read calls while STILL advancing its read position by
// the full request -- exactly what the engine does to the scaler's input on a
// cache miss (readaheadmanager.cpp:147-177: clear to silence, advance position).
// Two instances with the same gap setting are byte-identical; giving only one a
// gap models a single deck's cache miss.
class GapInjectingReadAheadManagerMock : public ReadAheadManager {
  public:
    GapInjectingReadAheadManagerMock()
            : ReadAheadManager() {
        constexpr int kCycleFrames = 44100;
        m_cycle.resize(kCycleFrames);
        for (int i = 0; i < kCycleFrames; ++i) {
            m_cycle[i] = static_cast<CSAMPLE>(
                    std::sin(2.0 * M_PI * 440.0 * i / kCycleFrames));
        }
    }

    // Emit silence (but keep advancing position) for read calls with index in
    // [gapStartCall, gapEndCall).
    void setGapCalls(int gapStartCall, int gapEndCall) {
        m_gapStartCall = gapStartCall;
        m_gapEndCall = gapEndCall;
    }

    SINT getNextSamplesFake(double dRate,
            CSAMPLE* buffer,
            SINT requested_samples,
            mixxx::audio::ChannelCount channelCount) {
        Q_UNUSED(dRate);
        const int channels = channelCount;
        const bool inGap =
                m_iCallCount >= m_gapStartCall && m_iCallCount < m_gapEndCall;
        for (SINT i = 0; i < requested_samples; i += channels) {
            // Position advances whether or not we are in the gap: a cache miss
            // drops the content but does NOT rewind the file position.
            const CSAMPLE v =
                    inGap ? CSAMPLE_ZERO : m_cycle[m_iReadPosition % m_cycle.size()];
            ++m_iReadPosition;
            for (int ch = 0; ch < channels; ++ch) {
                buffer[i + ch] = v;
            }
        }
        if (inGap) {
            ++m_gapCallsServed;
        }
        ++m_iCallCount;
        return requested_samples;
    }

    int gapCallsServed() const {
        return m_gapCallsServed;
    }

    MOCK_METHOD4(getNextSamples,
            SINT(double dRate,
                    CSAMPLE* buffer,
                    SINT requested_samples,
                    mixxx::audio::ChannelCount channelCount));

  private:
    std::vector<CSAMPLE> m_cycle;
    SINT m_iReadPosition = 0;
    int m_iCallCount = 0;
    int m_gapStartCall = -1;
    int m_gapEndCall = -1;
    int m_gapCallsServed = 0;
};

// Configure a scaler at a fixed rate (set twice to defeat the rate-LERP).
void configureScaler(EngineBufferScale* pScaler, double rate) {
    pScaler->setSignal(kSampleRate, kChannelCount);
    double tempo = rate;
    double pitch = 1.0;
    pScaler->setScaleParameters(1.0, &tempo, &pitch);
    tempo = rate;
    pitch = 1.0;
    pScaler->setScaleParameters(1.0, &tempo, &pitch);
}

} // namespace

#ifdef __RUBBERBAND__
// RubberBand needs the global worker pool to exist (the wrapper queries it to
// decide how many channels each stretcher instance handles).
class EngineBufferScaleGapTest : public MixxxTest {
  protected:
    void SetUp() override {
        RubberBandWorkerPool::createInstance(m_pConfig);
    }
    void TearDown() override {
        RubberBandWorkerPool::destroy();
    }
};

TEST_F(EngineBufferScaleGapTest, SilenceGapDoesNotCausePersistentDivergence) {
    constexpr double kRate = 1.08; // the reporter's +8%
    constexpr int kGapStartCall = 200;
    constexpr int kGapEndCall = 205; // a few input pulls of one-deck silence
    constexpr int kTotalBuffers = 2000;

    StrictMock<GapInjectingReadAheadManagerMock> mockA; // no gap (reference deck)
    StrictMock<GapInjectingReadAheadManagerMock> mockB; // one silence gap
    EXPECT_CALL(mockA, getNextSamples(_, _, _, _))
            .WillRepeatedly(Invoke(&mockA,
                    &GapInjectingReadAheadManagerMock::getNextSamplesFake));
    EXPECT_CALL(mockB, getNextSamples(_, _, _, _))
            .WillRepeatedly(Invoke(&mockB,
                    &GapInjectingReadAheadManagerMock::getNextSamplesFake));
    mockB.setGapCalls(kGapStartCall, kGapEndCall);

    EngineBufferScaleRubberBand scalerA(&mockA);
    EngineBufferScaleRubberBand scalerB(&mockB);
    configureScaler(&scalerA, kRate);
    configureScaler(&scalerB, kRate);

    std::vector<CSAMPLE> outA(kOutputSamples);
    std::vector<CSAMPLE> outB(kOutputSamples);

    double peakDiff = 0.0;
    int peakDiffBuffer = -1;
    int firstDivergentBuffer = -1;
    int lastDivergentBuffer = -1;
    double maxDiffLastQuarter = 0.0; // divergence in the final 25% of the run
    const int lastQuarterStart = kTotalBuffers * 3 / 4;
    for (int i = 0; i < kTotalBuffers; ++i) {
        scalerA.scaleBuffer(outA.data(), kOutputSamples);
        scalerB.scaleBuffer(outB.data(), kOutputSamples);
        double bufMax = 0.0;
        for (SINT s = 0; s < kOutputSamples; ++s) {
            bufMax = std::max(bufMax,
                    static_cast<double>(std::fabs(outA[s] - outB[s])));
        }
        if (bufMax > 1e-6) {
            if (firstDivergentBuffer < 0) {
                firstDivergentBuffer = i;
            }
            lastDivergentBuffer = i;
        }
        if (bufMax > peakDiff) {
            peakDiff = bufMax;
            peakDiffBuffer = i;
        }
        if (i >= lastQuarterStart) {
            maxDiffLastQuarter = std::max(maxDiffLastQuarter, bufMax);
        }
    }

    fprintf(stderr,
            "SilenceGap rate=%.2f gapCallsServed=%d | peakDiff=%.6g @buf%d | "
            "firstDivergent=%d lastDivergent=%d | maxDiffLastQuarter=%.6g "
            "(lastQuarter starts @buf%d, run=%d)\n",
            kRate,
            mockB.gapCallsServed(),
            peakDiff,
            peakDiffBuffer,
            firstDivergentBuffer,
            lastDivergentBuffer,
            maxDiffLastQuarter,
            lastQuarterStart,
            kTotalBuffers);

    // Sanity: the injected gap must actually perturb deck B (else the test is
    // vacuous). It does: a large transient appears right after the gap.
    EXPECT_GT(peakDiff, 0.1)
            << "the silence gap should visibly perturb deck B's output";

    // THE FINDING: the divergence is TRANSIENT. By the final quarter of the run,
    // long after the gap, the two RubberBand instances are bit-identical again.
    // A one-shot input silence gap (one deck's cache miss) does NOT cause a
    // permanent inter-deck phase offset -- RubberBand re-converges. This refutes
    // the simple "two decks drop different audio -> slide apart -> sweeping
    // flanger" theory at the SCALER layer: the drop is a self-healing glitch,
    // not an accumulating desync. If #16542's persistent drift is real, its
    // cause is elsewhere (e.g. per-deck file-position divergence, which has no
    // self-healing path), not RubberBand's response to a single dropped chunk.
    EXPECT_EQ(0.0, maxDiffLastQuarter)
            << "RubberBand did NOT re-converge after the gap: residual diff "
            << maxDiffLastQuarter << " persists into the final quarter. If this "
            << "fails, a single cache miss DOES cause lasting inter-deck "
            << "divergence -- which would support the scaler-layer flanger theory.";
}
#endif
