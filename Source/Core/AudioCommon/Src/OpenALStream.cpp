// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include <functional>

#include "aldlist.h"
#include "OpenALStream.h"

#if defined HAVE_OPENAL && HAVE_OPENAL

soundtouch::SoundTouch soundTouch;

//
// AyuanX: Spec says OpenAL1.1 is thread safe already
//
bool OpenALStream::Start()
{
	ALDeviceList *pDeviceList = NULL;
	ALCcontext *pContext = NULL;
	ALCdevice *pDevice = NULL;
	bool bReturn = false;

	pDeviceList = new ALDeviceList();
	if ((pDeviceList) && (pDeviceList->GetNumDevices()))
	{
		char *defDevName = pDeviceList-> \
			GetDeviceName(pDeviceList->GetDefaultDevice());
		pDevice = alcOpenDevice(defDevName);
		if (pDevice)
		{
			pContext = alcCreateContext(pDevice, NULL);
			if (pContext)
			{
				alcMakeContextCurrent(pContext);
				thread = std::thread(std::mem_fun(&OpenALStream::SoundLoop), this);
				bReturn = true;
			}
			else
			{
				alcCloseDevice(pDevice);
				PanicAlertT("OpenAL: can't create context "
					"for device %s", defDevName);
			}
		}
		else
		{
			PanicAlertT("OpenAL: can't open device %s", defDevName);
		}
		delete pDeviceList;
	}
	else
	{
		PanicAlertT("OpenAL: can't find sound devices");
	}

	soundTouch.clear();
	return bReturn;
}

void OpenALStream::Stop()
{
	threadData = 1;
	// kick the thread if it's waiting
	soundSyncEvent.Set();

	soundTouch.clear();

	thread.join();

	alSourceStop(uiSource);
	alSourcei(uiSource, AL_BUFFER, 0);

	// Clean up buffers and sources
	alDeleteSources(1, &uiSource);
	uiSource = 0;
	alDeleteBuffers(OAL_NUM_BUFFERS, uiBuffers);

	ALCcontext *pContext = alcGetCurrentContext();
	ALCdevice *pDevice = alcGetContextsDevice(pContext);

	alcMakeContextCurrent(NULL);
	alcDestroyContext(pContext);
	alcCloseDevice(pDevice);
}

void OpenALStream::SetVolume(int volume)
{
	fVolume = (float)volume / 100.0f;

	if (uiSource)
		alSourcef(uiSource, AL_GAIN, fVolume); 
}

void OpenALStream::Update()
{
	soundSyncEvent.Set();
	mainSyncEvent.Wait();
}

void OpenALStream::Clear(bool mute)
{
	m_muted = mute;

	if(m_muted)
	{
		soundTouch.clear();
		alSourceStop(uiSource);
	}
	else
	{
		alSourcePlay(uiSource);
	}
}

void OpenALStream::SoundLoop()
{
	Common::SetCurrentThreadName("Audio thread - openal");

	u32 ulFrequency = m_mixer->GetSampleRate();

	memset(uiBuffers, 0, OAL_NUM_BUFFERS * sizeof(ALuint));
	uiSource = 0;

	// Generate some AL Buffers for streaming
	alGenBuffers(OAL_NUM_BUFFERS, (ALuint *)uiBuffers);
	// Generate a Source to playback the Buffers
	alGenSources(1, &uiSource);

	// Short Silence
	memset(sampleBuffer, 0, OAL_MAX_SAMPLES * 4 * OAL_NUM_BUFFERS);
	memset(realtimeBuffer, 0, OAL_MAX_SAMPLES * 4);
	for (int i = 0; i < OAL_NUM_BUFFERS; i++)
		alBufferData(uiBuffers[i], AL_FORMAT_STEREO16, realtimeBuffer, OAL_MAX_SAMPLES * 4, ulFrequency);
	alSourceQueueBuffers(uiSource, OAL_NUM_BUFFERS, uiBuffers);
	alSourcePlay(uiSource);
	
	// Set the default sound volume as saved in the config file.
	alSourcef(uiSource, AL_GAIN, fVolume); 

	// TODO: Error handling
	//ALenum err = alGetError();

	ALint iBuffersFilled = 0;
	ALint iBuffersProcessed = 0;
	ALuint uiBufferTemp[OAL_NUM_BUFFERS] = {0};

	soundTouch.setChannels(2);
	soundTouch.setSampleRate(ulFrequency);
	soundTouch.setSetting(SETTING_USE_QUICKSEEK, 0);
	soundTouch.setSetting(SETTING_USE_AA_FILTER, 0);
	soundTouch.setSetting(SETTING_SEQUENCE_MS, 1);
	soundTouch.setSetting(SETTING_SEEKWINDOW_MS, 28);
	soundTouch.setSetting(SETTING_OVERLAP_MS, 12);

	while (!threadData) 
	{
		// num_samples_to_render in this update - depends on SystemTimers::AUDIO_DMA_PERIOD.
		const u32 stereo_16_bit_size = 4;
		const u32 dma_length = 32;
		const u64 ais_samples_per_second = 48000 * stereo_16_bit_size;
		u64 audio_dma_period = SystemTimers::GetTicksPerSecond() / (AudioInterface::GetAIDSampleRate() * stereo_16_bit_size / dma_length);
		u64 num_samples_to_render = (audio_dma_period * ais_samples_per_second) / SystemTimers::GetTicksPerSecond();

		unsigned int numSamples = (unsigned int)num_samples_to_render;

		numSamples = (numSamples > OAL_MAX_SAMPLES) ? OAL_MAX_SAMPLES : numSamples;
		numSamples = m_mixer->Mix(realtimeBuffer, numSamples);
		soundTouch.putSamples(realtimeBuffer, numSamples);

		if (iBuffersProcessed == iBuffersFilled)
		{
			alGetSourcei(uiSource, AL_BUFFERS_PROCESSED, &iBuffersProcessed);
			iBuffersFilled = 0;
		}

		if (iBuffersProcessed)
		{
			float rate = m_mixer->GetCurrentSpeed();
			if (rate <= 0)
			{
				Core::RequestRefreshInfo();
				rate = m_mixer->GetCurrentSpeed();
			}
			if (rate > 0)
			{
				// Adjust SETTING_SEQUENCE_MS to balance between lag vs hollow audio
				soundTouch.setSetting(SETTING_SEQUENCE_MS, (int)(1 / (rate * rate)));
				soundTouch.setTempo(rate);
			}
			unsigned int nSamples = soundTouch.receiveSamples(sampleBuffer, OAL_MAX_SAMPLES * 2 * OAL_NUM_BUFFERS);
			if (nSamples > 0)
			{
				// Remove the Buffer from the Queue.  (uiBuffer contains the Buffer ID for the unqueued Buffer)
				if (iBuffersFilled == 0)
					alSourceUnqueueBuffers(uiSource, iBuffersProcessed, uiBufferTemp);
				alBufferData(uiBufferTemp[iBuffersFilled], AL_FORMAT_STEREO16, sampleBuffer, nSamples * 4, ulFrequency);
				alSourceQueueBuffers(uiSource, 1, &uiBufferTemp[iBuffersFilled]);
				iBuffersFilled++;

				if (iBuffersFilled == OAL_NUM_BUFFERS)
					alSourcePlay(uiSource);
			}
		}
		else
		{
			soundSyncEvent.Wait();
		}
		mainSyncEvent.Set();
	}
}

#endif //HAVE_OPENAL

