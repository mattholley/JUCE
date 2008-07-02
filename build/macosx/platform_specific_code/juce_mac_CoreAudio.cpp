/*
  ==============================================================================

   This file is part of the JUCE library - "Jules' Utility Class Extensions"
   Copyright 2004-7 by Raw Material Software ltd.

  ------------------------------------------------------------------------------

   JUCE can be redistributed and/or modified under the terms of the
   GNU General Public License, as published by the Free Software Foundation;
   either version 2 of the License, or (at your option) any later version.

   JUCE is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with JUCE; if not, visit www.gnu.org/licenses or write to the
   Free Software Foundation, Inc., 59 Temple Place, Suite 330,
   Boston, MA 02111-1307 USA

  ------------------------------------------------------------------------------

   If you'd like to release a closed-source product which uses JUCE, commercial
   licenses are also available: visit www.rawmaterialsoftware.com/juce for
   more information.

  ==============================================================================
*/

#include "../../../src/juce_core/basics/juce_StandardHeader.h"
#include <CoreAudio/AudioHardware.h>

BEGIN_JUCE_NAMESPACE


#include "../../../src/juce_appframework/audio/devices/juce_AudioIODeviceType.h"
#include "../../../src/juce_appframework/events/juce_Timer.h"
#include "../../../src/juce_core/threads/juce_ScopedLock.h"
#include "../../../src/juce_core/threads/juce_Thread.h"
#include "../../../src/juce_core/text/juce_LocalisedStrings.h"
#include "../../../src/juce_appframework/gui/components/buttons/juce_TextButton.h"
#include "../../../src/juce_appframework/gui/components/lookandfeel/juce_LookAndFeel.h"
#include "../../../src/juce_appframework/gui/components/controls/juce_ComboBox.h"
#include "../../../src/juce_appframework/gui/components/special/juce_AudioDeviceSelectorComponent.h"
#include "../../../src/juce_appframework/gui/components/windows/juce_AlertWindow.h"


//==============================================================================
#ifndef JUCE_COREAUDIO_ERROR_LOGGING_ENABLED
  #define JUCE_COREAUDIO_ERROR_LOGGING_ENABLED 1
#endif

//==============================================================================
#undef log
#if JUCE_COREAUDIO_LOGGING_ENABLED
  #define log(a) Logger::writeToLog (a)
#else
  #define log(a)
#endif

#undef OK
#if JUCE_COREAUDIO_ERROR_LOGGING_ENABLED
  static bool logAnyErrors_CoreAudio (const OSStatus err, const int lineNum)
  {
      if (err == noErr)
          return true;

      Logger::writeToLog (T("CoreAudio error: ") + String (lineNum) + T(" - ") + String::toHexString ((int)err));
      jassertfalse
      return false;
  }

  #define OK(a) logAnyErrors_CoreAudio (a, __LINE__)
#else
  #define OK(a) (a == noErr)
#endif

//==============================================================================
static const int maxNumChans = 96;


//==============================================================================
class CoreAudioInternal  : public Timer
{
public:
    //==============================================================================
    CoreAudioInternal (AudioDeviceID id)
       : deviceID (id),
         started (false),
         audioBuffer (0),
         numInputChans (0),
         numOutputChans (0),
         callbacksAllowed (true),
         numInputChannelInfos (0),
         numOutputChannelInfos (0),
         inputLatency (0),
         outputLatency (0),
         callback (0),
         inputDevice (0),
         isSlaveDevice (false)
    {
        sampleRate = 0;
        bufferSize = 512;

        if (deviceID == 0)
        {
            error = TRANS("can't open device");
        }
        else
        {
            updateDetailsFromDevice();

            AudioDeviceAddPropertyListener (deviceID,
                                            kAudioPropertyWildcardChannel,
                                            kAudioPropertyWildcardSection,
                                            kAudioPropertyWildcardPropertyID,
                                            deviceListenerProc, this);
        }
    }

    ~CoreAudioInternal()
    {
        AudioDeviceRemovePropertyListener (deviceID,
                                           kAudioPropertyWildcardChannel,
                                           kAudioPropertyWildcardSection,
                                           kAudioPropertyWildcardPropertyID,
                                           deviceListenerProc);

        stop (false);

        juce_free (audioBuffer);
        delete inputDevice;
    }

    void setTempBufferSize (const int numChannels, const int numSamples)
    {
        juce_free (audioBuffer);

        audioBuffer = (float*) juce_calloc (32 + numChannels * numSamples * sizeof (float));

        zeromem (tempInputBuffers, sizeof (tempInputBuffers));
        zeromem (tempOutputBuffers, sizeof (tempOutputBuffers));

        int count = 0;
        int i;
        for (i = 0; i < numInputChans; ++i)
            tempInputBuffers[i] = audioBuffer + count++ * numSamples;

        for (i = 0; i < numOutputChans; ++i)
            tempOutputBuffers[i] = audioBuffer + count++ * numSamples;
    }

