#include "sound.h"
#include "../log.h"
#include "../Threads/mutex.h"
#include <al.h>
#include <alc.h>
#include <assert.h>
#include <string>
#include <map>

//internal inlines
#include "sound_inl.h"
#include "vocFormat.h"

namespace Sound
{
	//////////////////////////////////////////////////////////////////
	//Structures, enums and defines.
	//////////////////////////////////////////////////////////////////
	enum ActiveSoundFlags
	{
		SOUND_ACTIVE  = 0x08000000,
		SOUND_PLAYING = 0x10000000,
		SOUND_LOOPING = 0x20000000,
		SOUND_PAUSED  = 0x40000000,
		SOUND_UNUSED  = 0x80000000,	//reserved for future expansion.
	};

	enum BufferFlags
	{
		BUFFER_ACTIVE = 1,
	};

	struct SoundBuffer
	{
		//hold the data name associated with the buffered data.
		std::string name;

		u32 flags;			//buffer flags.
		u32 index;			//buffer index
		s32 refCount;		//number of sounds current referencing this buffer.
		u64 lastUsed;		//last "frame" referenced.
		ALuint oalBuffer;	//OpenAL buffer.
	};

	typedef std::map<std::string, u32> BufferMap;

	#define VALIDATE() \
	{  \
		ALenum error = alGetError();\
		if (error != AL_NO_ERROR)	\
		{ \
			LOG( LOG_ERROR, "alError = 0x%x", error ); \
			assert(0); \
		} \
	}

	//////////////////////////////////////////////////////////////////
	//Variables
	//////////////////////////////////////////////////////////////////
	static s32 s_numBuffers		= 256;
	static s32 s_maxSimulSounds	= 32;

	static bool			s_init   = false;
	static ALCdevice*	s_device = NULL;
	static ALCcontext*	s_context = NULL;

	static SoundBuffer*	s_buffers;
	static ALuint*		s_sources;

	static BufferMap    s_bufferMap;

	static u64 s_currentFrame;
	static f32 s_globalVolume;

	static Mutex* s_mutex;
	static XLSoundCallback s_callback = NULL;
	static u32* s_userValue;

	u32* s_activeSounds;

	//////////////////////////////////////////////////////////////////
	//Forward function declarations.
	//////////////////////////////////////////////////////////////////
	bool isActiveNoLock(SoundHandle handle);
	SoundBuffer* getSoundBuffer(const char* name);
	SoundHandle allocateSound(u32 bufferID);
	bool playSoundInternal(SoundHandle sound, f32 volume, f32 pan, Bool loop, Bool is3D);
	const void* getRawSoundData(const void* data, u32 size, u32 type, u32& rawSize);
	
	//////////////////////////////////////////////////////////////////
	//API implementation
	//////////////////////////////////////////////////////////////////
	bool init()
	{
		s_mutex = new Mutex();

		//setup the device.
		s_device = alcOpenDevice(NULL);
		if (!s_device)
		{
			LOG( LOG_ERROR, "Cannot open the audio device, no sound will be available." );
			return false;
		}

		//create the context
		s_context = alcCreateContext(s_device, 0);
		if (!s_context)
		{
			alcCloseDevice(s_device);
			LOG( LOG_ERROR, "Cannot create an audio context, no sound will be available." );
			return false;
		}
		alcMakeContextCurrent(s_context);

		//reset error handling.
		alGetError();

		//allocate buffer pool.
		s_buffers = new SoundBuffer[ s_numBuffers ];
		for (s32 i=0; i<s_numBuffers; i++)
		{
			s_buffers[i].flags    = 0;
			s_buffers[i].index	  = i;
			s_buffers[i].lastUsed = 0;
			s_buffers[i].refCount = 0;
			alGenBuffers(1, &s_buffers[i].oalBuffer);
		}
		if ( alGetError() != AL_NO_ERROR )
		{
			delete[] s_buffers;
			alcMakeContextCurrent(0);
			alcDestroyContext(s_context);
			alcCloseDevice(s_device);

			LOG( LOG_ERROR, "Cannot allocate space for audio buffers, no sound will be available." );
			return false;
		}

		//allocate sources
		s_sources = new ALuint[s_maxSimulSounds];
		alGenSources(s_maxSimulSounds, s_sources);
		if ( alGetError() != AL_NO_ERROR )
		{
			//free allocated memory.
			for (s32 i=0; i<s_numBuffers; i++)
			{
				alDeleteBuffers(1, &s_buffers[i].oalBuffer);
			}
			alcMakeContextCurrent(0);
			alcDestroyContext(s_context);
			alcCloseDevice(s_device);

			delete[] s_buffers;
			delete[] s_sources;

			LOG( LOG_ERROR, "Cannot allocate space for audio sources, no sound will be available." );
			return false;
		}

		s_activeSounds = new u32[ s_maxSimulSounds ];
		memset(s_activeSounds, 0, sizeof(u32)*s_maxSimulSounds);

		s_userValue = new u32[ s_maxSimulSounds ];
		memset(s_userValue, 0, sizeof(u32)*s_maxSimulSounds);

		s_init = true;
		s_currentFrame = 1;
		s_globalVolume = 0.80f;
		LOG( LOG_MESSAGE, "Sound System initialized." );
				
		return true;
	}

