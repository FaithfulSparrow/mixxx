#include "engine/readaheadmanager.h"

#include <gtest/gtest.h>

#include <QScopedPointer>
#include <QtDebug>

#include "control/controlobject.h"
#include "engine/cachingreader/cachingreader.h"
#include "engine/controls/cuecontrol.h"
#include "engine/controls/loopingcontrol.h"
#include "test/mixxxtest.h"
#include "util/assert.h"
#include "util/defs.h"
#include "util/sample.h"

namespace {
const QString kGroup = "[test]";
} // namespace

class StubReader : public CachingReader {
  public:
    StubReader()
            : CachingReader(kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stereo()) {
    }

    CachingReader::ReadResult read(SINT startSample,
            SINT numSamples,
            bool reverse,
            CSAMPLE* buffer,
            mixxx::audio::ChannelCount channelCount) override {
        Q_UNUSED(startSample);
        Q_UNUSED(reverse);
        Q_UNUSED(channelCount);
        SampleUtil::clear(buffer, numSamples);
        return CachingReader::ReadResult::AVAILABLE;
    }
};

// A reader that serves a deterministic ramp (sample value == absolute frame
// index) so we can tell exactly WHICH audio content landed in the output, but
// reports a cache miss (UNAVAILABLE) for a configurable window of start
// positions. This models the real CachingReader returning UNAVAILABLE while its
// background worker is still decoding a chunk (cachingreader.cpp:489).
//
// #16542 probe: on a cache miss the ReadAheadManager clears the buffer to
// silence but STILL advances m_currentPosition by the full request
// (readaheadmanager.cpp:147-177). If that is so, the audio content under the
// missed window is DROPPED -- position runs ahead of content permanently, with
// no retry. Two decks that miss at DIFFERENT positions then drift apart: the
// sweeping flanger. This reader lets us prove the drop deterministically.
class CacheMissReader : public CachingReader {
  public:
    CacheMissReader()
            : CachingReader(kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stereo()) {
    }

    // Report a cache miss for reads whose start frame is in [missStart, missEnd)
    // (in frames). Outside that window, serve the content ramp.
    void setMissWindowFrames(SINT missStartFrame, SINT missEndFrame) {
        m_missStartSample = missStartFrame * 2; // stereo
        m_missEndSample = missEndFrame * 2;
    }

    CachingReader::ReadResult read(SINT startSample,
            SINT numSamples,
            bool reverse,
            CSAMPLE* buffer,
            mixxx::audio::ChannelCount channelCount) override {
        Q_UNUSED(reverse);
        Q_UNUSED(channelCount);
        if (startSample >= m_missStartSample && startSample < m_missEndSample) {
            // Cache miss of the FIRST required chunk: no samples written, as the
            // real reader does when the head chunk is still decoding.
            return CachingReader::ReadResult::UNAVAILABLE;
        }
        // Content ramp: buffer[i] encodes the absolute sample index it carries,
        // so the caller can verify exactly which content was delivered.
        for (SINT i = 0; i < numSamples; ++i) {
            buffer[i] = static_cast<CSAMPLE>(startSample + i);
        }
        return CachingReader::ReadResult::AVAILABLE;
    }

  private:
    SINT m_missStartSample = -1;
    SINT m_missEndSample = -1;
};

class StubLoopControl : public LoopingControl {
  public:
    StubLoopControl()
            : LoopingControl(kGroup, UserSettingsPointer()) {
    }

    void pushValues(double trigger, double target) {
        m_triggerReturnValues.push_back(
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(trigger));
        m_targetReturnValues.push_back(
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(target));
    }

    mixxx::audio::FramePos nextTrigger(bool reverse,
            mixxx::audio::FramePos currentPosition,
            mixxx::audio::FramePos* pTargetPosition) override {
        Q_UNUSED(reverse);
        Q_UNUSED(currentPosition);
        Q_UNUSED(pTargetPosition);
        RELEASE_ASSERT(!m_targetReturnValues.isEmpty());
        *pTargetPosition = m_targetReturnValues.takeFirst();
        RELEASE_ASSERT(!m_triggerReturnValues.isEmpty());
        return m_triggerReturnValues.takeFirst();
    }

  protected:
    QList<mixxx::audio::FramePos> m_triggerReturnValues;
    QList<mixxx::audio::FramePos> m_targetReturnValues;
};

class StubCueControl : public CueControl {
  public:
    StubCueControl()
            : CueControl(kGroup, UserSettingsPointer()) {
    }

    void pushValues(double trigger, double target) {
        m_triggerReturnValues.push_back(
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(trigger));

        m_targetReturnValues.push_back(
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(target));
    }