    // returns the number of actual available channels
    void fillInChannelInfo (bool input)
    {
        int chanNum = 0, activeChans = 0;
        UInt32 size;

        if (OK (AudioDeviceGetPropertyInfo (deviceID, 0, input, kAudioDevicePropertyStreamConfiguration, &size, 0)))
        {
            AudioBufferList* const bufList = (AudioBufferList*) juce_calloc (size);

            if (OK (AudioDeviceGetProperty (deviceID, 0, input, kAudioDevicePropertyStreamConfiguration, &size, bufList)))
            {
                const int numStreams = bufList->mNumberBuffers;

                for (int i = 0; i < numStreams; ++i)
                {
                    const AudioBuffer& b = bufList->mBuffers[i];

                    for (unsigned int j = 0; j < b.mNumberChannels; ++j)
                    {
                        if (input)
                        {
                            if (activeInputChans[chanNum])
                            {
                                inputChannelInfo [activeChans].sourceChannelNum = chanNum;
                                inputChannelInfo [activeChans].streamNum = i;
                                inputChannelInfo [activeChans].dataOffsetSamples = j;
                                inputChannelInfo [activeChans].dataStrideSamples = b.mNumberChannels;
                                ++activeChans;
                                numInputChannelInfos = activeChans;
                            }

                            inChanNames.add (T("input ") + String (chanNum + 1));
                        }
                        else
                        {
                            if (activeOutputChans[chanNum])
                            {
                                outputChannelInfo [activeChans].sourceChannelNum = chanNum;
                                outputChannelInfo [activeChans].streamNum = i;
                                outputChannelInfo [activeChans].dataOffsetSamples = j;
                                outputChannelInfo [activeChans].dataStrideSamples = b.mNumberChannels;
                                ++activeChans;
                                numOutputChannelInfos = activeChans;
                            }

                            outChanNames.add (T("output ") + String (chanNum + 1));
                        }

                        ++chanNum;
                    }
                }
            }

            juce_free (bufList);
        }
    }

    void updateDetailsFromDevice()
    {
        stopTimer();

        if (deviceID == 0)
            return;

        const ScopedLock sl (callbackLock);

        Float64 sr;
        UInt32 size = sizeof (Float64);
        if (OK (AudioDeviceGetProperty (deviceID, 0, false, kAudioDevicePropertyNominalSampleRate, &size, &sr)))
            sampleRate = sr;

        UInt32 framesPerBuf;
        size = sizeof (framesPerBuf);

        if (OK (AudioDeviceGetProperty (deviceID, 0, false, kAudioDevicePropertyBufferFrameSize, &size, &framesPerBuf)))
        {
            bufferSize = framesPerBuf;

            if (bufferSize > 0)
                setTempBufferSize (numInputChans + numOutputChans, bufferSize);
        }

        bufferSizes.clear();

        if (OK (AudioDeviceGetPropertyInfo (deviceID, 0, false, kAudioDevicePropertyBufferFrameSizeRange, &size, 0)))
        {
            AudioValueRange* ranges = (AudioValueRange*) juce_calloc (size);

            if (OK (AudioDeviceGetProperty (deviceID, 0, false, kAudioDevicePropertyBufferFrameSizeRange, &size, ranges)))
            {
                bufferSizes.add ((int) ranges[0].mMinimum);

                for (int i = 32; i < 8192; i += 32)
                {
                    for (int j = size / sizeof (AudioValueRange); --j >= 0;)
                    {
                        if (i >= ranges[j].mMinimum && i <= ranges[j].mMaximum)
                        {
                            bufferSizes.addIfNotAlreadyThere (i);
                            break;
                        }
                    }
                }

                if (bufferSize > 0)
                    bufferSizes.addIfNotAlreadyThere (bufferSize);
            }

            juce_free (ranges);
        }

        if (bufferSizes.size() == 0 && bufferSize > 0)
            bufferSizes.add (bufferSize);

        sampleRates.clear();
        const double possibleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
        String rates;

        if (OK (AudioDeviceGetPropertyInfo (deviceID, 0, false, kAudioDevicePropertyAvailableNominalSampleRates, &size, 0)))
        {
            AudioValueRange* ranges = (AudioValueRange*) juce_calloc (size);

            if (OK (AudioDeviceGetProperty (deviceID, 0, false, kAudioDevicePropertyAvailableNominalSampleRates, &size, ranges)))
            {
                for (int i = 0; i < numElementsInArray (possibleRates); ++i)
                {
                    bool ok = false;

                    for (int j = size / sizeof (AudioValueRange); --j >= 0;)
                        if (possibleRates[i] >= ranges[j].mMinimum - 2 && possibleRates[i] <= ranges[j].mMaximum + 2)
                            ok = true;

                    if (ok)
                    {
                        sampleRates.add (possibleRates[i]);
                        rates << possibleRates[i] << T(" ");
                    }
                }
            }

            juce_free (ranges);
        }

        if (sampleRates.size() == 0 && sampleRate > 0)
        {
            sampleRates.add (sampleRate);
            rates << sampleRate;
        }

        log (T("sr: ") + rates);

        inputLatency = 0;
        outputLatency = 0;
        UInt32 lat;
        size = sizeof (UInt32);
        if (AudioDeviceGetProperty (deviceID, 0, true, kAudioDevicePropertyLatency, &size, &lat) == noErr)
            inputLatency = (int) lat;

        if (AudioDeviceGetProperty (deviceID, 0, false, kAudioDevicePropertyLatency, &size, &lat) == noErr)
            outputLatency = (int) lat;

        log (T("lat: ") + String (inputLatency) + T(" ") + String (outputLatency));

        inChanNames.clear();
        outChanNames.clear();

        zeromem (inputChannelInfo, sizeof (inputChannelInfo));
        zeromem (outputChannelInfo, sizeof (outputChannelInfo));

        fillInChannelInfo (true);
        fillInChannelInfo (false);
    }

    //==============================================================================
    const StringArray getSources (bool input)
    {
        StringArray s;
        int num = 0;
        OSType* types = getAllDataSourcesForDevice (deviceID, input, num);

        if (types != 0)
        {
            for (int i = 0; i < num; ++i)
            {
                AudioValueTranslation avt;
                char buffer[256];

                avt.mInputData = (void*) &(types[i]);
                avt.mInputDataSize = sizeof (UInt32);
                avt.mOutputData = buffer;
                avt.mOutputDataSize = 256;

                UInt32 transSize = sizeof (avt);
                if (OK (AudioDeviceGetProperty (deviceID, 0, input, kAudioDevicePropertyDataSourceNameForID, &transSize, &avt)))
                {
                    DBG (buffer);
                    s.add (buffer);
                }
            }

            juce_free (types);
        }

        return s;
    }