	void free()
	{
		reset();

		delete s_mutex;
		if (!s_init) { return; }

		for (s32 i=0; i<s_numBuffers; i++)
		{
			alDeleteBuffers(1, &s_buffers[i].oalBuffer);
		}
		alDeleteSources(s_maxSimulSounds, s_sources);
		alcMakeContextCurrent(0);
		alcDestroyContext(s_context);
		alcCloseDevice(s_device);

		delete[] s_buffers;
		delete[] s_sources;
		delete[] s_activeSounds;
	}

	void reset()
	{
		//first stop all sounds.
		s_mutex->Lock();

		for (s32 s=0; s<s_maxSimulSounds; s++)
		{
			alSourceStop( s_sources[s] );
			alSourcei( s_sources[s], AL_BUFFER, 0 );
			s_activeSounds[s] = 0;
		}

		for (s32 b=0; b<s_numBuffers; b++)
		{
			s_buffers[b].refCount = 0;
			s_buffers[b].flags    = 0;
			s_buffers[b].lastUsed = 0;
		}

		s_mutex->Unlock(); 
	}
		
	void setCallback( XLSoundCallback callback )
	{
		s_callback = callback;
	}

	void update()
	{
		if (!s_init) { return; }
		s_mutex->Lock();

		for (s32 s=0; s<s_maxSimulSounds; s++)
		{
			if ( checkActiveFlag(s, SOUND_PLAYING) )
			{
				s32 state;
				alGetSourcei( s_sources[s], AL_SOURCE_STATE, &state );
				if ( state != AL_PLAYING )
				{
					//was this playing up until now? - fire off the callback...
					if (s_callback && checkActiveFlag(s, SOUND_PLAYING))
					{
						s_callback( s_userValue[s] );

						const u32 bufferID = getActiveBuffer(s);
						s_buffers[bufferID].refCount--;
						assert(s_buffers[bufferID].refCount >= 0);
					}
					clearActiveFlag(s, SOUND_PLAYING);
				}
				if ( state != AL_PAUSED )
				{
					clearActiveFlag(s, SOUND_PAUSED);
				}
			}
		}

		s_currentFrame++;
		s_mutex->Unlock();
	}

	void setGlobalVolume(f32 volume)
	{
		volume *= 0.80f;
		if (volume != s_globalVolume && s_init)
		{
			s_mutex->Lock();

			const f32 scale = s_globalVolume > 0.0f ? volume / s_globalVolume : 1.0f;

			//change the volume of all the currently playing sounds.
			for (s32 s=0; s<s_maxSimulSounds; s++)
			{
				f32 currentVolume;
				alGetSourcef(s, AL_GAIN, &currentVolume);

				const f32 gain = min(currentVolume * scale, 1.0f);
				alSourcef( s, AL_GAIN, gain );
			}
			
			s_globalVolume = volume;

			s_mutex->Unlock();
		}
	}
	
	Bool isActive(SoundHandle handle)
	{
		s_mutex->Lock();
			const bool soundActive = isActiveNoLock(handle);
		s_mutex->Unlock();

		return soundActive;
	}

	Bool isPlaying(SoundHandle handle)
	{
		//debug
		s_mutex->Lock();
			const u32 sourceID = getHandleSource(handle);
			const bool soundIsPlaying = ( isActiveNoLock(handle) && checkActiveFlag(sourceID, SOUND_PLAYING) );
		s_mutex->Unlock();

		return soundIsPlaying;
	}

	Bool isLooping(SoundHandle handle)
	{
		s_mutex->Lock();
			const u32 sourceID = getHandleSource(handle);
			const bool soundIsLooping = ( isActiveNoLock(handle) && checkActiveFlag(sourceID, SOUND_LOOPING) );
		s_mutex->Unlock();
		
		return soundIsLooping;
	}
	
