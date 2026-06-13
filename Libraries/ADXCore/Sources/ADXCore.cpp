#include <ADXCore.h>

#include <cmath>
#include <cstring>
#include <fstream>

using namespace Shared;

namespace
{
	/* Avoid relying on M_PI (not part of standard C++ and not guaranteed to be
	 * defined by <cmath> on MSVC without _USE_MATH_DEFINES, which is fragile
	 * with respect to header include order). */
	constexpr double PI = 3.14159265358979323846;

	/* ADX frames are 18 bytes: 2 bytes scale (big endian, signed) + 16 bytes (32 nibbles) of 4-bit samples */
	constexpr int FRAME_SIZE = 18;
	constexpr int SAMPLES_PER_FRAME = 32;

	/* Clamp helper for 16-bit PCM */
	inline int16_t clampToInt16(int32_t value)
	{
		if (value > 32767) {
			return 32767;
		}
		if (value < -32768) {
			return -32768;
		}
		return static_cast<int16_t>(value);
	}

	/* Compute the type-03 ADX prediction coefficients from the highpass cutoff frequency.
	 * Matches the reference CRI ADX (type 03) decoder formula: 12-bit fixed point
	 * coefficients derived from the highpass cutoff and sample rate. */
	void computeCoefficients(uint32_t sampleRate, uint16_t highpassFrequency, int32_t &coef1, int32_t &coef2)
	{
		if (sampleRate == 0) {
			coef1 = 0;
			coef2 = 0;
			return;
		}

		double x = 2.0 * PI * static_cast<double>(highpassFrequency) / static_cast<double>(sampleRate);

		double a = std::sqrt(2.0) - std::cos(x);
		double b = std::sqrt(2.0) - 1.0;
		double c = (a - std::sqrt((a + b) * (a - b))) / b;

		coef1 = static_cast<int32_t>(std::floor(c * 8192.0));
		coef2 = static_cast<int32_t>(std::floor(c * c * -4096.0));
	}

	/* Minimal PCM16 WAV reader: locates the "fmt " and "data" chunks, skipping
	 * any other chunks (LIST, fact, etc). Returns false if the file isn't a
	 * readable PCM16 mono/stereo WAV. */
	bool readWAV(const std::vector<uint8_t> &raw, uint16_t &channels, uint32_t &sampleRate, std::vector<int16_t> &pcm, ADX_File::Error &error)
	{
		auto readLE16 = [&raw](size_t offset) -> uint16_t {
			return static_cast<uint16_t>(raw[offset] | (raw[offset + 1] << 8));
		};

		auto readLE32 = [&raw](size_t offset) -> uint32_t {
			return static_cast<uint32_t>(raw[offset]) | (static_cast<uint32_t>(raw[offset + 1]) << 8) | (static_cast<uint32_t>(raw[offset + 2]) << 16) | (static_cast<uint32_t>(raw[offset + 3]) << 24);
		};

		if (raw.size() < 12 || std::memcmp(raw.data(), "RIFF", 4) != 0 || std::memcmp(raw.data() + 8, "WAVE", 4) != 0) {
			error.notWAV = true;
			return false;
		}

		bool haveFmt = false;
		bool haveData = false;

		uint16_t audioFormat = 0;
		uint16_t bitsPerSample = 0;

		size_t pos = 12;

		while (pos + 8 <= raw.size()) {
			char chunkId[4];
			std::memcpy(chunkId, raw.data() + pos, 4);
			uint32_t chunkSize = readLE32(pos + 4);
			size_t chunkDataStart = pos + 8;

			if (chunkDataStart + chunkSize > raw.size()) {
				chunkSize = static_cast<uint32_t>(raw.size() - chunkDataStart); // tolerate truncated/odd-sized trailing chunk
			}

			if (std::memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
				audioFormat = readLE16(chunkDataStart);
				channels = readLE16(chunkDataStart + 2);
				sampleRate = readLE32(chunkDataStart + 4);
				bitsPerSample = readLE16(chunkDataStart + 14);
				haveFmt = true;
			}
			else if (std::memcmp(chunkId, "data", 4) == 0) {
				if (!haveFmt) {
					error.unsupportedWAV = true;
					return false;
				}

				if (audioFormat != 1 || bitsPerSample != 16 || channels == 0 || channels > 2) {
					error.unsupportedWAV = true; // only PCM16 mono/stereo is supported
					return false;
				}

				size_t sampleCountTotal = chunkSize / sizeof(int16_t);
				pcm.resize(sampleCountTotal);

				if (sampleCountTotal > 0) {
					std::memcpy(pcm.data(), raw.data() + chunkDataStart, sampleCountTotal * sizeof(int16_t));
				}

				haveData = true;
				break; // ignore any chunks after "data" (e.g. cue/LIST)
			}

			// chunks are padded to even sizes
			pos = chunkDataStart + chunkSize + (chunkSize % 2);
		}

		if (!haveFmt || !haveData) {
			error.unsupportedWAV = true;
			return false;
		}

		if (sampleRate == 0) {
			error.unsupportedWAV = true;
			return false;
		}

		return true;
	}