    int getCurrentSourceIndex (bool input) const
    {
        OSType currentSourceID = 0;
        UInt32 size = 0;
        int result = -1;

        if (deviceID != 0
             && OK (AudioDeviceGetPropertyInfo (deviceID, 0, input, kAudioDevicePropertyDataSource, &size, 0)))
        {
            if (OK (AudioDeviceGetProperty (deviceID, 0, input, kAudioDevicePropertyDataSource, &size, &currentSourceID)))
            {
                int num = 0;
                OSType* const types = getAllDataSourcesForDevice (deviceID, input, num);

                if (types != 0)
                {
                    for (int i = 0; i < num; ++i)
                    {
                        if (types[num] == currentSourceID)
                        {
                            result = i;
                            break;
                        }
                    }

                    juce_free (types);
                }
            }
        }

        return result;
    }

    void setCurrentSourceIndex (int index, bool input)
    {
        if (deviceID != 0)
        {
            int num = 0;
            OSType* types = getAllDataSourcesForDevice (deviceID, input, num);

            if (types != 0)
            {
                if (((unsigned int) index) < num)
                {
                    OSType typeId = types[index];
                    AudioDeviceSetProperty (deviceID, 0, 0, input, kAudioDevicePropertyDataSource, sizeof (typeId), &typeId);
                }

                juce_free (types);
            }
        }
    }

    //==============================================================================
    const String reopen (const BitArray& inputChannels,
                         const BitArray& outputChannels,
                         double newSampleRate,
                         int bufferSizeSamples)
    {
        error = String::empty;
        log ("CoreAudio reopen");
        callbacksAllowed = false;
        stopTimer();

        stop (false);

        activeInputChans = inputChannels;
        activeOutputChans = outputChannels;

        activeInputChans.setRange (inChanNames.size(),
                                   activeInputChans.getHighestBit() + 1 - inChanNames.size(),
                                   false);

        activeOutputChans.setRange (outChanNames.size(),
                                    activeOutputChans.getHighestBit() + 1 - outChanNames.size(),
                                    false);

        numInputChans = activeInputChans.countNumberOfSetBits();
        numOutputChans = activeOutputChans.countNumberOfSetBits();

        // set sample rate
        Float64 sr = newSampleRate;
        UInt32 size = sizeof (sr);
        OK (AudioDeviceSetProperty (deviceID, 0, 0, false, kAudioDevicePropertyNominalSampleRate, size, &sr));
        OK (AudioDeviceSetProperty (deviceID, 0, 0, true, kAudioDevicePropertyNominalSampleRate, size, &sr));

        // change buffer size
        UInt32 framesPerBuf = bufferSizeSamples;
        size = sizeof (framesPerBuf);

        OK (AudioDeviceSetProperty (deviceID, 0, 0, false, kAudioDevicePropertyBufferFrameSize, size, &framesPerBuf));
        OK (AudioDeviceSetProperty (deviceID, 0, 0, true, kAudioDevicePropertyBufferFrameSize, size, &framesPerBuf));

        // wait for the changes to happen (on some devices)
        int i = 30;
        while (--i >= 0)
        {
            updateDetailsFromDevice();

            if (sampleRate == newSampleRate && bufferSizeSamples == bufferSize)
                break;

            Thread::sleep (100);
        }

        if (i < 0)
            error = "Couldn't change sample rate/buffer size";

        if (sampleRates.size() == 0)
            error = "Device has no available sample-rates";

        if (bufferSizes.size() == 0)
            error = "Device has no available buffer-sizes";

        if (inputDevice != 0 && error.isEmpty())
            error = inputDevice->reopen (inputChannels,
                                         outputChannels,
                                         newSampleRate,
                                         bufferSizeSamples);

        callbacksAllowed = true;

        return error;
    }

    bool start (AudioIODeviceCallback* cb)
    {
        if (! started)
        {
            callback = 0;

            if (deviceID != 0)
            {
                if (OK (AudioDeviceAddIOProc (deviceID, audioIOProc, (void*) this)))
                {
                    if (OK (AudioDeviceStart (deviceID, audioIOProc)))
                    {
                        started = true;
                    }
                    else
                    {
                        OK (AudioDeviceRemoveIOProc (deviceID, audioIOProc));
                    }
                }
            }
        }

        if (started)
        {
            const ScopedLock sl (callbackLock);
            callback = cb;
        }

        if (inputDevice != 0)
            return started && inputDevice->start (cb);
        else
            return started;
    }

    void stop (bool leaveInterruptRunning)
    {
        callbackLock.enter();
        callback = 0;
        callbackLock.exit();

        if (started
             && (deviceID != 0)
             && ! leaveInterruptRunning)
        {
            OK (AudioDeviceStop (deviceID, audioIOProc));
            OK (AudioDeviceRemoveIOProc (deviceID, audioIOProc));
            started = false;

            callbackLock.enter();
            callbackLock.exit();

            // wait until it's definately stopped calling back..
            for (int i = 40; --i >= 0;)
            {
                Thread::sleep (50);

                UInt32 running = 0;
                UInt32 size = sizeof (running);
                OK (AudioDeviceGetProperty (deviceID, 0, false, kAudioDevicePropertyDeviceIsRunning, &size, &running));
                if (running == 0)
                    break;
            }

            callbackLock.enter();
            callbackLock.exit();
        }

        if (inputDevice != 0)
            inputDevice->stop (leaveInterruptRunning);
    }

    double getSampleRate() const
    {
        return sampleRate;
    }

    int getBufferSize() const
    {
        return bufferSize;
    }