	SoundHandle playSound2D(const char* name, const void* data, u32 size, u32 type, SoundInfo* info, Bool looping)
	{
		if (!s_init) { return INVALID_SOUND_HANDLE; }

		s_mutex->Lock();

		//allocate a free buffer if needed
		SoundBuffer* buffer = getSoundBuffer(name);
		if (!buffer)
		{
			return INVALID_SOUND_HANDLE;
		}

		//allocate a sound
		SoundHandle sound = allocateSound(buffer->index);

		//load the sound buffer (if not already active).
		if ( !(buffer->flags&BUFFER_ACTIVE) )
		{
			ALenum bufferFmt = AL_FORMAT_MONO8;
			if (info->bitRate == 8)
			{
				bufferFmt = (info->stereo) ? AL_FORMAT_STEREO8 : AL_FORMAT_MONO8;
			}
			else if (info->bitRate == 16)
			{
				bufferFmt = (info->stereo) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
			}

			u32 rawSize = 0;
			const void* rawData = getRawSoundData(data, size, type, rawSize);

			u32 samplingRate = info->samplingRate;
			if (type == STYPE_VOC)
			{
				samplingRate = Voc::getSampleRate() * 3;
			}

			alBufferData( buffer->oalBuffer, bufferFmt, rawData, rawSize, samplingRate );
			ALenum error = alGetError();
			if (error != AL_NO_ERROR)
			{
				LOG( LOG_ERROR, "Sound \"%s\" has invalid data.", name );
				s_mutex->Unlock();
				return false;
			}

			buffer->flags |= BUFFER_ACTIVE;
		}

		//finally play the sound.
		bool playResult = playSoundInternal( sound, info->volume, info->pan, looping, false );
		if (!playResult)
		{
			LOG( LOG_ERROR, "Cannot play sound \"%s\"", name );

			clearActiveFlag( getHandleSource(sound), SOUND_ACTIVE );
			clearActiveFlag( getHandleSource(sound), SOUND_PLAYING );
			clearActiveFlag( getHandleSource(sound), SOUND_LOOPING );

			sound = INVALID_SOUND_HANDLE;
		}
		else
		{
			s_userValue[ getHandleSource(sound) ] = info->userValue;
		}

		s_mutex->Unlock();

		return sound;
	}

	Bool playOneShot2D(const char* name, const void* data, u32 size, u32 type, SoundInfo* info)
	{
		SoundHandle sound = playSound2D(name, data, size, type, info, false);
		return (sound != INVALID_SOUND_HANDLE);
	}

	SoundHandle playSoundLooping(const char* name, const void* data, u32 size, u32 type, SoundInfo* info)
	{
		return playSound2D(name, data, size, type, info, true);
	}

	void stopSound(SoundHandle handle)
	{
		s_mutex->Lock();
		
		if (!isActiveNoLock(handle)) { s_mutex->Unlock(); return; }
		const u32 sourceID = getHandleSource(handle);

		//if the sound is not playing then it doesn't need to be stopped.
		if (!checkActiveFlag(sourceID, SOUND_PLAYING))
		{
			s_mutex->Unlock(); 
			return;
		}

		//stop the sound
		alSourceStop( s_sources[sourceID] );
		alSourcei( s_sources[sourceID], AL_BUFFER, 0 );

		//clear the playing flag (and looping).
		clearActiveFlag(sourceID, SOUND_PLAYING);
		clearActiveFlag(sourceID, SOUND_LOOPING);
		clearActiveFlag(sourceID, SOUND_PAUSED);

		const u32 bufferID = getHandleBuffer(handle);
		s_buffers[bufferID].refCount--;
		assert(s_buffers[bufferID].refCount >= 0);

		s_mutex->Unlock(); 
	}

	void stopAllSounds()
	{
		s_mutex->Lock();

		for (s32 s=0; s<s_maxSimulSounds; s++)
		{
			alSourceStop( s_sources[s] );
			alSourcei( s_sources[s], AL_BUFFER, 0 );

			setActiveBuffer(s, 0);
			clearActiveFlag(s, SOUND_PLAYING);
			clearActiveFlag(s, SOUND_LOOPING);
			clearActiveFlag(s, SOUND_PAUSED);
		}

		for (s32 b=0; b<s_numBuffers; b++)
		{
			s_buffers[b].refCount = 0;
		}

		s_mutex->Unlock(); 
	}