    mixxx::audio::FramePos nextTrigger(bool,
            mixxx::audio::FramePos,
            mixxx::audio::FramePos* pTargetPosition,
            mixxx::audio::FrameDiff_t) override {
        RELEASE_ASSERT(!m_targetReturnValues.isEmpty());
        *pTargetPosition = m_targetReturnValues.takeFirst();
        RELEASE_ASSERT(!m_triggerReturnValues.isEmpty());
        return m_triggerReturnValues.takeFirst();
    }

  protected:
    QList<mixxx::audio::FramePos> m_triggerReturnValues;
    QList<mixxx::audio::FramePos> m_targetReturnValues;
};

class ReadAheadManagerTest : public MixxxTest {
  public:
    ReadAheadManagerTest()
            : m_beatClosestCO(ConfigKey(kGroup, "beat_closest")),
              m_beatNextCO(ConfigKey(kGroup, "beat_next")),
              m_beatPrevCO(ConfigKey(kGroup, "beat_prev")),
              m_playCO(ConfigKey(kGroup, "play")),
              m_stopCO(ConfigKey(kGroup, "stop")),
              m_vinylControlCO(ConfigKey(kGroup, "vinylcontrol_enabled")),
              m_vinylControlModeCO(ConfigKey(kGroup, "vinylcontrol_mode")),
              m_passthroughCO(ConfigKey(kGroup, "passthrough")),
              m_indicator250msCO(ConfigKey("[App]", "indicator_250ms")),
              m_indicator500msCO(ConfigKey("[App]", "indicator_500ms")),
              m_quantizeCO(ConfigKey(kGroup, "quantize")),
              m_repeatCO(ConfigKey(kGroup, "repeat")),
              m_slipEnabledCO(ConfigKey(kGroup, "slip_enabled")),
              m_trackSamplesCO(ConfigKey(kGroup, "track_samples")),
              m_pBuffer(SampleUtil::alloc(MAX_BUFFER_LEN)) {
    }

  protected:
    void SetUp() override {
        SampleUtil::clear(m_pBuffer, MAX_BUFFER_LEN);
        m_pReader.reset(new StubReader());
        m_pLoopControl.reset(new StubLoopControl());
        m_pCueControl.reset(new StubCueControl());
        m_pReadAheadManager.reset(new ReadAheadManager(m_pReader.data(),
                m_pLoopControl.data(),
                m_pCueControl.data()));
    }

    ControlObject m_beatClosestCO;
    ControlObject m_beatNextCO;
    ControlObject m_beatPrevCO;
    ControlObject m_playCO;
    ControlObject m_stopCO;
    ControlObject m_vinylControlCO;
    ControlObject m_vinylControlModeCO;
    ControlObject m_passthroughCO;
    ControlObject m_indicator250msCO;
    ControlObject m_indicator500msCO;
    ControlObject m_quantizeCO;
    ControlObject m_repeatCO;
    ControlObject m_slipEnabledCO;
    ControlObject m_trackSamplesCO;
    CSAMPLE* m_pBuffer;
    QScopedPointer<StubReader> m_pReader;
    QScopedPointer<StubLoopControl> m_pLoopControl;
    QScopedPointer<StubCueControl> m_pCueControl;
    QScopedPointer<ReadAheadManager> m_pReadAheadManager;
};

TEST_F(ReadAheadManagerTest, SavedJump) {
    m_pReadAheadManager->notifySeek(0.5);

    for (int i = 0; i < 2; i++) {
        m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    }

    m_pCueControl->pushValues(20, 6);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);

    EXPECT_EQ(20,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 30, mixxx::audio::ChannelCount::stereo()));
    EXPECT_NEAR(6.5, m_pReadAheadManager->getPlaypos(), 1);
    EXPECT_EQ(80,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 80, mixxx::audio::ChannelCount::stereo()));

    EXPECT_NEAR(86.5, m_pReadAheadManager->getPlaypos(), 1);
}

TEST_F(ReadAheadManagerTest, TriggerOnJumpOrLoop) {
    m_pReadAheadManager->notifySeek(0);

    // The jump trigger is located before the loop end
    m_pLoopControl->pushValues(50, 10);
    m_pCueControl->pushValues(40, 20);

    EXPECT_EQ(40,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 100, mixxx::audio::ChannelCount::stereo()));
    EXPECT_NEAR(20, m_pReadAheadManager->getPlaypos(), 1);

    m_pReadAheadManager->notifySeek(0);

    // The jump trigger is located after the loop end
    m_pLoopControl->pushValues(50, 40);
    m_pCueControl->pushValues(60, 30);

    EXPECT_EQ(50,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 100, mixxx::audio::ChannelCount::stereo()));
    EXPECT_NEAR(40, m_pReadAheadManager->getPlaypos(), 1);
}