    void audioCallback (const AudioBufferList* inInputData,
                        AudioBufferList* outOutputData)
    {
        int i;
        const ScopedLock sl (callbackLock);

        if (callback != 0)
        {
            if (inputDevice == 0)
            {
                for (i = numInputChans; --i >= 0;)
                {
                    const CallbackDetailsForChannel& info = inputChannelInfo[i];
                    float* dest = tempInputBuffers [info.sourceChannelNum];
                    const float* src = ((const float*) inInputData->mBuffers[info.streamNum].mData)
                                        + info.dataOffsetSamples;
                    const int stride = info.dataStrideSamples;

                    if (stride != 0) // if this is zero, info is invalid
                    {
                        for (int j = bufferSize; --j >= 0;)
                        {
                            *dest++ = *src;
                            src += stride;
                        }
                    }
                }
            }

            if (! isSlaveDevice)
            {
                if (inputDevice == 0)
                {
                    callback->audioDeviceIOCallback ((const float**) tempInputBuffers,
                                                     numInputChans,
                                                     tempOutputBuffers,
                                                     numOutputChans,
                                                     bufferSize);
                }
                else
                {
                    jassert (inputDevice->bufferSize == bufferSize);

                    callback->audioDeviceIOCallback ((const float**) inputDevice->tempInputBuffers,
                                                     inputDevice->numInputChans,
                                                     tempOutputBuffers,
                                                     numOutputChans,
                                                     bufferSize);
                }

                for (i = numOutputChans; --i >= 0;)
                {
                    const CallbackDetailsForChannel& info = outputChannelInfo[i];
                    const float* src = tempOutputBuffers [i];
                    float* dest = ((float*) outOutputData->mBuffers[info.streamNum].mData)
                                    + info.dataOffsetSamples;
                    const int stride = info.dataStrideSamples;

                    if (stride != 0) // if this is zero, info is invalid
                    {
                        for (int j = bufferSize; --j >= 0;)
                        {
                            *dest = *src++;
                            dest += stride;
                        }
                    }
                }
            }
        }
        else
        {
            for (i = jmin (numOutputChans, numOutputChannelInfos); --i >= 0;)
            {
                const CallbackDetailsForChannel& info = outputChannelInfo[i];
                float* dest = ((float*) outOutputData->mBuffers[info.streamNum].mData)
                                + info.dataOffsetSamples;
                const int stride = info.dataStrideSamples;

                if (stride != 0) // if this is zero, info is invalid
                {
                    for (int j = bufferSize; --j >= 0;)
                    {
                        *dest = 0.0f;
                        dest += stride;
                    }
                }
            }
        }
    }

    // called by callbacks
    void deviceDetailsChanged()
    {
        if (callbacksAllowed)
            startTimer (100);
    }

    void timerCallback()
    {
        stopTimer();
        log ("CoreAudio device changed callback");

        const double oldSampleRate = sampleRate;
        const int oldBufferSize = bufferSize;
        updateDetailsFromDevice();

        if (oldBufferSize != bufferSize || oldSampleRate != sampleRate)
        {
            callbacksAllowed = false;
            stop (false);
            updateDetailsFromDevice();
            callbacksAllowed = true;
        }
    }

    CoreAudioInternal* getRelatedDevice() const
    {
        UInt32 size = 0;
        CoreAudioInternal* result = 0;

        if (deviceID != 0
             && AudioDeviceGetPropertyInfo (deviceID, 0, false, kAudioDevicePropertyRelatedDevices, &size, 0) == noErr
             && size > 0)
        {
            AudioDeviceID* devs = (AudioDeviceID*) juce_calloc (size);

            if (OK (AudioDeviceGetProperty (deviceID, 0, false, kAudioDevicePropertyRelatedDevices, &size, devs)))
            {
                for (unsigned int i = 0; i < size / sizeof (AudioDeviceID); ++i)
                {
                    if (devs[i] != deviceID && devs[i] != 0)
                    {
                        result = new CoreAudioInternal (devs[i]);

                        if (result->error.isEmpty())
                        {
                            const bool thisIsInput = inChanNames.size() > 0 && outChanNames.size() == 0;
                            const bool otherIsInput = result->inChanNames.size() > 0 && result->outChanNames.size() == 0;

                            if (thisIsInput != otherIsInput
                                 || (inChanNames.size() + outChanNames.size() == 0)
                                 || (result->inChanNames.size() + result->outChanNames.size()) == 0)
                                break;
                        }

                        deleteAndZero (result);
                    }
                }
            }

            juce_free (devs);
        }

        return result;
    }

    //==============================================================================
    juce_UseDebuggingNewOperator

    String error;
    int inputLatency, outputLatency;
    BitArray activeInputChans, activeOutputChans;
    StringArray inChanNames, outChanNames;
    Array <double> sampleRates;
    Array <int> bufferSizes;
    AudioIODeviceCallback* callback;

    CoreAudioInternal* inputDevice;
    bool isSlaveDevice;

private:
    CriticalSection callbackLock;
    AudioDeviceID deviceID;
    bool started;
    double sampleRate;
    int bufferSize;
    float* audioBuffer;
    int numInputChans, numOutputChans;
    bool callbacksAllowed;

    struct CallbackDetailsForChannel
    {
        int sourceChannelNum;
        int streamNum;
        int dataOffsetSamples;
        int dataStrideSamples;
    };

    int numInputChannelInfos, numOutputChannelInfos;
    CallbackDetailsForChannel inputChannelInfo [maxNumChans];
    CallbackDetailsForChannel outputChannelInfo [maxNumChans];
    float* tempInputBuffers [maxNumChans];
    float* tempOutputBuffers [maxNumChans];

    CoreAudioInternal (const CoreAudioInternal&);
    const CoreAudioInternal& operator= (const CoreAudioInternal&);

    //==============================================================================
    static OSStatus audioIOProc (AudioDeviceID inDevice,
                                 const AudioTimeStamp* inNow,
                                 const AudioBufferList* inInputData,
                                 const AudioTimeStamp* inInputTime,
                                 AudioBufferList* outOutputData,
                                 const AudioTimeStamp* inOutputTime,
                                 void* device)
    {
        ((CoreAudioInternal*) device)->audioCallback (inInputData, outOutputData);
        return noErr;
    }