	void writeBE16(std::vector<uint8_t> &buf, size_t offset, uint16_t value)
	{
		buf[offset] = static_cast<uint8_t>(value >> 8);
		buf[offset + 1] = static_cast<uint8_t>(value & 0xFF);
	}

	void writeBE32(std::vector<uint8_t> &buf, size_t offset, uint32_t value)
	{
		buf[offset] = static_cast<uint8_t>(value >> 24);
		buf[offset + 1] = static_cast<uint8_t>(value >> 16);
		buf[offset + 2] = static_cast<uint8_t>(value >> 8);
		buf[offset + 3] = static_cast<uint8_t>(value & 0xFF);
	}
}

ADX_File::Error::Error() : unableToOpen(false), notADX(false), unsupportedFormat(false), corruptedContent(false), badStream(false), notWAV(false), unsupportedWAV(false), writeFailed(false)
{
}

ADX_File::ADX_File(const std::string &adxName) : channelCount(0), sampleRate(0), sampleCount(0), bitsPerSample(16)
{
	std::ifstream inFile(adxName, std::ios::in | std::ios::binary);

	if (!inFile.is_open()) {
		error.unableToOpen = true;
		return;
	}

	inFile.seekg(0, std::ios::end);
	auto size = static_cast<size_t>(inFile.tellg());
	inFile.seekg(0, std::ios::beg);

	if (size < 4) {
		inFile.close();
		error.notADX = true;
		return;
	}

	std::vector<uint8_t> raw(size);
	inFile.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(size));

	bool readOk = !inFile.fail();
	inFile.close();

	if (!readOk) {
		error.badStream = true;
		return;
	}

	decode(raw);
}

ADX_File::~ADX_File() = default;

ADX_File::Error ADX_File::getError() const
{
	return error;
}

uint16_t ADX_File::getChannelCount() const
{
	return channelCount;
}

uint32_t ADX_File::getSampleRate() const
{
	return sampleRate;
}

uint32_t ADX_File::getSampleCount() const
{
	return sampleCount;
}

uint16_t ADX_File::getBitsPerSample() const
{
	return bitsPerSample;
}

const std::vector<int16_t> &ADX_File::getPCM() const
{
	return pcm;
}

