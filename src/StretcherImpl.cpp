/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Rubber Band
    An audio time-stretching and pitch-shifting library.
    Copyright 2007 Chris Cannam.
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "StretcherImpl.h"
#include "PercussiveAudioCurve.h"
#include "HighFrequencyAudioCurve.h"
#include "ConstantAudioCurve.h"
#include "StretchCalculator.h"
#include "StretcherChannelData.h"
#include "Resampler.h"

#include <cassert>
#include <cmath>
#include <set>
#include <map>

using std::cerr;
using std::endl;
using std::vector;
using std::map;
using std::set;
using std::max;
using std::min;

namespace RubberBand {

const size_t
RubberBandStretcher::Impl::m_defaultIncrement = 256;

const size_t
RubberBandStretcher::Impl::m_defaultWindowSize = 2048;

int
RubberBandStretcher::Impl::m_defaultDebugLevel = 0;


RubberBandStretcher::Impl::Impl(RubberBandStretcher *stretcher,
                                size_t sampleRate,
                                size_t channels,
                                Options options,
                                double initialTimeRatio,
                                double initialPitchScale) :
    m_stretcher(stretcher),
    m_channels(channels),
    m_timeRatio(initialTimeRatio),
    m_pitchScale(initialPitchScale),
    m_windowSize(m_defaultWindowSize),
    m_increment(m_defaultIncrement),
    m_outbufSize(m_defaultWindowSize * 2),
    m_maxProcessSize(m_defaultWindowSize),
    m_expectedInputDuration(0),
    m_threaded(false),
    m_realtime(false),
    m_options(options),
    m_debugLevel(m_defaultDebugLevel),
    m_mode(JustCreated),
    m_window(0),
    m_studyFFT(0),
    m_inputDuration(0),
    m_lastProcessOutputIncrements(16),
    m_lastProcessPhaseResetDf(16),
    m_phaseResetAudioCurve(0),
    m_stretchAudioCurve(0),
    m_stretchCalculator(0),
    m_freq0(600),
    m_freq1(1200),
    m_freq2(12000),
    m_baseWindowSize(m_defaultWindowSize)
{
    if (m_debugLevel > 0) {
        cerr << "RubberBandStretcher::Impl::Impl: rate = " << m_stretcher->m_sampleRate << ", options = " << options << endl;
    }

    // Window size will vary according to the audio sample rate, but
    // we don't let it drop below the 48k default
    m_rateMultiple = float(m_stretcher->m_sampleRate) / 48000.f;
    if (m_rateMultiple < 1.f) m_rateMultiple = 1.f;
    m_baseWindowSize = roundUp(int(m_defaultWindowSize * m_rateMultiple));

    if ((options & OptionWindowShort) || (options & OptionWindowLong)) {
        if ((options & OptionWindowShort) && (options & OptionWindowLong)) {
            cerr << "RubberBandStretcher::Impl::Impl: Cannot specify OptionWindowLong and OptionWindowShort together; falling back to OptionWindowStandard" << endl;
        } else if (options & OptionWindowShort) {
            m_baseWindowSize = m_baseWindowSize / 2;
            if (m_debugLevel > 0) {
                cerr << "setting baseWindowSize to " << m_baseWindowSize << endl;
            }
        } else if (options & OptionWindowLong) {
            m_baseWindowSize = m_baseWindowSize * 2;
            if (m_debugLevel > 0) {
                cerr << "setting baseWindowSize to " << m_baseWindowSize << endl;
            }
        }
        m_windowSize = m_baseWindowSize;
        m_outbufSize = m_baseWindowSize * 2;
        m_maxProcessSize = m_baseWindowSize;
    }

    if (m_options & OptionProcessRealTime) {

        m_realtime = true;

        if (!(m_options & OptionStretchPrecise)) {
            cerr << "RubberBandStretcher::Impl::Impl: Real-time mode: enabling OptionStretchPrecise" << endl;
            m_options |= OptionStretchPrecise;
        }
    }

    if (m_channels > 1) {

        if (!m_realtime &&
            !(m_options & OptionThreadingNone) &&
            Thread::threadingAvailable() &&
            system_is_multiprocessor()) {

            m_threaded = true;
        
            if (m_debugLevel > 0) {
                cerr << "Going multithreaded..." << endl;
            }
        }
    }

    configure();
}

RubberBandStretcher::Impl::~Impl()
{
    if (m_threaded) {
        MutexLocker locker(&m_threadSetMutex);
        for (set<ProcessThread *>::iterator i = m_threadSet.begin();
             i != m_threadSet.end(); ++i) {
            if (m_debugLevel > 0) {
                cerr << "RubberBandStretcher::~RubberBandStretcher: joining (channel " << *i << ")" << endl;
            }
            (*i)->wait();
            delete *i;
        }
    }

    for (size_t c = 0; c < m_channels; ++c) {
        delete m_channelData[c];
    }

    delete m_phaseResetAudioCurve;
    delete m_stretchAudioCurve;
    delete m_stretchCalculator;
    delete m_studyFFT;

    for (map<size_t, Window<float> *>::iterator i = m_windows.begin();
         i != m_windows.end(); ++i) {
        delete i->second;
    }
}

void
RubberBandStretcher::Impl::reset()
{
    //!!! does not do the right thing in threaded mode

    if (m_threaded) m_threadSetMutex.lock();

    for (size_t c = 0; c < m_channels; ++c) {
        delete m_channelData[c];
        m_channelData[c] = new ChannelData(m_windowSize, m_outbufSize);
    }
    m_mode = JustCreated;
    if (m_phaseResetAudioCurve) m_phaseResetAudioCurve->reset();
    if (m_stretchAudioCurve) m_stretchAudioCurve->reset();
    m_inputDuration = 0;

    if (m_threaded) m_threadSetMutex.unlock();
//    m_done = false;
}

void
RubberBandStretcher::Impl::setTimeRatio(double ratio)
{
    if (!m_realtime) {
        if (m_mode == Studying || m_mode == Processing) {
            cerr << "RubberBandStretcher::Impl::setTimeRatio: Cannot set ratio while studying or processing in non-RT mode" << endl;
            return;
        }
    }

    if (ratio == m_timeRatio) return;
    m_timeRatio = ratio;

    reconfigure();
}

void
RubberBandStretcher::Impl::setPitchScale(double fs)
{
    if (!m_realtime) {
        if (m_mode == Studying || m_mode == Processing) {
            cerr << "RubberBandStretcher::Impl::setPitchScale: Cannot set ratio while studying or processing in non-RT mode" << endl;
            return;
        }
    }

    if (fs == m_pitchScale) return;
    m_pitchScale = fs;

    reconfigure();
}

double
RubberBandStretcher::Impl::getTimeRatio() const
{
    return m_timeRatio;
}

double
RubberBandStretcher::Impl::getPitchScale() const
{
    return m_pitchScale;
}

void
RubberBandStretcher::Impl::setExpectedInputDuration(size_t samples)
{
    if (samples == m_expectedInputDuration) return;
    m_expectedInputDuration = samples;

    reconfigure();
}

void
RubberBandStretcher::Impl::setMaxProcessSize(size_t samples)
{
    if (samples <= m_maxProcessSize) return;
    m_maxProcessSize = samples;

    reconfigure();
}

float
RubberBandStretcher::Impl::getFrequencyCutoff(int n) const
{
    switch (n) {
    case 0: return m_freq0;
    case 1: return m_freq1;
    case 2: return m_freq2;
    }
    return 0.f;
}

void
RubberBandStretcher::Impl::setFrequencyCutoff(int n, float f)
{
    switch (n) {
    case 0: m_freq0 = f; break;
    case 1: m_freq1 = f; break;
    case 2: m_freq2 = f; break;
    }
}

double
RubberBandStretcher::Impl::getEffectiveRatio() const
{
    // Returns the ratio that the internal time stretcher needs to
    // achieve, not the resulting duration ratio of the output (which
    // is simply m_timeRatio).

    // A frequency shift is achieved using an additional time shift,
    // followed by resampling back to the original time shift to
    // change the pitch.  Note that the resulting frequency change is
    // fixed, as it is effected by the resampler -- in contrast to
    // time shifting, which is variable aiming to place the majority
    // of the stretch or squash in low-interest regions of audio.

    return m_timeRatio * m_pitchScale;
}

size_t
RubberBandStretcher::Impl::roundUp(size_t value)
{
    if (!(value & (value - 1))) return value;
    int bits = 0;
    while (value) { ++bits; value >>= 1; }
    value = 1 << bits;
    return value;
}

void
RubberBandStretcher::Impl::calculateSizes()
{
    size_t inputIncrement = m_defaultIncrement;
    size_t windowSize = m_baseWindowSize;
    size_t outputIncrement;

    double r = getEffectiveRatio();

    if (m_realtime) {

        // use a fixed input increment

        inputIncrement = roundUp(int(m_defaultIncrement * m_rateMultiple));

        if (r < 1) {
            outputIncrement = int(floor(inputIncrement * r));
            if (outputIncrement < 1) {
                outputIncrement = 1;
                inputIncrement = roundUp(lrint(ceil(outputIncrement / r)));
                windowSize = inputIncrement * 4;
            }
        } else {
            outputIncrement = int(ceil(inputIncrement * r));
            while (outputIncrement > 1024 && inputIncrement > 1) {
                inputIncrement /= 2;
                outputIncrement = lrint(ceil(inputIncrement * r));
            }
            windowSize = std::max(windowSize, roundUp(outputIncrement * 6));
            if (r > 5) while (windowSize < 8192) windowSize *= 2;
        }

    } else {

        // use a variable increment

        if (r < 1) {
            inputIncrement = windowSize / 4;
            while (inputIncrement >= 512) inputIncrement /= 2;
            outputIncrement = int(floor(inputIncrement * r));
            if (outputIncrement < 1) {
                outputIncrement = 1;
                inputIncrement = roundUp(lrint(ceil(outputIncrement / r)));
                windowSize = inputIncrement * 4;
            }
        } else {
            outputIncrement = windowSize / 6;
            inputIncrement = int(outputIncrement / r);
            while (outputIncrement > 1024 && inputIncrement > 1) {
                outputIncrement /= 2;
                inputIncrement = int(outputIncrement / r);
            }
            windowSize = std::max(windowSize, roundUp(outputIncrement * 6));
            if (r > 5) while (windowSize < 8192) windowSize *= 2;
        }
    }        

    if (m_expectedInputDuration > 0) {
        while (inputIncrement * 4 > m_expectedInputDuration &&
               inputIncrement > 1) {
            inputIncrement /= 2;
        }
    }

    // windowSize can be almost anything, but it can't be greater than
    // 4 * m_baseWindowSize unless ratio is less than 1/1024.

    m_windowSize = windowSize;
    m_increment = inputIncrement;

    // When squashing, the greatest theoretically possible output
    // increment is the input increment.  When stretching adaptively
    // the sky's the limit in principle, but we expect
    // StretchCalculator to restrict itself to using no more than
    // twice the basic output increment (i.e. input increment times
    // ratio) for any chunk.

    if (m_debugLevel > 0) {
        cerr << "configure: effective ratio = " << getEffectiveRatio() << endl;
        cerr << "configure: window size = " << m_windowSize << ", increment = " << m_increment << " (approx output increment = " << int(lrint(m_increment * getEffectiveRatio())) << ")" << endl;
    }

    if (m_windowSize > m_maxProcessSize) {
        m_maxProcessSize = m_windowSize;
    }

    m_outbufSize =
        size_t
        (ceil(max
              (m_maxProcessSize / m_pitchScale,
               m_windowSize * 2 * (m_timeRatio > 1.f ? m_timeRatio : 1.f))));

    if (m_realtime) {
        // This headroom is so as to try to avoid reallocation when
        // the pitch scale changes
        m_outbufSize = m_outbufSize * 16;
    } else {
        if (m_threaded) {
            // This headroom is to permit the processing threads to
            // run ahead of the buffer output drainage; the exact
            // amount of headroom is a question of tuning rather than
            // results
            m_outbufSize = m_outbufSize * 16;
        }
    }

    if (m_debugLevel > 0) {
        cerr << "configure: outbuf size = " << m_outbufSize << endl;
    }
    
    //!!! for very long stretches (e.g. x5), this is necessary; for
    //even longer ones (e.g. x10), even more of an outbuf is
    //necessary. clearly something wrong in our calculations... or do
    //we just need to ensure client calls setMaxProcessSize?
//    if (!m_realtime && !m_threaded) {
//        m_outbufSize = m_outbufSize * 10;
//    }
}

void
RubberBandStretcher::Impl::configure()
{
    size_t prevWindowSize = m_windowSize;
    size_t prevOutbufSize = m_outbufSize;
    if (m_windows.empty()) {
        prevWindowSize = 0;
        prevOutbufSize = 0;
    }

    calculateSizes();

    bool windowSizeChanged = (prevWindowSize != m_windowSize);
    bool outbufSizeChanged = (prevOutbufSize != m_outbufSize);

    // This function may be called at any time in non-RT mode, after a
    // parameter has changed.  It shouldn't be legal to call it after
    // processing has already begun.

    // This function is only called once (on construction) in RT
    // mode.  After that reconfigure() does the work in a hopefully
    // RT-safe way.

    set<size_t> windowSizes;
    if (m_realtime) {
        windowSizes.insert(m_baseWindowSize);
        windowSizes.insert(m_baseWindowSize * 2);
        windowSizes.insert(m_baseWindowSize * 4);
    }
    windowSizes.insert(m_windowSize);

    if (windowSizeChanged) {

        for (set<size_t>::const_iterator i = windowSizes.begin();
             i != windowSizes.end(); ++i) {
            if (m_windows.find(*i) == m_windows.end()) {
                m_windows[*i] = new Window<float>(HanningWindow, *i);
            }
        }
        m_window = m_windows[m_windowSize];

        if (m_debugLevel > 0) {
            cerr << "Window area: " << m_window->getArea() << "; synthesis window area: " << m_window->getArea() << endl;
        }
    }

    if (windowSizeChanged || outbufSizeChanged) {
        
        for (size_t c = 0; c < m_channelData.size(); ++c) {
            delete m_channelData[c];
        }
        m_channelData.clear();

        for (size_t c = 0; c < m_channels; ++c) {
            m_channelData.push_back
                (new ChannelData(windowSizes, m_windowSize, m_outbufSize));
        }
    }

    if (!m_realtime && windowSizeChanged) {
        delete m_studyFFT;
        m_studyFFT = new FFT(m_windowSize);
        m_studyFFT->initFloat();
    }

    if (m_pitchScale != 1.0 || m_realtime) {

        for (size_t c = 0; c < m_channels; ++c) {

            if (m_channelData[c]->resampler) continue;

            m_channelData[c]->resampler =
                new Resampler(Resampler::FastestTolerable, 1, 4096 * 16);

            // rbs is the amount of buffer space we think we'll need
            // for resampling; but allocate a sensible amount in case
            // the pitch scale changes during use
            size_t rbs = 
                lrintf(ceil((m_increment * m_timeRatio * 2) / m_pitchScale));
            if (rbs < m_increment * 16) rbs = m_increment * 16;
            m_channelData[c]->resamplebufSize = rbs;
            m_channelData[c]->resamplebuf = new float[rbs];
        }
    }
    
    delete m_phaseResetAudioCurve;
    m_phaseResetAudioCurve = new PercussiveAudioCurve(m_stretcher->m_sampleRate,
                                                      m_windowSize);

    // stretchAudioCurve unused in RT mode; phaseResetAudioCurve and
    // stretchCalculator however are used in all modes

    if (!m_realtime) {
        delete m_stretchAudioCurve;
        if (!(m_options & OptionStretchPrecise)) {
            //!!! probably adaptively-whitened spectral difference curve
            //would be better
            m_stretchAudioCurve = new HighFrequencyAudioCurve
                (m_stretcher->m_sampleRate, m_windowSize);
        } else {
            m_stretchAudioCurve = new ConstantAudioCurve
                (m_stretcher->m_sampleRate, m_windowSize);
        }
    }

    delete m_stretchCalculator;
    m_stretchCalculator = new StretchCalculator
        (m_stretcher->m_sampleRate, m_increment,
         !(m_options & OptionTransientsSmooth));

    m_stretchCalculator->setDebugLevel(m_debugLevel);
    m_inputDuration = 0;

    // Prepare the inbufs with half a chunk of emptiness.  The centre
    // point of the first processing chunk for the onset detector
    // should be the first sample of the audio, and we continue until
    // we can no longer centre a chunk within the input audio.  The
    // number of onset detector chunks will be the number of audio
    // samples input, divided by the input increment, plus one.

    // In real-time mode, we don't do this prefill -- it's better to
    // start with a swoosh than introduce more latency, and we don't
    // want gaps when the ratio changes.

    if (!m_realtime) {
        for (size_t c = 0; c < m_channels; ++c) {
            m_channelData[c]->reset();
            m_channelData[c]->inbuf->zero(m_windowSize/2);
        }
    }
}


//!!! separated out from configure() for the moment so we can look at
// the logic of it
void
RubberBandStretcher::Impl::reconfigure()
{
    if (!m_realtime) {
        if (m_mode == Studying) {
            // stop and calculate the stretch curve so far, then reset
            // the df vectors
            calculateStretch();
            m_phaseResetDf.clear();
            m_stretchDf.clear();
            m_inputDuration = 0;
        }
        configure();
    }

    size_t prevWindowSize = m_windowSize;
    size_t prevOutbufSize = m_outbufSize;

    calculateSizes();

    // There are various allocations in this function, but they should
    // never happen in normal use -- they just recover from the case
    // where not all of the things we need were correctly created when
    // we first configured (for whatever reason).  This is intended to
    // be "effectively" realtime safe.  The same goes for
    // ChannelData::setOutbufSize and setWindowSize.

    if (m_windowSize != prevWindowSize) {

        if (m_windows.find(m_windowSize) == m_windows.end()) {
            std::cerr << "WARNING: reconfigure(): window allocation (size " << m_windowSize << ") required in RT mode" << std::endl;
            m_windows[m_windowSize] = new Window<float>(HanningWindow, m_windowSize);
        }
        m_window = m_windows[m_windowSize];

        for (size_t c = 0; c < m_channels; ++c) {
            m_channelData[c]->setWindowSize(m_windowSize);
        }
    }

    if (m_outbufSize != prevOutbufSize) {
        for (size_t c = 0; c < m_channels; ++c) {
            m_channelData[c]->setOutbufSize(m_outbufSize);
        }
    }

    if (m_pitchScale != 1.0) {
        for (size_t c = 0; c < m_channels; ++c) {

            if (m_channelData[c]->resampler) continue;

            std::cerr << "WARNING: reconfigure(): resampler construction required in RT mode" << std::endl;

            m_channelData[c]->resampler =
                new Resampler(Resampler::FastestTolerable, 1, m_windowSize);

            m_channelData[c]->resamplebufSize =
                lrintf(ceil((m_increment * m_timeRatio * 2) / m_pitchScale));
            m_channelData[c]->resamplebuf =
                new float[m_channelData[c]->resamplebufSize];
        }
    }

    if (m_windowSize != prevWindowSize) {
        m_phaseResetAudioCurve->setWindowSize(m_windowSize);
    }
}

size_t
RubberBandStretcher::Impl::getLatency() const
{
    if (!m_realtime) return 0;
    return int((m_windowSize/2) / m_pitchScale + 1);
}

void
RubberBandStretcher::Impl::setTransientsOption(Options options)
{
    if (!m_realtime) {
        cerr << "RubberBandStretcher::Impl::setTransientsOption: Not permissible in non-realtime mode" << endl;
        return;
    }
    m_options &= ~(OptionTransientsMixed |
                   OptionTransientsSmooth |
                   OptionTransientsCrisp);
    m_options |= options;

    m_stretchCalculator->setUseHardPeaks
        (!(m_options & OptionTransientsSmooth));
}

void
RubberBandStretcher::Impl::setPhaseOption(Options options)
{
    m_options &= ~(OptionPhaseAdaptive |
                   OptionPhasePeakLocked |
                   OptionPhaseIndependent);
    m_options |= options;
}

void
RubberBandStretcher::Impl::study(const float *const *input, size_t samples, bool final)
{
    if (m_realtime) {
        if (m_debugLevel > 1) {
            cerr << "RubberBandStretcher::Impl::study: Not meaningful in realtime mode" << endl;
        }
        return;
    }

    if (m_mode == Processing || m_mode == Finished) {
        cerr << "RubberBandStretcher::Impl::study: Cannot study after processing" << endl;
        return;
    }
    m_mode = Studying;
    
    size_t consumed = 0;

    ChannelData &cd = *m_channelData[0];
    RingBuffer<float> &inbuf = *cd.inbuf;

    const float *mixdown;
    float *mdalloc = 0;

    if (m_channels > 1 || final) {
        // mix down into a single channel for analysis
        mdalloc = new float[samples];
        for (size_t i = 0; i < samples; ++i) {
            if (i < samples) {
                mdalloc[i] = input[0][i];
            } else {
                mdalloc[i] = 0.f;
            }
        }
        for (size_t c = 1; c < m_channels; ++c) {
            for (size_t i = 0; i < samples; ++i) {
                mdalloc[i] += input[c][i];
            }
        }
        for (size_t i = 0; i < samples; ++i) {
            mdalloc[i] /= m_channels;
        }
        mixdown = mdalloc;
    } else {
        mixdown = input[0];
    }

    while (consumed < samples) {

	size_t writable = inbuf.getWriteSpace();
	writable = min(writable, samples - consumed);

	if (writable == 0) {
            // warn
            cerr << "WARNING: writable == 0 (consumed = " << consumed << ", samples = " << samples << ")" << endl;
	} else {
            inbuf.write(mixdown + consumed, writable);
            consumed += writable;
        }

	while ((inbuf.getReadSpace() >= m_windowSize) ||
               (final && (inbuf.getReadSpace() >= m_windowSize/2))) {

	    // We know we have at least m_windowSize samples available
	    // in m_inbuf.  We need to peek m_windowSize of them for
	    // processing, and then skip m_increment to advance the
	    // read pointer.

            // cd.accumulator is not otherwise used during studying,
            // so we can use it as a temporary buffer here

            size_t got = inbuf.peek(cd.accumulator, m_windowSize);
            assert(final || got == m_windowSize);

            m_window->cut(cd.accumulator);

            // We don't need the fftshift for studying, as we're only
            // interested in magnitude

            m_studyFFT->forwardMagnitude(cd.accumulator, cd.fltbuf);

            float df = m_phaseResetAudioCurve->process(cd.fltbuf, m_increment);
            m_phaseResetDf.push_back(df);

//            cout << m_phaseResetDf.size() << " [" << final << "] -> " << df << " \t: ";

            df = m_stretchAudioCurve->process(cd.fltbuf, m_increment);
            m_stretchDf.push_back(df);

//            cout << df << endl;

            // We have augmented the input by m_windowSize/2 so
            // that the first chunk is centred on the first audio
            // sample.  We want to ensure that m_inputDuration
            // contains the exact input duration without including
            // this extra bit.  We just add up all the increments
            // here, and deduct the extra afterwards.

            m_inputDuration += m_increment;
//                cerr << "incr input duration by increment: " << m_increment << " -> " << m_inputDuration << endl;
            inbuf.skip(m_increment);
	}
    }

    if (final) {
        int rs = inbuf.getReadSpace();
        m_inputDuration += rs;
//        cerr << "incr input duration by read space: " << rs << " -> " << m_inputDuration << endl;

        if (m_inputDuration > m_windowSize/2) { // deducting the extra
            m_inputDuration -= m_windowSize/2;
        }
    }

    if (m_channels > 1) delete[] mdalloc;
}

vector<int>
RubberBandStretcher::Impl::getOutputIncrements() const
{
    if (!m_realtime) {
        return m_outputIncrements;
    } else {
        vector<int> increments;
        while (m_lastProcessOutputIncrements.getReadSpace() > 0) {
            increments.push_back(m_lastProcessOutputIncrements.readOne());
        }
        return increments;
    }
}

vector<float>
RubberBandStretcher::Impl::getPhaseResetCurve() const
{
    if (!m_realtime) {
        return m_phaseResetDf;
    } else {
        vector<float> df;
        while (m_lastProcessPhaseResetDf.getReadSpace() > 0) {
            df.push_back(m_lastProcessPhaseResetDf.readOne());
        }
        return df;
    }
}

vector<int>
RubberBandStretcher::Impl::getExactTimePoints() const
{
    std::vector<int> points;
    if (!m_realtime) {
        std::vector<StretchCalculator::Peak> peaks =
            m_stretchCalculator->getLastCalculatedPeaks();
        for (size_t i = 0; i < peaks.size(); ++i) {
            points.push_back(peaks[i].chunk);
        }
    }
    return points;
}

void
RubberBandStretcher::Impl::calculateStretch()
{
    std::vector<int> increments = m_stretchCalculator->calculate
        (getEffectiveRatio(),
         m_inputDuration,
         m_phaseResetDf,
         m_stretchDf);

    if (m_outputIncrements.empty()) m_outputIncrements = increments;
    else {
        for (size_t i = 0; i < increments.size(); ++i) {
            m_outputIncrements.push_back(increments[i]);
        }
    }
    
    return;
}

void
RubberBandStretcher::Impl::setDebugLevel(int level)
{
    m_debugLevel = level;
    if (m_stretchCalculator) m_stretchCalculator->setDebugLevel(level);
}	

size_t
RubberBandStretcher::Impl::getSamplesRequired() const
{
    size_t reqd = 0;

    for (size_t c = 0; c < m_channels; ++c) {

        size_t reqdHere = 0;

        ChannelData &cd = *m_channelData[c];
        RingBuffer<float> &inbuf = *cd.inbuf;

        size_t rs = inbuf.getReadSpace();

        // See notes in testInbufReadSpace below

        if (rs < m_windowSize && !cd.draining) {
            
            if (cd.inputSize == -1) {
                reqdHere = m_windowSize - rs;
                if (reqdHere > reqd) reqd = reqdHere;
                continue;
            }
        
            if (rs == 0) {
                reqdHere = m_windowSize;
                if (reqdHere > reqd) reqd = reqdHere;
                continue;
            }
        }
    }
    
    return reqd;
}    

void
RubberBandStretcher::Impl::process(const float *const *input, size_t samples, bool final)
{
    if (m_mode == Finished) {
        cerr << "RubberBandStretcher::Impl::process: Cannot process again after final chunk" << endl;
        return;
    }

    if (m_mode == JustCreated || m_mode == Studying) {

        //!!! m_studying isn't the right test for "is this the first
        //time we've processed?"

        if (m_mode == Studying) {
            calculateStretch();
        }

        for (size_t c = 0; c < m_channels; ++c) {
            m_channelData[c]->reset();
            m_channelData[c]->inbuf->zero(m_windowSize/2);
        }

        if (m_threaded) {
            MutexLocker locker(&m_threadSetMutex);

            for (size_t c = 0; c < m_channels; ++c) {
                ProcessThread *thread = new ProcessThread(this, c);
                m_threadSet.insert(thread);
                thread->start();
            }
            
            if (m_debugLevel > 0) {
                cerr << m_channels << " threads created" << endl;
            }
        }
        
        m_mode = Processing;
    }

    bool allConsumed = false;

    map<size_t, size_t> consumed;
    for (size_t c = 0; c < m_channels; ++c) {
        consumed[c] = 0;
    }

    while (!allConsumed) {

//        cerr << "process looping" << endl;

//#ifndef NO_THREADING
//        if (m_threaded) {
//            pthread_mutex_lock(&m_inputProcessedMutex);
//        }
//#endif

        // In a threaded mode, our "consumed" counters only indicate
        // the number of samples that have been taken into the input
        // ring buffers waiting to be processed by the process thread.
        // In non-threaded mode, "consumed" counts the number that
        // have actually been processed.

        allConsumed = true;
        for (size_t c = 0; c < m_channels; ++c) {
            consumed[c] += consumeChannel(c,
                                          input[c] + consumed[c],
                                          samples - consumed[c]);
            if (consumed[c] < samples) {
                allConsumed = false;
//                cerr << "process: waiting on input consumption for channel " << c << endl;
            } else {
                if (final) {
                    m_channelData[c]->inputSize = m_channelData[c]->inCount;
                }
//                cerr << "process: happy with channel " << c << endl;
            }
            if (!m_threaded && !m_realtime) {
                processChunks(c);
            }
        }

        if (m_realtime) {
            // When running in real time, we need to process both
            // channels in step because we will need to use the sum of
            // their frequency domain representations as the input to
            // the realtime onset detector
            processOneChunk();
        }

        if (m_threaded) {
            m_dataAvailable.signal();
            m_spaceAvailable.lock();
            if (!allConsumed) {
                m_spaceAvailable.wait(500);
            }
/*
        } else {
            if (!allConsumed) {
                cerr << "RubberBandStretcher::Impl::process: ERROR: Too much data provided to process() call -- either call setMaxProcessSize() beforehand, or provide only getSamplesRequired() frames at a time" << endl;
                for (size_t c = 0; c < m_channels; ++c) {
                    cerr << "channel " << c << ": " << samples << " provided, " << consumed[c] << " consumed" << endl;
                }
//                break;
            }
*/
        }
    }
    
//    cerr << "process returning" << endl;

    if (final) m_mode = Finished;
}

size_t
RubberBandStretcher::Impl::consumeChannel(size_t c, const float *input, size_t samples)
{
    size_t consumed = 0;
    
    ChannelData &cd = *m_channelData[c];
    RingBuffer<float> &inbuf = *cd.inbuf;
    
    while (consumed < samples) {

	size_t writable = inbuf.getWriteSpace();

//        cerr << "channel " << c << ": samples remaining = " << samples - consumed << ", writable space = " << writable << endl;

	writable = min(writable, samples - consumed);

	if (writable == 0) {
            // warn
//            cerr << "WARNING: writable == 0 for ch " << c << " (consumed = " << consumed << ", samples = " << samples << ")" << endl;
            return consumed;
	} else {
            inbuf.write(input + consumed, writable);
            consumed += writable;
            cd.inCount += writable;
        }
    }

    return samples;
}


}