    static OSStatus deviceListenerProc (AudioDeviceID inDevice,
                                        UInt32 inLine,
                                        Boolean isInput,
                                        AudioDevicePropertyID inPropertyID,
                                        void* inClientData)
    {
        CoreAudioInternal* const intern = (CoreAudioInternal*) inClientData;

        switch (inPropertyID)
        {
            case kAudioDevicePropertyBufferSize:
            case kAudioDevicePropertyBufferFrameSize:
            case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyStreamFormat:
            case kAudioDevicePropertyDeviceIsAlive:
                intern->deviceDetailsChanged();
                break;

            case kAudioDevicePropertyBufferSizeRange:
            case kAudioDevicePropertyVolumeScalar:
            case kAudioDevicePropertyMute:
            case kAudioDevicePropertyPlayThru:
            case kAudioDevicePropertyDataSource:
            case kAudioDevicePropertyDeviceIsRunning:
                break;
        }

        return noErr;
    }

    //==============================================================================
    static OSType* getAllDataSourcesForDevice (AudioDeviceID deviceID, const bool input, int& num)
    {
        OSType* types = 0;
        UInt32 size = 0;
        num = 0;

        if (deviceID != 0
             && OK (AudioDeviceGetPropertyInfo (deviceID, 0, input, kAudioDevicePropertyDataSources, &size, 0)))
        {
            types = (OSType*) juce_calloc (size);

            if (OK (AudioDeviceGetProperty (deviceID, 0, input, kAudioDevicePropertyDataSources, &size, types)))
            {
                num = size / sizeof (OSType);
            }
            else
            {
                juce_free (types);
                types = 0;
            }
        }

        return types;
    }
};


//==============================================================================
class CoreAudioIODevice   : public AudioIODevice
{
public:
    CoreAudioIODevice (const String& deviceName,
                       AudioDeviceID inputDeviceId,
                       const int inputIndex_,
                       AudioDeviceID outputDeviceId,
                       const int outputIndex_)
        : AudioIODevice (deviceName, "CoreAudio"),
          inputIndex (inputIndex_),
          outputIndex (outputIndex_),
          isOpen_ (false),
          isStarted (false)
    {
        internal = 0;
        CoreAudioInternal* device = 0;

        if (outputDeviceId == 0 || outputDeviceId == inputDeviceId)
        {
            jassert (inputDeviceId != 0);

            device = new CoreAudioInternal (inputDeviceId);
            lastError = device->error;

            if (lastError.isNotEmpty())
                deleteAndZero (device);
        }
        else
        {
            device = new CoreAudioInternal (outputDeviceId);
            lastError = device->error;

            if (lastError.isNotEmpty())
            {
                deleteAndZero (device);
            }
            else if (inputDeviceId != 0)
            {
                CoreAudioInternal* secondDevice = new CoreAudioInternal (inputDeviceId);
                lastError = device->error;

                if (lastError.isNotEmpty())
                {
                    delete secondDevice;
                }
                else
                {
                    device->inputDevice = secondDevice;
                    secondDevice->isSlaveDevice = true;
                }
            }
        }

        internal = device;

        AudioHardwareAddPropertyListener (kAudioPropertyWildcardPropertyID,
                                          hardwareListenerProc, internal);
    }

    ~CoreAudioIODevice()
    {
        AudioHardwareRemovePropertyListener (kAudioPropertyWildcardPropertyID,
                                             hardwareListenerProc);

        delete internal;
    }

    const StringArray getOutputChannelNames()
    {
        return internal->outChanNames;
    }

    const StringArray getInputChannelNames()
    {
        if (internal->inputDevice != 0)
            return internal->inputDevice->inChanNames;
        else
            return internal->inChanNames;
    }

    int getNumSampleRates()
    {
        return internal->sampleRates.size();
    }

    double getSampleRate (int index)
    {
        return internal->sampleRates [index];
    }

    int getNumBufferSizesAvailable()
    {
        return internal->bufferSizes.size();
    }

    int getBufferSizeSamples (int index)
    {
        return internal->bufferSizes [index];
    }

    int getDefaultBufferSize()
    {
        for (int i = 0; i < getNumBufferSizesAvailable(); ++i)
            if (getBufferSizeSamples(i) >= 512)
                return getBufferSizeSamples(i);

        return 512;
    }

    const String open (const BitArray& inputChannels,
                       const BitArray& outputChannels,
                       double sampleRate,
                       int bufferSizeSamples)
    {
        isOpen_ = true;

        if (bufferSizeSamples <= 0)
            bufferSizeSamples = getDefaultBufferSize();

        internal->reopen (inputChannels, outputChannels, sampleRate, bufferSizeSamples);
        lastError = internal->error;
        return lastError;
    }

    void close()
    {
        isOpen_ = false;
    }

    bool isOpen()
    {
        return isOpen_;
    }

    int getCurrentBufferSizeSamples()
    {
        return internal != 0 ? internal->getBufferSize() : 512;
    }

    double getCurrentSampleRate()
    {
        return internal != 0 ? internal->getSampleRate() : 0;
    }

    int getCurrentBitDepth()
    {
        return 32;  // no way to find out, so just assume it's high..
    }

    const BitArray getActiveOutputChannels() const
    {
        return internal != 0 ? internal->activeOutputChans : BitArray();
    }

    const BitArray getActiveInputChannels() const
    {
        BitArray chans;

        if (internal != 0)
        {
            chans = internal->activeInputChans;

            if (internal->inputDevice != 0)
                chans.orWith (internal->inputDevice->activeInputChans);
        }

        return chans;
    }

    int getOutputLatencyInSamples()
    {
        if (internal == 0)
            return 0;

        // this seems like a good guess at getting the latency right - comparing
        // this with a round-trip measurement, it gets it to within a few millisecs
        // for the built-in mac soundcard
        return internal->outputLatency + internal->getBufferSize() * 2;
    }

    int getInputLatencyInSamples()
    {
        if (internal == 0)
            return 0;

        return internal->inputLatency + internal->getBufferSize() * 2;
    }