bool ADX_File::decode(const std::vector<uint8_t> &raw)
{
	auto readBE16 = [&raw](size_t offset) -> uint16_t {
		return static_cast<uint16_t>((raw[offset] << 8) | raw[offset + 1]);
	};

	auto readBE32 = [&raw](size_t offset) -> uint32_t {
		return (static_cast<uint32_t>(raw[offset]) << 24) | (static_cast<uint32_t>(raw[offset + 1]) << 16) | (static_cast<uint32_t>(raw[offset + 2]) << 8) | static_cast<uint32_t>(raw[offset + 3]);
	};

	/* CRI ADX header starts with 0x80 0x00 */
	if (raw.size() < 20 || raw[0] != 0x80) {
		error.notADX = true;
		return false;
	}

	uint16_t copyrightOffset = readBE16(2); // offset to "(c)CRI" copyright tag, data starts right after header padding
	uint8_t encodingType = raw[4];
	uint8_t frameSize = raw[5];
	uint8_t bitDepth = raw[6];
	uint8_t channels = raw[7];
	uint32_t sampleRateValue = readBE32(8);
	uint32_t totalSamples = readBE32(12);
	uint16_t highpassFrequency = readBE16(16);
	uint8_t version = raw[18];

	(void)version;

	if (encodingType != 0x03) {
		error.unsupportedFormat = true; // only standard type-03 ADPCM is supported
		return false;
	}

	if (frameSize == 0 || frameSize != FRAME_SIZE) {
		error.unsupportedFormat = true;
		return false;
	}

	if (bitDepth != 4) {
		error.unsupportedFormat = true;
		return false;
	}

	if (channels == 0 || channels > 2) {
		error.unsupportedFormat = true; // mono/stereo only
		return false;
	}

	if (sampleRateValue == 0 || totalSamples == 0) {
		error.corruptedContent = true;
		return false;
	}

	if (copyrightOffset < 2 || static_cast<size_t>(copyrightOffset) + 4 > raw.size()) {
		error.corruptedContent = true;
		return false;
	}

	/* The copyright offset field stores a value such that audio data starts at
	 * (copyrightOffset + 4); the "(c)CRI" copyright tag sits at
	 * (copyrightOffset - 2) .. (copyrightOffset + 2). This matches the standard
	 * CRI ADX header layout used by reference decoders. */
	size_t dataStart = static_cast<size_t>(copyrightOffset) + 4;

	if (dataStart >= raw.size()) {
		error.corruptedContent = true;
		return false;
	}

	int32_t coef1, coef2;
	computeCoefficients(sampleRateValue, highpassFrequency, coef1, coef2);

	channelCount = channels;
	sampleRate = sampleRateValue;
	sampleCount = totalSamples;
	bitsPerSample = 16;

	pcm.assign(static_cast<size_t>(sampleCount) * channelCount, 0);

	std::vector<int16_t> hist1(channelCount, 0);
	std::vector<int16_t> hist2(channelCount, 0);

	size_t bytesPerFrame = static_cast<size_t>(frameSize);

	for (uint16_t ch = 0; ch < channelCount; ++ch) {
		size_t pos = dataStart + static_cast<size_t>(ch) * bytesPerFrame;

		uint32_t sampleIndex = 0;

		while (sampleIndex < sampleCount) {
			if (pos + bytesPerFrame > raw.size()) {
				break; // truncated stream, stop decoding gracefully
			}

			uint16_t rawScale = readBE16(pos);

			if (rawScale & 0x8000) {
				break; // dummy/terminator frame reached
			}

			/* the +1 is important on quiet ADXs (reference decoder behavior) */
			int32_t scale = static_cast<int32_t>(rawScale) + 1;

			for (int n = 0; n < SAMPLES_PER_FRAME && sampleIndex < sampleCount; ++n) {
				size_t bytePos = pos + 2 + (n / 2);
				uint8_t byte = raw[bytePos];

				int8_t nibble;
				if (n % 2 == 0) {
					nibble = static_cast<int8_t>(byte >> 4);
				}
				else {
					nibble = static_cast<int8_t>(byte & 0x0F);
				}

				/* sign-extend 4-bit value */
				if (nibble & 0x08) {
					nibble = static_cast<int8_t>(nibble - 16);
				}

				int32_t sampleDelta = static_cast<int32_t>(nibble) * scale;

				int32_t predicted12 = coef1 * static_cast<int32_t>(hist1[ch]) + coef2 * static_cast<int32_t>(hist2[ch]);
				int32_t predicted = predicted12 >> 12;

				int32_t sampleRaw = predicted + sampleDelta;

				int16_t out = clampToInt16(sampleRaw);

				pcm[static_cast<size_t>(sampleIndex) * channelCount + ch] = out;

				hist2[ch] = hist1[ch];
				hist1[ch] = out;

				++sampleIndex;
			}

			/* advance to this channel's next frame, skipping over the other channels' frames */
			pos += static_cast<size_t>(channelCount) * bytesPerFrame;
		}

		/* zero-fill any remaining samples for this channel if the stream was shorter than declared */
		for (; sampleIndex < sampleCount; ++sampleIndex) {
			pcm[static_cast<size_t>(sampleIndex) * channelCount + ch] = 0;
		}
	}

	return true;
}