TEST_F(ReadAheadManagerTest, FractionalFrameLoop) {
    // If we are in reverse, a loop is enabled, and the current playposition
    // is before of the loop, we should seek to the out point of the loop.
    m_pReadAheadManager->notifySeek(0.5);
    // Trigger value means, the sample that triggers the loop (loop in) and the
    // sample we should seek to.
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, kNoTrigger);

    for (int i = 0; i < 6; i++) {
        m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    }

    // read from start to loop trigger, overshoot 0.3
    EXPECT_EQ(20,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 100, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(18,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 80, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(16,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 62, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(18,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 46, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(16,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 28, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(12,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 12, mixxx::audio::ChannelCount::stereo()));

    // start 0.5 to 20.2 = 19.7
    // loop 3.3 to 20.2 = 16.9
    // 100 - 19,7 - 4 * 16,9 = 12,7
    // 12.7 + 3.3 = 16

    // The rounding error must not exceed a half frame (one samples in stereo)
    EXPECT_NEAR(16, m_pReadAheadManager->getPlaypos(), 1);
}

// ---------------------------------------------------------------------------
// #16542 proof-of-concept: a cache miss DROPS audio content (does not defer it).
//
// This is a CHARACTERIZATION test -- it passes by asserting the CURRENT,
// arguably-wrong behaviour, to pin the mechanism down for review. It is not a
// regression guard for desired behaviour; if the drop is later fixed (stall and
// retry instead of skip), the final assertion here must be updated.
//
// Behaviour demonstrated: when CachingReader returns UNAVAILABLE (its background
// decode thread has not produced the chunk yet -- a cache miss), ReadAheadManager
// emits silence for the missing audio but advances m_currentPosition by the full
// request anyway (readaheadmanager.cpp:147-177). There is no retry: when the
// chunk later arrives, playback resumes at the ADVANCED position, so the audio
// content under the missed window is skipped permanently. Position stays locked
// to wall-clock; the audio content falls irrecoverably behind it.
//
// Why this is the likely #16542 "RubberBand gets out of sync" flanger: each deck
// has its OWN CachingReader + decode thread + cache (enginebuffer.cpp:116), so
// two decks playing the same file miss at DIFFERENT moments and drop DIFFERENT
// audio. Both positions keep marching in lockstep (each still "in sync" with the
// clock), but their emitted CONTENT slides apart by the differing dropped
// amounts -- a wandering inter-deck offset = the sweeping flanger. Keylock /
// stems / slip all raise the decode load, so they raise the miss rate, matching
// every condition in the report. A single deck plays the correct DURATION (the
// position clock is never wrong), which is why the reporter's stopwatch test is
// clean even though the audio desyncs -- the drop is invisible to any position
// or frame counter, and only audible as two decks combing against each other.
//
// Proven here deterministically, with no scaler and no threads.
// ---------------------------------------------------------------------------
class CacheMissDropTest : public MixxxTest {
  public:
    CacheMissDropTest()
            : m_beatClosestCO(ConfigKey(kGroup, "beat_closest")),
              m_beatNextCO(ConfigKey(kGroup, "beat_next")),
              m_beatPrevCO(ConfigKey(kGroup, "beat_prev")),
              m_playCO(ConfigKey(kGroup, "play")),
              m_stopCO(ConfigKey(kGroup, "stop")),
              m_vinylControlCO(ConfigKey(kGroup, "vinylcontrol_enabled")),
              m_vinylControlModeCO(ConfigKey(kGroup, "vinylcontrol_mode")),
              m_passthroughCO(ConfigKey(kGroup, "passthrough")),
              m_indicator250msCO(ConfigKey("[App]", "indicator_250ms")),
              m_indicator500msCO(ConfigKey("[App]", "indicator_500ms")),
              m_quantizeCO(ConfigKey(kGroup, "quantize")),
              m_repeatCO(ConfigKey(kGroup, "repeat")),
              m_slipEnabledCO(ConfigKey(kGroup, "slip_enabled")),
              m_trackSamplesCO(ConfigKey(kGroup, "track_samples")),
              m_pBuffer(SampleUtil::alloc(MAX_BUFFER_LEN)) {
    }

  protected:
    void SetUp() override {
        SampleUtil::clear(m_pBuffer, MAX_BUFFER_LEN);
        m_pReader.reset(new CacheMissReader());
        m_pLoopControl.reset(new StubLoopControl());
        m_pCueControl.reset(new StubCueControl());
        m_pReadAheadManager.reset(new ReadAheadManager(m_pReader.data(),
                m_pLoopControl.data(),
                m_pCueControl.data()));
    }

    // Helper: push "no trigger" so the RAMAN does a plain forward read.
    void noTriggers(int n) {
        for (int i = 0; i < n; ++i) {
            m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
            m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
        }
    }

    ControlObject m_beatClosestCO;
    ControlObject m_beatNextCO;
    ControlObject m_beatPrevCO;
    ControlObject m_playCO;
    ControlObject m_stopCO;
    ControlObject m_vinylControlCO;
    ControlObject m_vinylControlModeCO;
    ControlObject m_passthroughCO;
    ControlObject m_indicator250msCO;
    ControlObject m_indicator500msCO;
    ControlObject m_quantizeCO;
    ControlObject m_repeatCO;
    ControlObject m_slipEnabledCO;
    ControlObject m_trackSamplesCO;
    CSAMPLE* m_pBuffer;
    QScopedPointer<CacheMissReader> m_pReader;
    QScopedPointer<StubLoopControl> m_pLoopControl;
    QScopedPointer<StubCueControl> m_pCueControl;
    QScopedPointer<ReadAheadManager> m_pReadAheadManager;
};

TEST_F(CacheMissDropTest, CacheMissDropsContentAndAdvancesPosition) {
    constexpr SINT kRead = 100; // samples per getNextSamples call (50 frames)
    m_pReadAheadManager->notifySeek(0);

    // First read: no miss. Content ramp starts at sample 0.
    noTriggers(1);
    SINT got = m_pReadAheadManager->getNextSamples(
            1.0, m_pBuffer, kRead, mixxx::audio::ChannelCount::stereo());
    EXPECT_EQ(kRead, got);
    EXPECT_FLOAT_EQ(0.0f, m_pBuffer[0]);   // delivered content == sample 0
    EXPECT_FLOAT_EQ(99.0f, m_pBuffer[99]); // ... through sample 99
    const double posAfterFirst = m_pReadAheadManager->getPlaypos();
    EXPECT_NEAR(kRead, posAfterFirst, 1); // position advanced by the read

    // Second read: force a cache miss covering this window
    // (start frame 50 == sample 100, length 50 frames).
    m_pReader->setMissWindowFrames(50, 100);
    noTriggers(1);
    got = m_pReadAheadManager->getNextSamples(
            1.0, m_pBuffer, kRead, mixxx::audio::ChannelCount::stereo());
    EXPECT_EQ(kRead, got) << "RAMAN reports the full read even on a cache miss";
    // The buffer is SILENCE -- the content (samples 100..199) was NOT delivered.
    EXPECT_FLOAT_EQ(0.0f, m_pBuffer[0]);
    EXPECT_FLOAT_EQ(0.0f, m_pBuffer[99]);
    // ...but the POSITION advanced past that content anyway.
    const double posAfterMiss = m_pReadAheadManager->getPlaypos();
    EXPECT_NEAR(2 * kRead, posAfterMiss, 1)
            << "position advanced through the missed window";

    // Third read: cache warm again, but this buffer carries the 0->1 ramping
    // gain the RAMAN applies to mask the post-silence pop (readaheadmanager.cpp:157),
    // so its raw values are attenuated. We don't assert on it.
    m_pReader->setMissWindowFrames(-1, -1); // disable miss
    noTriggers(1);
    got = m_pReadAheadManager->getNextSamples(
            1.0, m_pBuffer, kRead, mixxx::audio::ChannelCount::stereo());
    EXPECT_EQ(kRead, got);
    EXPECT_NEAR(3 * kRead, m_pReadAheadManager->getPlaypos(), 1)
            << "position kept advancing through the miss to sample 300";

    // Fourth read: no miss, no ramp (the miss flag was cleared on the third
    // read), so content is the pure ramp value. THE critical assertion: the
    // content here is sample 300+, and there is NO sign of the missed 100..199 --
    // that audio was permanently DROPPED, never replayed. Position (now 400) and
    // content (300) are locked, but content is exactly the missed 100 samples
    // behind where it would be with no miss. Two decks missing at DIFFERENT
    // positions accumulate DIFFERENT drops and slide apart: the #16542 flanger.
    noTriggers(1);
    got = m_pReadAheadManager->getNextSamples(
            1.0, m_pBuffer, kRead, mixxx::audio::ChannelCount::stereo());
    EXPECT_EQ(kRead, got);
    // This is the defect, asserted as-is: had the miss STALLED (held position to
    // replay 100..199 once decoded), content here would be 200. Instead it is
    // 300 -- the missed 100 samples were dropped and the deck is now permanently
    // 100 samples of content ahead of where a no-miss deck would be.
    EXPECT_FLOAT_EQ(300.0f, m_pBuffer[0])
            << "after a cache miss, playback resumed past the missed window: "
               "content 100..199 was permanently dropped, not deferred.";
    EXPECT_FLOAT_EQ(399.0f, m_pBuffer[99]);
}