    void start (AudioIODeviceCallback* callback)
    {
        if (internal != 0 && ! isStarted)
        {
            if (callback != 0)
                callback->audioDeviceAboutToStart (this);

            isStarted = true;
            internal->start (callback);
        }
    }

    void stop()
    {
        if (isStarted && internal != 0)
        {
            AudioIODeviceCallback* const lastCallback = internal->callback;

            isStarted = false;
            internal->stop (true);

            if (lastCallback != 0)
                lastCallback->audioDeviceStopped();
        }
    }

    bool isPlaying()
    {
        if (internal->callback == 0)
            isStarted = false;

        return isStarted;
    }

    const String getLastError()
    {
        return lastError;
    }

    int inputIndex, outputIndex;

    juce_UseDebuggingNewOperator

private:
    CoreAudioInternal* internal;
    bool isOpen_, isStarted;
    String lastError;

    static OSStatus hardwareListenerProc (AudioHardwarePropertyID inPropertyID, void* inClientData)
    {
        CoreAudioInternal* const intern = (CoreAudioInternal*) inClientData;

        switch (inPropertyID)
        {
            case kAudioHardwarePropertyDevices:
                intern->deviceDetailsChanged();
                break;

            case kAudioHardwarePropertyDefaultOutputDevice:
            case kAudioHardwarePropertyDefaultInputDevice:
            case kAudioHardwarePropertyDefaultSystemOutputDevice:
                break;
        }

        return noErr;
    }

    CoreAudioIODevice (const CoreAudioIODevice&);
    const CoreAudioIODevice& operator= (const CoreAudioIODevice&);
};