bool ADX_File::exportWAV(const std::string &path) const
{
	if (!error.unableToOpen && !error.notADX && !error.unsupportedFormat && !error.corruptedContent && !error.badStream) {
		std::ofstream outFile(path, std::ios::out | std::ios::binary | std::ios::trunc);
		if (!outFile.is_open()) {
			return false;
		}

		uint32_t dataSize = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
		uint32_t byteRate = sampleRate * channelCount * (bitsPerSample / 8);
		uint16_t blockAlign = static_cast<uint16_t>(channelCount * (bitsPerSample / 8));
		uint32_t riffSize = 36 + dataSize;
		uint32_t fmtSize = 16;
		uint16_t audioFormat = 1; // PCM

		outFile.write("RIFF", 4);
		outFile.write(reinterpret_cast<const char *>(&riffSize), 4);
		outFile.write("WAVE", 4);

		outFile.write("fmt ", 4);
		outFile.write(reinterpret_cast<const char *>(&fmtSize), 4);
		outFile.write(reinterpret_cast<const char *>(&audioFormat), 2);
		outFile.write(reinterpret_cast<const char *>(&channelCount), 2);
		outFile.write(reinterpret_cast<const char *>(&sampleRate), 4);
		outFile.write(reinterpret_cast<const char *>(&byteRate), 4);
		outFile.write(reinterpret_cast<const char *>(&blockAlign), 2);
		outFile.write(reinterpret_cast<const char *>(&bitsPerSample), 2);

		outFile.write("data", 4);
		outFile.write(reinterpret_cast<const char *>(&dataSize), 4);

		if (!pcm.empty()) {
			outFile.write(reinterpret_cast<const char *>(pcm.data()), dataSize);
		}

		outFile.close();

		return !outFile.fail();
	}

	return false;
}

bool ADX_File::convertToWAV(const std::string &adxPath, const std::string &wavPath, Error *outError)
{
	ADX_File adx(adxPath);

	if (outError != nullptr) {
		*outError = adx.getError();
	}

	auto error = adx.getError();
	if (error.unableToOpen || error.notADX || error.unsupportedFormat || error.corruptedContent || error.badStream) {
		return false;
	}

	return adx.exportWAV(wavPath);
}

bool ADX_File::isADX(const std::string &path)
{
	std::ifstream inFile(path, std::ios::in | std::ios::binary);
	if (!inFile.is_open()) {
		return false;
	}

	uint8_t header[5] = {0};
	inFile.read(reinterpret_cast<char *>(header), 5);

	bool readOk = !inFile.fail();
	inFile.close();

	if (!readOk) {
		return false;
	}

	/* 0x80 marker + encoding type 0x03 (standard ADX ADPCM) */
	return header[0] == 0x80 && header[4] == 0x03;
}