	s32 soundsPlaying()
	{
		s32 numSoundsPlaying = 0;

		s_mutex->Lock();
		for (s32 s=0; s<s_maxSimulSounds; s++)
		{
			if (checkActiveFlag(s, SOUND_PLAYING))
			{
				numSoundsPlaying++;
			}
		}
		s_mutex->Unlock(); 

		return numSoundsPlaying;
	}

	void pauseSound(SoundHandle handle)
	{
		s_mutex->Lock();

		if (!isActiveNoLock(handle)) { s_mutex->Unlock(); return; }
		const u32 sourceID = getHandleSource(handle);

		//if the sound is not playing then it doesn't need to be stopped.
		if (!checkActiveFlag(sourceID, SOUND_PLAYING))
		{
			s_mutex->Unlock();
			return;
		}

		//stop the sound but leave the source intact.
		alSourcePause( s_sources[sourceID] );

		//clear the playing flag.
		clearActiveFlag(sourceID, SOUND_PLAYING);
		setActiveFlag(sourceID, SOUND_PAUSED);

		const u32 bufferID = getHandleBuffer(handle);
		s_buffers[bufferID].lastUsed = s_currentFrame;

		s_mutex->Unlock();
	}

	void resumeSound(SoundHandle handle)
	{
		s_mutex->Lock();

		if (!isActiveNoLock(handle)) { s_mutex->Unlock(); return; }
		const u32 sourceID = getHandleSource(handle);

		//if the sound is not playing then it doesn't need to be stopped.
		if (!checkActiveFlag(sourceID, SOUND_PAUSED))
		{
			s_mutex->Unlock();
			return;
		}

		//resume the sound
		alSourcePlay( s_sources[sourceID] );

		//set the playing flag.
		setActiveFlag(sourceID, SOUND_PLAYING);
		clearActiveFlag(sourceID, SOUND_PAUSED);

		const u32 bufferID = getHandleBuffer(handle);
		s_buffers[bufferID].lastUsed = s_currentFrame;

		s_mutex->Unlock();
	}

	void setPan(SoundHandle handle, f32 pan)
	{
		s_mutex->Lock();

		if (!isActiveNoLock(handle)) { s_mutex->Unlock(); return; }
		const u32 sourceID = getHandleSource(handle);
		const f32 sourcePosAL[] = { pan, 0.0f, 0.0f }; 

		alSourcefv(s_sources[sourceID], AL_POSITION, sourcePosAL);

		const u32 bufferID = getHandleBuffer(handle);
		s_buffers[bufferID].lastUsed = s_currentFrame;

		s_mutex->Unlock();
	}

	void setVolume(SoundHandle handle, f32 volume)
	{
		s_mutex->Lock();

		if (!isActiveNoLock(handle)) { s_mutex->Unlock(); return; }
		//set the gain.
		const u32 sourceID = getHandleSource(handle);
		const f32 gain = min(volume * s_globalVolume, 1.0f);
		alSourcef( s_sources[sourceID], AL_GAIN, gain );

		const u32 bufferID = getHandleBuffer(handle);
		s_buffers[bufferID].lastUsed = s_currentFrame;

		s_mutex->Unlock();
	}

	//////////////////////////////////////////////////////////////////
	//Internal implementation
	//////////////////////////////////////////////////////////////////
	bool isActiveNoLock(SoundHandle handle)
	{
		if (!s_init || handle == INVALID_SOUND_HANDLE)
		{
			return false;
		}

		const u32 sourceID = getHandleSource(handle);
		const u32 bufferID = getHandleBuffer(handle);
		const u32 allocID  = getHandleAllocID(handle);

		const bool isActive = checkActiveFlag(sourceID, SOUND_ACTIVE);
		const u32 activeAllocID = getActiveAllocID(sourceID);
		const u32 activeBufferID = getActiveBuffer(sourceID);

		const bool soundIsActive = (allocID == activeAllocID && bufferID == activeBufferID && isActive);
		return soundIsActive;
	}