//==============================================================================
class CoreAudioDevicePanel   : public Component,
                               public ComboBoxListener,
                               public ChangeListener,
                               public ButtonListener
{
public:
    CoreAudioDevicePanel (AudioIODeviceType* type_, 
                       AudioIODeviceType::DeviceSetupDetails& setup_)
        : type (type_),
          setup (setup_)
    {
        sampleRateDropDown = 0;
        sampleRateLabel = 0;
        bufferSizeDropDown = 0;
        bufferSizeLabel = 0;
        outputDeviceDropDown = 0;
        outputDeviceLabel = 0;
        inputDeviceDropDown = 0;
        inputDeviceLabel = 0;
        testButton = 0;
        inputLevelMeter = 0;

        type->scanForDevices();

        if (setup.maxNumOutputChannels > 0)
        {
            outputDeviceDropDown = new ComboBox (String::empty);
            addNamesToDeviceBox (*outputDeviceDropDown, false);
            outputDeviceDropDown->addListener (this);
            addAndMakeVisible (outputDeviceDropDown);

            outputDeviceLabel = new Label (String::empty, TRANS ("output:"));
            outputDeviceLabel->attachToComponent (outputDeviceDropDown, true);

            addAndMakeVisible (testButton = new TextButton (TRANS ("Test")));
            testButton->addButtonListener (this);
        }

        if (setup.maxNumInputChannels > 0)
        {
            inputDeviceDropDown = new ComboBox (String::empty);
            addNamesToDeviceBox (*inputDeviceDropDown, true);
            inputDeviceDropDown->addListener (this);
            addAndMakeVisible (inputDeviceDropDown);

            inputDeviceLabel = new Label (String::empty, TRANS ("input:"));
            inputDeviceLabel->attachToComponent (inputDeviceDropDown, true);

            addAndMakeVisible (inputLevelMeter = AudioDeviceSelectorComponent::createSimpleLevelMeterComponent (setup_.manager));
        }

        setup.manager->addChangeListener (this);
        changeListenerCallback (0);
    }

    ~CoreAudioDevicePanel()
    {
        setup.manager->removeChangeListener (this);

        deleteAndZero (outputDeviceLabel);
        deleteAndZero (inputDeviceLabel);
        deleteAndZero (sampleRateLabel);
        deleteAndZero (bufferSizeLabel);
        deleteAllChildren();
    }

    void resized()
    {
        const int lx = proportionOfWidth (0.35f);
        const int w = proportionOfWidth (0.5f);
        const int h = 24;
        const int space = 6;
        const int dh = h + space;
        int y = 0;

        if (outputDeviceDropDown != 0)
        {
            outputDeviceDropDown->setBounds (lx, y, w, h);
            testButton->setBounds (outputDeviceDropDown->getRight() + 8, 
                                   outputDeviceDropDown->getY(),
                                   getWidth() - outputDeviceDropDown->getRight() - 10,
                                   h);
            y += dh;
        }

        if (inputDeviceDropDown != 0)
        {
            inputDeviceDropDown->setBounds (lx, y, w, h);

            inputLevelMeter->setBounds (inputDeviceDropDown->getRight() + 8, 
                                        inputDeviceDropDown->getY(),
                                        getWidth() - inputDeviceDropDown->getRight() - 10,
                                        h);
            y += dh;
        }

        y += space * 2;

        if (sampleRateDropDown != 0)
        {
            sampleRateDropDown->setBounds (lx, y, w, h);
            y += dh;
        }

        if (bufferSizeDropDown != 0)
        {
            bufferSizeDropDown->setBounds (lx, y, w, h);
            y += dh;
        }
    }

    void comboBoxChanged (ComboBox* comboBoxThatHasChanged)
    {
        if (comboBoxThatHasChanged == 0)
            return;

        AudioDeviceManager::AudioDeviceSetup config;
        setup.manager->getAudioDeviceSetup (config);
        String error;

        if (comboBoxThatHasChanged == outputDeviceDropDown
              || comboBoxThatHasChanged == inputDeviceDropDown)
        {
            config.outputDeviceName = outputDeviceDropDown->getSelectedId() < 0 ? String::empty 
                                                                                : outputDeviceDropDown->getText();
            config.inputDeviceName = inputDeviceDropDown->getSelectedId() < 0 ? String::empty 
                                                                              : inputDeviceDropDown->getText();

            if (comboBoxThatHasChanged == inputDeviceDropDown)
                config.useDefaultInputChannels = true;
            else
                config.useDefaultOutputChannels = true;

            error = setup.manager->setAudioDeviceSetup (config, true);

            showCorrectDeviceName (inputDeviceDropDown, true);
            showCorrectDeviceName (outputDeviceDropDown, false);
        }
        else if (comboBoxThatHasChanged == sampleRateDropDown)
        {
            if (sampleRateDropDown->getSelectedId() > 0)
            {
                config.sampleRate = sampleRateDropDown->getSelectedId();
                error = setup.manager->setAudioDeviceSetup (config, true);
            }
        }
        else if (comboBoxThatHasChanged == bufferSizeDropDown)
        {
            if (bufferSizeDropDown->getSelectedId() > 0)
            {
                config.bufferSize = bufferSizeDropDown->getSelectedId();
                error = setup.manager->setAudioDeviceSetup (config, true);
            }
        }

        if (error.isNotEmpty())
        {
            AlertWindow::showMessageBox (AlertWindow::WarningIcon,
                                         T("Error when trying to open audio device!"),
                                         error);
        }
    }

    void buttonClicked (Button*)
    {
        setup.manager->playTestSound();
    }

    void changeListenerCallback (void*)
    {
        AudioIODevice* const currentDevice = setup.manager->getCurrentAudioDevice();

        if (currentDevice != 0)
        {
            showCorrectDeviceName (inputDeviceDropDown, true);
            showCorrectDeviceName (outputDeviceDropDown, false);

            // sample rate..
            {
                if (sampleRateDropDown == 0)
                {
                    addAndMakeVisible (sampleRateDropDown = new ComboBox (String::empty));
                    sampleRateDropDown->addListener (this);

                    delete sampleRateLabel;
                    sampleRateLabel = new Label (String::empty, TRANS ("sample rate:"));
                    sampleRateLabel->attachToComponent (sampleRateDropDown, true);
                }
                else
                {
                    sampleRateDropDown->clear();
                    sampleRateDropDown->removeListener (this);
                }

                const int numRates = currentDevice->getNumSampleRates();

                for (int i = 0; i < numRates; ++i)
                {
                    const int rate = roundDoubleToInt (currentDevice->getSampleRate (i));
                    sampleRateDropDown->addItem (String (rate) + T(" Hz"), rate);
                }

                sampleRateDropDown->setSelectedId (roundDoubleToInt (currentDevice->getCurrentSampleRate()), true);
                sampleRateDropDown->addListener (this);
            }

            // buffer size
            {
                if (bufferSizeDropDown == 0)
                {
                    addAndMakeVisible (bufferSizeDropDown = new ComboBox (String::empty));
                    bufferSizeDropDown->addListener (this);

                    delete bufferSizeLabel;
                    bufferSizeLabel = new Label (String::empty, TRANS ("audio buffer size:"));
                    bufferSizeLabel->attachToComponent (bufferSizeDropDown, true);
                }
                else
                {
                    bufferSizeDropDown->clear();
                }

                const int numBufferSizes = currentDevice->getNumBufferSizesAvailable();
                double currentRate = currentDevice->getCurrentSampleRate();
                if (currentRate == 0)
                    currentRate = 44100.0;

                for (int i = 0; i < numBufferSizes; ++i)
                {
                    const int bs = currentDevice->getBufferSizeSamples (i);
                    bufferSizeDropDown->addItem (String (bs)
                                                  + T(" samples (")
                                                  + String (bs * 1000.0 / currentRate, 1)
                                                  + T(" ms)"),
                                                 bs);
                }

                bufferSizeDropDown->setSelectedId (currentDevice->getCurrentBufferSizeSamples(), true);
            }
        }
        else
        {
            jassert (setup.manager->getCurrentAudioDevice() == 0); // not the correct device type!

            deleteAndZero (sampleRateLabel);
            deleteAndZero (bufferSizeLabel);
            deleteAndZero (sampleRateDropDown);
            deleteAndZero (bufferSizeDropDown);

            if (outputDeviceDropDown != 0)
                outputDeviceDropDown->setSelectedId (-1, true);

            if (inputDeviceDropDown != 0)
                inputDeviceDropDown->setSelectedId (-1, true);
        }

        resized();
        setSize (getWidth(), getLowestY() + 4);
    }

private:
    AudioIODeviceType* const type;
    const AudioIODeviceType::DeviceSetupDetails setup;

    ComboBox* outputDeviceDropDown;
    ComboBox* inputDeviceDropDown;
    ComboBox* sampleRateDropDown;
    ComboBox* bufferSizeDropDown;
    Label* outputDeviceLabel;
    Label* inputDeviceLabel;
    Label* sampleRateLabel;
    Label* bufferSizeLabel;
    TextButton* testButton;
    Component* inputLevelMeter;

    void showCorrectDeviceName (ComboBox* const box, const bool isInput)
    {
        if (box != 0)
        {
            CoreAudioIODevice* const currentDevice = dynamic_cast <CoreAudioIODevice*> (setup.manager->getCurrentAudioDevice());

            const int index = (currentDevice == 0) ? -1 
                                                   : (isInput ? currentDevice->inputIndex
                                                              : currentDevice->outputIndex);

            if (index >= 0)
                box->setText (type->getDeviceNames (isInput) [index], true);
            else
                box->setSelectedId (-1, true);

            if (! isInput)
                testButton->setEnabled (index >= 0);
        }
    }

    void addNamesToDeviceBox (ComboBox& combo, bool isInputs)
    {
        const StringArray devs (type->getDeviceNames (isInputs));

        for (int i = 0; i < devs.size(); ++i)
            combo.addItem (devs[i], i + 1);

        combo.addItem (TRANS("<< none >>"), -1);
        combo.setSelectedId (-1, true);
    }

    int getLowestY() const
    {
        int y = 0;

        for (int i = getNumChildComponents(); --i >= 0;)
            y = jmax (y, getChildComponent (i)->getBottom());

        return y;
    }

    CoreAudioDevicePanel (const CoreAudioDevicePanel&);
    const CoreAudioDevicePanel& operator= (const CoreAudioDevicePanel&);
};