bool ADX_File::encodeFromWAV(const std::string &wavPath, const std::string &adxPath, uint16_t highpassFrequency, Error *outError)
{
	Error encodeError;

	std::ifstream inFile(wavPath, std::ios::in | std::ios::binary);
	if (!inFile.is_open()) {
		encodeError.unableToOpen = true;
		if (outError != nullptr) {
			*outError = encodeError;
		}
		return false;
	}

	inFile.seekg(0, std::ios::end);
	auto size = static_cast<size_t>(inFile.tellg());
	inFile.seekg(0, std::ios::beg);

	std::vector<uint8_t> raw(size);
	if (size > 0) {
		inFile.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(size));
	}

	bool readOk = !inFile.fail();
	inFile.close();

	if (!readOk) {
		encodeError.badStream = true;
		if (outError != nullptr) {
			*outError = encodeError;
		}
		return false;
	}

	uint16_t channels = 0;
	uint32_t sampleRateValue = 0;
	std::vector<int16_t> interleavedPCM;

	if (!readWAV(raw, channels, sampleRateValue, interleavedPCM, encodeError)) {
		if (outError != nullptr) {
			*outError = encodeError;
		}
		return false;
	}

	uint32_t sampleCountTotal = (channels > 0) ? static_cast<uint32_t>(interleavedPCM.size() / channels) : 0;

	if (sampleCountTotal == 0) {
		encodeError.unsupportedWAV = true;
		if (outError != nullptr) {
			*outError = encodeError;
		}
		return false;
	}

	int32_t coef1, coef2;
	computeCoefficients(sampleRateValue, highpassFrequency, coef1, coef2);

	// number of full frames needed (32 samples each); the last frame is padded with silence
	uint32_t frameCount = (sampleCountTotal + SAMPLES_PER_FRAME - 1) / SAMPLES_PER_FRAME;

	constexpr size_t HEADER_SIZE = 40; // matches the standard CRI ADX header layout (copyrightOffset = 36)
	constexpr uint16_t COPYRIGHT_OFFSET = 36;

	std::vector<uint8_t> out(HEADER_SIZE, 0);

	out[0] = 0x80;
	out[1] = 0x00;
	writeBE16(out, 2, COPYRIGHT_OFFSET);
	out[4] = 0x03; // encoding type
	out[5] = FRAME_SIZE;
	out[6] = 4; // bit depth
	out[7] = static_cast<uint8_t>(channels);
	writeBE32(out, 8, sampleRateValue);
	writeBE32(out, 12, sampleCountTotal);
	writeBE16(out, 16, highpassFrequency);
	out[18] = 0x03; // version
	out[19] = 0x00; // flags
	// bytes 20..33 stay zero (padding)
	out[34] = '(';
	out[35] = 'c';
	out[36] = ')';
	out[37] = 'C';
	out[38] = 'R';
	out[39] = 'I';

	std::vector<int16_t> hist1(channels, 0);
	std::vector<int16_t> hist2(channels, 0);

	for (uint16_t ch = 0; ch < channels; ++ch) {
		for (uint32_t frame = 0; frame < frameCount; ++frame) {
			int16_t startH1 = hist1[ch];
			int16_t startH2 = hist2[ch];

			// initial scale estimate: rough lower bound based on sample amplitude,
			// refined below if any nibble would need clamping
			int32_t maxAbsSample = 0;
			for (int n = 0; n < SAMPLES_PER_FRAME; ++n) {
				uint32_t sampleIndex = frame * SAMPLES_PER_FRAME + n;
				if (sampleIndex < sampleCountTotal) {
					int32_t s = std::abs(static_cast<int32_t>(interleavedPCM[static_cast<size_t>(sampleIndex) * channels + ch]));
					if (s > maxAbsSample) {
						maxAbsSample = s;
					}
				}
			}

			int32_t scaleValue = (maxAbsSample + 7) / 8;
			if (scaleValue < 1) {
				scaleValue = 1;
			}

			int32_t nibbles[SAMPLES_PER_FRAME];
			int16_t endH1 = startH1;
			int16_t endH2 = startH2;

			// Sequential encode attempt at a given scale: prediction for sample n
			// depends on the *decoded* values of samples n-1 and n-2 (possibly from
			// the previous frame). clampNibbles forces out-of-range nibbles to
			// [-8, 7] instead of reporting overflow (used for the final, capped attempt).
			auto tryEncode = [&](int32_t scale, bool clampNibbles, bool &overflow) {
				int16_t h1 = startH1;
				int16_t h2 = startH2;
				overflow = false;

				for (int n = 0; n < SAMPLES_PER_FRAME; ++n) {
					uint32_t sampleIndex = frame * SAMPLES_PER_FRAME + n;

					int32_t sample = 0;
					if (sampleIndex < sampleCountTotal) {
						sample = interleavedPCM[static_cast<size_t>(sampleIndex) * channels + ch];
					}

					int32_t pred12 = coef1 * static_cast<int32_t>(h1) + coef2 * static_cast<int32_t>(h2);
					int32_t pred = pred12 >> 12;

					int32_t res = sample - pred;

					int32_t nibble = (res >= 0) ? ((res + scale / 2) / scale) : -(((-res) + scale / 2) / scale);

					if (nibble > 7 || nibble < -8) {
						if (!clampNibbles) {
							overflow = true;
							return;
						}

						if (nibble > 7) {
							nibble = 7;
						}
						if (nibble < -8) {
							nibble = -8;
						}
					}

					int32_t decodedSample = pred + nibble * scale;
					int16_t decoded16 = clampToInt16(decodedSample);

					nibbles[n] = nibble;

					h2 = h1;
					h1 = decoded16;
				}

				endH1 = h1;
				endH2 = h2;
			};

			// increase scale until the frame encodes without nibble overflow, or
			// until the maximum representable scale is reached
			for (;;) {
				bool overflow = false;
				tryEncode(scaleValue, false, overflow);

				if (!overflow) {
					break;
				}

				if (scaleValue >= 0x10000) {
					// at the maximum scale, accept clamping rather than looping forever
					bool dummy = false;
					tryEncode(0x10000, true, dummy);
					scaleValue = 0x10000;
					break;
				}

				++scaleValue;
			}

			uint16_t storedScale = static_cast<uint16_t>(scaleValue - 1); // decoder reconstructs scale = stored + 1

			size_t framePos = HEADER_SIZE + (static_cast<size_t>(frame) * channels + ch) * FRAME_SIZE;

			if (out.size() < framePos + FRAME_SIZE) {
				out.resize(framePos + FRAME_SIZE, 0);
			}

			writeBE16(out, framePos, storedScale);

			for (int n = 0; n < SAMPLES_PER_FRAME; ++n) {
				uint8_t nibbleBits = static_cast<uint8_t>(nibbles[n] & 0x0F);

				size_t bytePos = framePos + 2 + (n / 2);
				if (n % 2 == 0) {
					out[bytePos] = static_cast<uint8_t>((nibbleBits << 4) & 0xF0);
				}
				else {
					out[bytePos] |= static_cast<uint8_t>(nibbleBits & 0x0F);
				}
			}

			hist1[ch] = endH1;
			hist2[ch] = endH2;
		}
	}

	// terminator/dummy frame (per-channel), so decoders relying on the high bit stop cleanly
	{
		size_t terminatorPos = HEADER_SIZE + static_cast<size_t>(frameCount) * channels * FRAME_SIZE;
		out.resize(terminatorPos + static_cast<size_t>(channels) * FRAME_SIZE, 0);

		for (uint16_t ch = 0; ch < channels; ++ch) {
			size_t pos = terminatorPos + static_cast<size_t>(ch) * FRAME_SIZE;
			writeBE16(out, pos, 0x8001);
		}
	}

	std::ofstream outFile(adxPath, std::ios::out | std::ios::binary | std::ios::trunc);
	if (!outFile.is_open()) {
		encodeError.writeFailed = true;
		if (outError != nullptr) {
			*outError = encodeError;
		}
		return false;
	}

	outFile.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
	outFile.close();

	if (outFile.fail()) {
		encodeError.writeFailed = true;
		if (outError != nullptr) {
			*outError = encodeError;
		}
		return false;
	}

	if (outError != nullptr) {
		*outError = encodeError;
	}

	return true;
}