	SoundBuffer* getSoundBuffer(const char* name)
	{
		//is a buffer already loaded with this data?
		BufferMap::iterator iBuffer = s_bufferMap.find(name);
		if (iBuffer != s_bufferMap.end())
		{
			return &s_buffers[ iBuffer->second ];
		}

		//if not then find a free buffer.
		for (s32 i=0; i<s_numBuffers; i++)
		{
			if ( !(s_buffers[i].flags&BUFFER_ACTIVE) )
			{
				s_buffers[i].name = name;
				s_bufferMap[name] = i;

				return &s_buffers[i];
			}
		}

		//if all buffers are active, try to find the oldest one that is no longer referenced.
		u64 oldestFrame = 0xffffffffffffffffULL;
		s32 oldestIndex = -1;
		for (s32 i=0; i<s_numBuffers; i++)
		{
			if ( s_buffers[i].refCount == 0 )
			{
				if (s_buffers[i].lastUsed < oldestFrame)
				{
					oldestFrame = s_buffers[i].lastUsed;
					oldestIndex = i;
				}
			}
		}

		if (oldestIndex >= 0)
		{
			//free the buffer.
			SoundBuffer* buffer = &s_buffers[oldestIndex];
			buffer->refCount = 0;
			buffer->flags = 0;

			//remove the old name from the map
			BufferMap::iterator iOldBuffer = s_bufferMap.find(buffer->name);
			if (iOldBuffer != s_bufferMap.end())
			{
				s_bufferMap.erase(iOldBuffer);
			}
			s_bufferMap[name] = oldestIndex;

			buffer->name = name;
			
			//return it.
			return buffer;
		}

		return NULL;
	}

	SoundHandle allocateSound(u32 bufferID)
	{
		//find an active sound that is no longer playing.
		s32 soundID = -1;
		for (s32 i=0; i<s_maxSimulSounds; i++)
		{
			if (!checkActiveFlag(i, SOUND_PLAYING) && !checkActiveFlag(i, SOUND_PAUSED))
			{
				soundID = s32(i);
				break;
			}
		}

		//for now do not overwrite sounds already playing.
		if (soundID < 0)
		{
			return INVALID_SOUND_HANDLE;
		}

		u32 allocID = getActiveAllocID(soundID);
		allocID = (allocID + 1) & 0x7ffff;

		setActiveBuffer(soundID, bufferID);
		setActiveAllocID(soundID, allocID);

		clearActiveFlag(soundID, SOUND_PLAYING);
		clearActiveFlag(soundID, SOUND_LOOPING);
		setActiveFlag(soundID, SOUND_ACTIVE);
		
		//create a sound handle.
		return SoundHandle( bufferID | (soundID<<8) | (allocID<<13) );
	}
	
	bool playSoundInternal(SoundHandle sound, f32 volume, f32 pan, Bool loop, Bool is3D)
	{
		const u32 sourceID = getHandleSource(sound);
		const u32 bufferID = getHandleBuffer(sound);
		const ALuint oalSource = s_sources[sourceID];

		alSourceStop(oalSource);
		alSourcei( oalSource, AL_BUFFER, s_buffers[bufferID].oalBuffer );
		alSourcef( oalSource, AL_ROLLOFF_FACTOR, 1.0f );

		//this is a "2D" source.
		if (!is3D)
		{
			alSourcei( oalSource, AL_SOURCE_RELATIVE, AL_TRUE );
			alSourcef( oalSource, AL_REFERENCE_DISTANCE, 15.0f );
			alSourcef( oalSource, AL_MAX_DISTANCE, 200.0f );
			
			const f32 pos2D[] = { pan, 0.0f, 0.0f };
			alSourcefv( oalSource, AL_POSITION, pos2D );
		}
		else
		{
			//adjust the hearing distance...
			f32 distScale = max(volume, 1.0f);
			alSourcef( oalSource, AL_REFERENCE_DISTANCE, 15.0f*distScale );
			alSourcef( oalSource, AL_MAX_DISTANCE, 200.0f*distScale );
		}

		//set looping.
		alSourcei( oalSource, AL_LOOPING, loop ? AL_TRUE : AL_FALSE );

		//set the gain.
		f32 gain = min(volume * s_globalVolume, 1.0f);
		alSourcef( oalSource, AL_GAIN, gain );

		//finally play the sound.
		alSourcePlay( oalSource );

		//mark the sound as playing.
		setActiveFlag(sourceID, SOUND_PLAYING);
		if (loop)
		{
			setActiveFlag(sourceID, SOUND_LOOPING);
		}

		s_buffers[bufferID].lastUsed = s_currentFrame;
		s_buffers[bufferID].refCount++;

		return true;
	}

	const void* getRawSoundData(const void* data, u32 size, u32 type, u32& rawSize)
	{
		if (type == STYPE_RAW)
		{
			rawSize = size;
			return data;
		}
		else if (type == STYPE_VOC)
		{
			if (!Voc::read((u8*)data, size))
			{
				LOG( LOG_ERROR, "Cannot read VOC data for sound." );
				return NULL;
			}

			rawSize = Voc::getRawSize();
			const void* outData = Voc::getRawData();
			Voc::free();

			return outData;
		}

		return NULL;
	}
}