//==============================================================================
class CoreAudioIODeviceType  : public AudioIODeviceType
{
public:
    //==============================================================================
    CoreAudioIODeviceType()
        : AudioIODeviceType (T("CoreAudio")),
          hasScanned (false)
    {
    }

    ~CoreAudioIODeviceType()
    {
    }

    //==============================================================================
    void scanForDevices()
    {
        hasScanned = true;

        inputDeviceNames.clear();
        outputDeviceNames.clear();
        inputIds.clear();
        outputIds.clear();

        UInt32 size;
        if (OK (AudioHardwareGetPropertyInfo (kAudioHardwarePropertyDevices, &size, 0)))
        {
            AudioDeviceID* const devs = (AudioDeviceID*) juce_calloc (size);

            if (OK (AudioHardwareGetProperty (kAudioHardwarePropertyDevices, &size, devs)))
            {
                static bool alreadyLogged = false;
                const int num = size / sizeof (AudioDeviceID);
                for (int i = 0; i < num; ++i)
                {
                    char name[1024];
                    size = sizeof (name);
                    if (OK (AudioDeviceGetProperty (devs[i], 0, false, kAudioDevicePropertyDeviceName, &size, name)))
                    {
                        const String nameString (String::fromUTF8 ((const uint8*) name, strlen (name)));

                        if (! alreadyLogged)
                            log (T("CoreAudio device: ") + nameString);

                        const int numIns = getNumChannels (devs[i], true);
                        const int numOuts = getNumChannels (devs[i], false);

                        if (numIns > 0)
                        {
                            inputDeviceNames.add (nameString);
                            inputIds.add (devs[i]);
                        }

                        if (numOuts > 0)
                        {
                            outputDeviceNames.add (nameString);
                            outputIds.add (devs[i]);
                        }
                    }
                }

                alreadyLogged = true;
            }

            juce_free (devs);
        }

        inputDeviceNames.appendNumbersToDuplicates (false, true);
        outputDeviceNames.appendNumbersToDuplicates (false, true);
    }

    const StringArray getDeviceNames (const bool wantInputNames) const
    {
        jassert (hasScanned); // need to call scanForDevices() before doing this

        if (wantInputNames)
            return inputDeviceNames;
        else
            return outputDeviceNames;
    }

    int getDefaultDeviceIndex (const bool forInput) const
    {
        jassert (hasScanned); // need to call scanForDevices() before doing this

        AudioDeviceID deviceID;
        UInt32 size = sizeof (deviceID);

        // if they're asking for any input channels at all, use the default input, so we
        // get the built-in mic rather than the built-in output with no inputs..
        if (AudioHardwareGetProperty (forInput ? kAudioHardwarePropertyDefaultInputDevice
                                               : kAudioHardwarePropertyDefaultOutputDevice,
                                      &size, &deviceID) == noErr)
        {
            if (forInput)
            {
                for (int i = inputIds.size(); --i >= 0;)
                    if (inputIds[i] == deviceID)
                        return i;
            }
            else
            {
                for (int i = outputIds.size(); --i >= 0;)
                    if (outputIds[i] == deviceID)
                        return i;
            }
        }

        return 0;
    }

    int getIndexOfDevice (AudioIODevice* device, const bool asInput) const
    {
        jassert (hasScanned); // need to call scanForDevices() before doing this

        CoreAudioIODevice* const d = dynamic_cast <CoreAudioIODevice*> (device);
        if (d == 0)
            return -1;

        return asInput ? d->inputIndex
                       : d->outputIndex;
    }

    bool hasSeparateInputsAndOutputs() const    { return true; }

    AudioIODevice* createDevice (const String& outputDeviceName,
                                 const String& inputDeviceName)
    {
        jassert (hasScanned); // need to call scanForDevices() before doing this

        const int inputIndex = inputDeviceNames.indexOf (inputDeviceName);
        const int outputIndex = outputDeviceNames.indexOf (outputDeviceName);

        String deviceName (outputDeviceName);
        if (deviceName.isEmpty())
            deviceName = inputDeviceName;

        if (index >= 0)
            return new CoreAudioIODevice (deviceName, 
                                          inputIds [inputIndex],
                                          inputIndex,
                                          outputIds [outputIndex],
                                          outputIndex);

        return 0;
    }

    //==============================================================================
    juce_UseDebuggingNewOperator

private:
    StringArray inputDeviceNames, outputDeviceNames;
    Array <AudioDeviceID> inputIds, outputIds;

    bool hasScanned;

    static int getNumChannels (AudioDeviceID deviceID, bool input)
    {
        int total = 0;
        UInt32 size;

        if (OK (AudioDeviceGetPropertyInfo (deviceID, 0, input, kAudioDevicePropertyStreamConfiguration, &size, 0)))
        {
            AudioBufferList* const bufList = (AudioBufferList*) juce_calloc (size);

            if (OK (AudioDeviceGetProperty (deviceID, 0, input, kAudioDevicePropertyStreamConfiguration, &size, bufList)))
            {
                const int numStreams = bufList->mNumberBuffers;

                for (int i = 0; i < numStreams; ++i)
                {
                    const AudioBuffer& b = bufList->mBuffers[i];
                    total += b.mNumberChannels;
                }
            }

            juce_free (bufList);
        }

        return total;
    }

    CoreAudioIODeviceType (const CoreAudioIODeviceType&);
    const CoreAudioIODeviceType& operator= (const CoreAudioIODeviceType&);
};

//==============================================================================
AudioIODeviceType* juce_createDefaultAudioIODeviceType()
{
    return new CoreAudioIODeviceType();
}

#undef log

END_JUCE_NAMESPACE
