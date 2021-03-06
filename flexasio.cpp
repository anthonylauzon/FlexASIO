/*

	Copyright (C) 2014 Etienne Dechamps (e-t172) <etienne@edechamps.fr>

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Lesser General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "flexasio.h"

#include <MMReg.h>
#include "pa_win_wasapi.h"

CFlexASIO::CFlexASIO() :
	portaudio_initialized(false), init_error(""), pa_api_info(nullptr),
	input_device_info(nullptr), output_device_info(nullptr),
	input_channel_count(0), output_channel_count(0),
	input_channel_mask(0), output_channel_mask(0),
	sample_rate(0), buffers(nullptr), stream(NULL), started(false)
{
	Log() << "CFlexASIO::CFlexASIO()";
}

ASIOBool CFlexASIO::init(void* sysHandle)
{
	Log() << "CFlexASIO::init()";
	if (input_device_info || output_device_info)
	{
		Log() << "Already initialized";
		return ASE_NotPresent;
	}

	Log() << "Initializing PortAudio";
	PaError error = Pa_Initialize();
	if (error != paNoError)
	{
		init_error = std::string("Could not initialize PortAudio: ") + Pa_GetErrorText(error);
		Log() << init_error;
		return ASIOFalse;
	}
	portaudio_initialized = true;

	// The default API used by PortAudio is WinMME. It's also the worst one.
	// The following attempts to get a better API (in order of preference).
	PaHostApiIndex pa_api_index = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
	if (pa_api_index == paHostApiNotFound)
		pa_api_index = Pa_HostApiTypeIdToHostApiIndex(paDirectSound);
	if (pa_api_index == paHostApiNotFound)
		pa_api_index = Pa_GetDefaultHostApi();
	if (pa_api_index < 0)
	{
		init_error = "Unable to get PortAudio API index";
		Log() << init_error;
		return ASIOFalse;
	}

	pa_api_info = Pa_GetHostApiInfo(pa_api_index);
	if (!pa_api_info)
	{
		init_error = "Unable to get PortAudio API info";
		Log() << init_error;
		return ASIOFalse;
	}
	Log() << "Selected host API #" << pa_api_index << " (" << pa_api_info->name << ")";

	sample_rate = 0;

	Log() << "Getting input device info";
	if (pa_api_info->defaultInputDevice != paNoDevice)
	{
		input_device_info = Pa_GetDeviceInfo(pa_api_info->defaultInputDevice);
		if (!input_device_info)
		{
			init_error = std::string("Unable to get input device info");
			Log() << init_error;
			return ASIOFalse;
		}
		Log() << "Selected input device: " << input_device_info->name;
		input_channel_count = input_device_info->maxInputChannels;
		sample_rate = (std::max)(input_device_info->defaultSampleRate, sample_rate);
	}

	Log() << "Getting output device info";
	if (pa_api_info->defaultOutputDevice != paNoDevice)
	{
		output_device_info = Pa_GetDeviceInfo(pa_api_info->defaultOutputDevice);
		if (!output_device_info)
		{
			init_error = std::string("Unable to get output device info");
			Log() << init_error;
			return ASIOFalse;
		}
		Log() << "Selected output device: " << output_device_info->name;
		output_channel_count = output_device_info->maxOutputChannels;
		sample_rate = (std::max)(output_device_info->defaultSampleRate, sample_rate);
	}

	if (pa_api_info->type == paWASAPI)
	{
		// PortAudio has some WASAPI-specific goodies to make us smarter.
		WAVEFORMATEXTENSIBLE input_waveformat;
		PaError error = PaWasapi_GetDeviceDefaultFormat(&input_waveformat, sizeof(input_waveformat), pa_api_info->defaultInputDevice);
		if (error <= 0)
			Log() << "Unable to get WASAPI default format for input device";
		else
		{
			input_channel_count = input_waveformat.Format.nChannels;
			input_channel_mask = input_waveformat.dwChannelMask;
		}

		WAVEFORMATEXTENSIBLE output_waveformat;
		error = PaWasapi_GetDeviceDefaultFormat(&output_waveformat, sizeof(output_waveformat), pa_api_info->defaultOutputDevice);
		if (error <= 0)
			Log() << "Unable to get WASAPI default format for output device";
		else
		{
			output_channel_count = output_waveformat.Format.nChannels;
			output_channel_mask = output_waveformat.dwChannelMask;
		}
	}

	if (sample_rate == 0)
		sample_rate = 44100;

	Log() << "Initialized successfully";
	return ASIOTrue;
}

CFlexASIO::~CFlexASIO()
{
	Log() << "CFlexASIO::~CFlexASIO()";
	if (started)
		stop();
	if (buffers)
		disposeBuffers();
	if (portaudio_initialized)
	{
		Log() << "Closing PortAudio";
		PaError error = Pa_Terminate();
		if (error != paNoError)
			Log() << "Pa_Terminate() returned " << Pa_GetErrorText(error) << "!";
		else
			Log() << "PortAudio closed successfully";
	}
}

ASIOError CFlexASIO::getClockSources(ASIOClockSource* clocks, long* numSources) throw()
{
	Log() << "CFlexASIO::getClockSources()";
	if (!clocks || !numSources || *numSources < 1)
	{
		Log() << "Invalid parameters";
		return ASE_NotPresent;
	}

	clocks->index = 0;
	clocks->associatedChannel = -1;
	clocks->associatedGroup = -1;
	clocks->isCurrentSource = ASIOTrue;
	strcpy_s(clocks->name, 32, "Internal");
	*numSources = 1;
	return ASE_OK;
}

ASIOError CFlexASIO::setClockSource(long reference) throw()
{
	Log() << "CFlexASIO::setClockSource(" << reference << ")";
	if (reference != 0)
	{
		Log() << "Parameter out of bounds";
		return ASE_InvalidMode;
	}
	return ASE_OK;
}

ASIOError CFlexASIO::getChannels(long* numInputChannels, long* numOutputChannels)
{
	Log() << "CFlexASIO::getChannels()";
	if (!input_device_info && !output_device_info)
	{
		Log() << "getChannels() called in unitialized state";
		return ASE_NotPresent;
	}

	*numInputChannels = input_channel_count;
	*numOutputChannels = output_channel_count;

	Log() << "Returning " << *numInputChannels << " input channels and " << *numOutputChannels << " output channels";
	return ASE_OK;
}

namespace {
std::string getChannelName(size_t channel, DWORD channelMask)
{
	// Search for the matching bit in channelMask
	size_t current_channel = 0;
	DWORD current_channel_speaker = 1;
	for (;;)
	{
		while ((current_channel_speaker & channelMask) == 0 && current_channel_speaker < SPEAKER_ALL)
			current_channel_speaker <<= 1;
		if (current_channel_speaker == SPEAKER_ALL)
			break;
		// Now current_channel_speaker contains the speaker for current_channel
		if (current_channel == channel)
			break;
		++current_channel;
		current_channel_speaker <<= 1;
	}

	std::stringstream channel_name;
	channel_name << channel;
	if (current_channel_speaker == SPEAKER_ALL)
		Log() << "Channel " << channel << " is outside channel mask " << channelMask;
	else
	{
		const char* pretty_name = nullptr;
		switch (current_channel_speaker)
		{
			case SPEAKER_FRONT_LEFT: pretty_name = "FL (Front Left)"; break;
			case SPEAKER_FRONT_RIGHT: pretty_name = "FR (Front Right)"; break;
			case SPEAKER_FRONT_CENTER: pretty_name = "FC (Front Center)"; break;
			case SPEAKER_LOW_FREQUENCY: pretty_name = "LFE (Low Frequency)"; break;
			case SPEAKER_BACK_LEFT: pretty_name = "BL (Back Left)"; break;
			case SPEAKER_BACK_RIGHT: pretty_name = "BR (Back Right)"; break;
			case SPEAKER_FRONT_LEFT_OF_CENTER: pretty_name = "FCL (Front Left Center)"; break;
			case SPEAKER_FRONT_RIGHT_OF_CENTER: pretty_name = "FCR (Front Right Center)"; break;
			case SPEAKER_BACK_CENTER: pretty_name = "BC (Back Center)"; break;
			case SPEAKER_SIDE_LEFT: pretty_name = "SL (Side Left)"; break;
			case SPEAKER_SIDE_RIGHT: pretty_name = "SR (Side Right)"; break;
			case SPEAKER_TOP_CENTER: pretty_name = "TC (Top Center)"; break;
			case SPEAKER_TOP_FRONT_LEFT: pretty_name = "TFL (Top Front Left)"; break;
			case SPEAKER_TOP_FRONT_CENTER: pretty_name = "TFC (Top Front Center)"; break;
			case SPEAKER_TOP_FRONT_RIGHT: pretty_name = "TFR (Top Front Right)"; break;
			case SPEAKER_TOP_BACK_LEFT: pretty_name = "TBL (Top Back left)"; break;
			case SPEAKER_TOP_BACK_CENTER: pretty_name = "TBC (Top Back Center)"; break;
			case SPEAKER_TOP_BACK_RIGHT: pretty_name = "TBR (Top Back Right)"; break;
		}
		if (!pretty_name)
			Log() << "Speaker " << current_channel_speaker << " is unknown";
		else
			channel_name << " " << pretty_name;
	}
	return channel_name.str();
}
}

ASIOError CFlexASIO::getChannelInfo(ASIOChannelInfo* info)
{
	Log() << "CFlexASIO::getChannelInfo()";

	Log() << "Channel info requested for " << (info->isInput ? "input" : "output") << " channel " << info->channel;
	if (info->isInput)
	{
		if (info->channel < 0 || info->channel >= input_channel_count)
		{
			Log() << "No such input channel, returning error";
			return ASE_NotPresent;
		}
	}
	else
	{
		if (info->channel < 0 || info->channel >= output_channel_count)
		{
			Log() << "No such output channel, returning error";
			return ASE_NotPresent;
		}
	}

	info->isActive = false;
	for (std::vector<ASIOBufferInfo>::const_iterator buffers_info_it = buffers_info.begin(); buffers_info_it != buffers_info.end(); ++buffers_info_it)
		if (buffers_info_it->isInput == info->isInput && buffers_info_it->channelNum == info->channel)
		{
			info->isActive = true;
			break;
		}

	info->channelGroup = 0;
	info->type = asio_sample_type;
	std::stringstream channel_string;
	channel_string << (info->isInput ? "IN" : "OUT") << " " << getChannelName(info->channel, info->isInput ? input_channel_mask : output_channel_mask);
	strcpy_s(info->name, 32, channel_string.str().c_str());
	Log() << "Returning: " << info->name << ", " << (info->isActive ? "active" : "inactive") << ", group " << info->channelGroup << ", type " << info->type;
	return ASE_OK;
}

ASIOError CFlexASIO::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity)
{
	// These values are purely arbitrary, since PortAudio doesn't provide them. Feel free to change them if you'd like.
	// TODO: let the user should these values
	Log() << "CFlexASIO::getBufferSize()";
	*minSize = 48; // 1 ms at 48kHz, there's basically no chance we'll get glitch-free streaming below this
	*maxSize = 48000; // 1 second at 48kHz, more would be silly
	*preferredSize = 1024; // typical - 21.3 ms at 48kHz
	*granularity = 1; // Don't care
	Log() << "Returning: min buffer size " << *minSize << ", max buffer size " << *maxSize << ", preferred buffer size " << *preferredSize << ", granularity " << *granularity;
	return ASE_OK;
}

PaError CFlexASIO::OpenStream(PaStream** stream, double sampleRate, unsigned long framesPerBuffer) throw()
{
	Log() << "CFlexASIO::OpenStream(" << sampleRate << ", " << framesPerBuffer << ")";

	PaStreamParameters input_parameters;
	PaWasapiStreamInfo input_wasapi_stream_info;
	if (input_device_info)
	{
		input_parameters.device = pa_api_info->defaultInputDevice;
		input_parameters.channelCount = input_channel_count;
		input_parameters.sampleFormat = portaudio_sample_format | paNonInterleaved;
		input_parameters.suggestedLatency = input_device_info->defaultLowInputLatency;
		input_parameters.hostApiSpecificStreamInfo = NULL;
		if (pa_api_info->type == paWASAPI)
		{
			input_wasapi_stream_info.size = sizeof(input_wasapi_stream_info);
			input_wasapi_stream_info.hostApiType = paWASAPI;
			input_wasapi_stream_info.version = 1;
			input_wasapi_stream_info.flags = 0;
			if (input_channel_mask != 0)
			{
				input_wasapi_stream_info.flags |= paWinWasapiUseChannelMask;
				input_wasapi_stream_info.channelMask = input_channel_mask;
			}
			input_parameters.hostApiSpecificStreamInfo = &input_wasapi_stream_info;
		}
	}

	PaStreamParameters output_parameters;
	PaWasapiStreamInfo output_wasapi_stream_info;
	if (output_device_info)
	{
		output_parameters.device = pa_api_info->defaultOutputDevice;
		output_parameters.channelCount = output_channel_count;
		output_parameters.sampleFormat = portaudio_sample_format | paNonInterleaved;
		output_parameters.suggestedLatency = output_device_info->defaultLowOutputLatency;
		output_parameters.hostApiSpecificStreamInfo = NULL;
		if (pa_api_info->type == paWASAPI)
		{
			output_wasapi_stream_info.size = sizeof(output_wasapi_stream_info);
			output_wasapi_stream_info.hostApiType = paWASAPI;
			output_wasapi_stream_info.version = 1;
			output_wasapi_stream_info.flags = 0;
			if (output_channel_mask != 0)
			{
				output_wasapi_stream_info.flags |= paWinWasapiUseChannelMask;
				output_wasapi_stream_info.channelMask = output_channel_mask;
			}
			output_parameters.hostApiSpecificStreamInfo = &output_wasapi_stream_info;
		}
	}

	return Pa_OpenStream(
		stream,
		input_device_info ? &input_parameters : NULL,
		output_device_info ? &output_parameters : NULL,
		sampleRate, framesPerBuffer, paNoFlag, &CFlexASIO::StaticStreamCallback, this);
}

ASIOError CFlexASIO::canSampleRate(ASIOSampleRate sampleRate) throw()
{
	Log() << "CFlexASIO::canSampleRate(" << sampleRate << ")";
	if (!input_device_info && !output_device_info)
	{
		Log() << "canSampleRate() called in unitialized state";
		return ASE_NotPresent;
	}

	PaStream* temp_stream;
	PaError error = OpenStream(&temp_stream, sampleRate, paFramesPerBufferUnspecified);
	if (error != paNoError)
	{
		init_error = std::string("Cannot do this sample rate: ") + Pa_GetErrorText(error);
		Log() << init_error;
		return ASE_NoClock;
	}

	Log() << "Sample rate is available";
	Pa_CloseStream(temp_stream);
	return ASE_OK;
}

ASIOError CFlexASIO::getSampleRate(ASIOSampleRate* sampleRate) throw()
{
	Log() << "CFlexASIO::getSampleRate()";
	if (sample_rate == 0)
	{
		Log() << "getSampleRate() called in unitialized state";
		return ASE_NoClock;
	}
	*sampleRate = sample_rate;
	Log() << "Returning sample rate: " << *sampleRate;
	return ASE_OK;
}

ASIOError CFlexASIO::setSampleRate(ASIOSampleRate sampleRate) throw()
{
	Log() << "CFlexASIO::setSampleRate(" << sampleRate << ")";
	if (buffers)
	{
		if (callbacks.asioMessage)
		{
			Log() << "Sending a reset request to the host as it's not possible to change sample rate when streaming";
			callbacks.asioMessage(kAsioResetRequest, 0, NULL, NULL);
			return ASE_OK;
		}
		else
		{
			Log() << "Changing the sample rate after createBuffers() is not supported";
			return ASE_NotPresent;
		}
	}
	sample_rate = sampleRate;
	return ASE_OK;
}

ASIOError CFlexASIO::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) throw()
{
	Log() << "CFlexASIO::createBuffers(" << numChannels << ", " << bufferSize << ")";
	if (numChannels < 1 || bufferSize < 1 || !callbacks || !callbacks->bufferSwitch)
	{
		Log() << "Invalid invocation";
		return ASE_InvalidMode;
	}
	if (!input_device_info && !output_device_info)
	{
		Log() << "createBuffers() called in unitialized state";
		return ASE_InvalidMode;
	}
	if (buffers)
	{
		Log() << "createBuffers() called twice";
		return ASE_InvalidMode;
	}

	buffers_info.reserve(numChannels);
	std::unique_ptr<Buffers> temp_buffers(new Buffers(2, numChannels, bufferSize));
	Log() << "Buffers instantiated, memory range : " << temp_buffers->buffers << "-" << temp_buffers->buffers + temp_buffers->getSize();
	for (long channel_index = 0; channel_index < numChannels; ++channel_index)
	{
		ASIOBufferInfo& buffer_info = bufferInfos[channel_index];
		if (buffer_info.isInput)
		{
			if (buffer_info.channelNum < 0 || buffer_info.channelNum >= input_channel_count)
			{
				Log() << "out of bounds input channel";
				return ASE_InvalidMode;
			}
		}
		else
		{
			if (buffer_info.channelNum < 0 || buffer_info.channelNum >= output_channel_count)
			{
				Log() << "out of bounds output channel";
				return ASE_InvalidMode;
			}
		}

		Sample* first_half = temp_buffers->getBuffer(0, channel_index);
		Sample* second_half = temp_buffers->getBuffer(1, channel_index);
		buffer_info.buffers[0] = static_cast<void*>(first_half);
		buffer_info.buffers[1] = static_cast<void*>(second_half);
		Log() << "ASIO buffer #" << channel_index << " is " << (buffer_info.isInput ? "input" : "output") << " channel " << buffer_info.channelNum
		      << " - first half: " << first_half << "-" << first_half + bufferSize << " - second half: " << second_half << "-" << second_half + bufferSize;
		buffers_info.push_back(buffer_info);
	}

	
	Log() << "Opening PortAudio stream";
	if (sample_rate == 0)
	{
		sample_rate = 44100;
		Log() << "The sample rate was never specified, using " << sample_rate << " as fallback";
	}
	PaStream* temp_stream;
	PaError error = OpenStream(&temp_stream, sample_rate, temp_buffers->buffer_size);
	if (error != paNoError)
	{
		init_error = std::string("Unable to open PortAudio stream: ") + Pa_GetErrorText(error);
		Log() << init_error;
		return ASE_HWMalfunction;
	}

	buffers = std::move(temp_buffers);
	stream = temp_stream;
	this->callbacks = *callbacks;
	return ASE_OK;
}

ASIOError CFlexASIO::disposeBuffers() throw()
{
	Log() << "CFlexASIO::disposeBuffers()";
	if (!buffers)
	{
		Log() << "disposeBuffers() called before createBuffers()";
		return ASE_InvalidMode;
	}
	if (started)
	{
		Log() << "disposeBuffers() called before stop()";
		return ASE_InvalidMode;
	}

	Log() << "Closing PortAudio stream";
	PaError error = Pa_CloseStream(stream);
	if (error != paNoError)
	{
		init_error = std::string("Unable to close PortAudio stream: ") + Pa_GetErrorText(error);
		Log() << init_error;
		return ASE_NotPresent;
	}
	stream = NULL;

	buffers.reset();
	buffers_info.clear();
	return ASE_OK;
}

ASIOError CFlexASIO::getLatencies(long* inputLatency, long* outputLatency)
{
	Log() << "CFlexASIO::getLatencies()";
	if (!stream)
	{
		Log() << "getLatencies() called before createBuffers()";
		return ASE_NotPresent;
	}

	const PaStreamInfo* stream_info = Pa_GetStreamInfo(stream);
	if (!stream_info)
	{
		Log() << "Unable to get stream info";
		return ASE_NotPresent;
	}

	// TODO: should we add the buffer size?
	*inputLatency = (long)(stream_info->inputLatency * sample_rate);
	*outputLatency = (long)(stream_info->outputLatency * sample_rate);
	Log() << "Returning input latency of " << *inputLatency << " samples and output latency of " << *outputLatency << " samples";
	return ASE_OK;
}

ASIOError CFlexASIO::start() throw()
{
	Log() << "CFlexASIO::start()";
	if (!buffers)
	{
		Log() << "start() called before createBuffers()";
		return ASE_NotPresent;
	}
	if (started)
	{
		Log() << "start() called twice";
		return ASE_NotPresent;
	}

	host_supports_timeinfo = callbacks.asioMessage &&
		callbacks.asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, NULL, NULL) == 1 &&
		callbacks.asioMessage(kAsioSupportsTimeInfo, 0, NULL, NULL) == 1;
	if (host_supports_timeinfo)
		Log() << "The host supports time info";

	Log() << "Starting stream";
	our_buffer_index = 0;
	position.samples = 0;
	position_timestamp.timestamp = ((long long int) timeGetTime()) * 1000000;
	started = true;
	PaError error = Pa_StartStream(stream);
	if (error != paNoError)
	{
		started = false;
		init_error = std::string("Unable to start PortAudio stream: ") + Pa_GetErrorText(error);
		Log() << init_error;
		return ASE_HWMalfunction;
	}

	Log() << "Started successfully";
	return ASE_OK;
}

ASIOError CFlexASIO::stop()
{
	Log() << "CFlexASIO::stop()";
	if (!started)
	{
		Log() << "stop() called before start()";
		return ASE_NotPresent;
	}

	Log() << "Stopping stream";
	PaError error = Pa_StopStream(stream);
	if (error != paNoError)
	{
		init_error = std::string("Unable to stop PortAudio stream: ") + Pa_GetErrorText(error);
		Log() << init_error;
		return ASE_NotPresent;
	}

	started = false;
	Log() << "Stopped successfully";
	return ASE_OK;
}

int CFlexASIO::StreamCallback(const void *input, void *output, unsigned long frameCount, const PaStreamCallbackTimeInfo *timeInfo, PaStreamCallbackFlags statusFlags)
{
	Log() << "CFlexASIO::StreamCallback("<< frameCount << ")";
	if (!started)
	{
		Log() << "Ignoring callback as stream is not started";
		return paContinue;
	}
	if (frameCount != buffers->buffer_size)
	{
		Log() << "Expected " << buffers->buffer_size << " frames, got " << frameCount << " instead, aborting";
		return paContinue;
	}

	if (statusFlags & paInputOverflow)
		Log() << "INPUT OVERFLOW detected (some input data was discarded)";
	if (statusFlags & paInputUnderflow)
		Log() << "INPUT UNDERFLOW detected (gaps were inserted in the input)";
	if (statusFlags & paOutputOverflow)
		Log() << "OUTPUT OVERFLOW detected (some output data was discarded)";
	if (statusFlags & paOutputUnderflow)
		Log() << "OUTPUT UNDERFLOW detected (gaps were inserted in the output)";

	const Sample* const* input_samples = static_cast<const Sample* const*>(input);
	Sample* const* output_samples = static_cast<Sample* const*>(output);

	for (int output_channel_index = 0; output_channel_index < output_channel_count; ++output_channel_index)
		memset(output_samples[output_channel_index], 0, frameCount * sizeof(Sample));

	size_t locked_buffer_index = (our_buffer_index + 1) % 2; // The host is currently busy with locked_buffer_index and is not touching our_buffer_index.
	Log() << "Transferring between PortAudio and buffer #" << our_buffer_index;
	for (std::vector<ASIOBufferInfo>::const_iterator buffers_info_it = buffers_info.begin(); buffers_info_it != buffers_info.end(); ++buffers_info_it)
	{
		Sample* buffer = reinterpret_cast<Sample*>(buffers_info_it->buffers[our_buffer_index]);
		if (buffers_info_it->isInput)
			memcpy(buffer, input_samples[buffers_info_it->channelNum], frameCount * sizeof(Sample));
		else
			memcpy(output_samples[buffers_info_it->channelNum], buffer, frameCount * sizeof(Sample));
	}

	Log() << "Handing off the buffer to the ASIO host";
	if (!host_supports_timeinfo)
		callbacks.bufferSwitch(our_buffer_index, ASIOFalse);
	else
	{
		ASIOTime time;
		time.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;
		time.timeInfo.speed = 1;
		time.timeInfo.samplePosition = position.asio_samples;
		time.timeInfo.systemTime = position_timestamp.asio_timestamp;
		time.timeInfo.sampleRate = sample_rate;
		time.timeCode.flags = 0;
		time.timeCode.timeCodeSamples.lo = time.timeCode.timeCodeSamples.hi = 0;
		time.timeCode.speed = 1;
		callbacks.bufferSwitchTimeInfo(&time, our_buffer_index, ASIOFalse);
	}
	std::swap(locked_buffer_index, our_buffer_index);
	position.samples += frameCount;
	position_timestamp.timestamp = ((long long int) timeGetTime()) * 1000000;
	
	Log() << "Returning from stream callback";
	return paContinue;
}

ASIOError CFlexASIO::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp)
{
	Log() << "CFlexASIO::getSamplePosition()";
	if (!started)
	{
		Log() << "getSamplePosition() called before start()";
		return ASE_SPNotAdvancing;
	}

	*sPos = position.asio_samples;
	*tStamp = position_timestamp.asio_timestamp;
	Log() << "Returning: sample position " << position.samples << ", timestamp " << position_timestamp.timestamp;
	return ASE_OK;
}
