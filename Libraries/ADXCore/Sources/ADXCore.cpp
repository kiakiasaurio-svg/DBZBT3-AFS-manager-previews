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
	inline int16_t clampToInt16(double value)
	{
		if (value > 32767.0) {
			return 32767;
		}
		if (value < -32768.0) {
			return -32768;
		}
		return static_cast<int16_t>(value);
	}

	/* Compute the type-03 ADX prediction coefficients from the highpass cutoff frequency.
	 * This matches the well-known formula used by standard CRI ADX (type 03) decoders. */
	void computeCoefficients(uint32_t sampleRate, uint16_t highpassFrequency, double &coeff1, double &coeff2)
	{
		if (sampleRate == 0) {
			coeff1 = 0.0;
			coeff2 = 0.0;
			return;
		}

		double x = 2.0 * PI * static_cast<double>(highpassFrequency) / static_cast<double>(sampleRate);
		double y = std::sqrt(2.0) - std::cos(x);
		double z = y - std::sqrt((y + 1.0) * (1.0 - y));

		coeff1 = z * 2.0;
		coeff2 = -(z * z);
	}
}

ADX_File::Error::Error() : unableToOpen(false), notADX(false), unsupportedFormat(false), corruptedContent(false), badStream(false)
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

	if (copyrightOffset < 2 || static_cast<size_t>(copyrightOffset) + 2 > raw.size()) {
		error.corruptedContent = true;
		return false;
	}

	/* The copyright offset field stores the byte position of the "(c)CRI" tag, relative to
	 * byte 2 of the file. Audio data therefore starts right at (copyrightOffset + 2). */
	size_t dataStart = static_cast<size_t>(copyrightOffset) + 2;

	if (dataStart >= raw.size()) {
		error.corruptedContent = true;
		return false;
	}

	double coeff1, coeff2;
	computeCoefficients(sampleRateValue, highpassFrequency, coeff1, coeff2);

	channelCount = channels;
	sampleRate = sampleRateValue;
	sampleCount = totalSamples;
	bitsPerSample = 16;

	pcm.assign(static_cast<size_t>(sampleCount) * channelCount, 0);

	std::vector<double> hist1(channelCount, 0.0);
	std::vector<double> hist2(channelCount, 0.0);

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

			int16_t scale = static_cast<int16_t>(rawScale);

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

				double sample = static_cast<double>(scale) * static_cast<double>(nibble) + coeff1 * hist1[ch] + coeff2 * hist2[ch];

				int16_t out = clampToInt16(sample);

				pcm[static_cast<size_t>(sampleIndex) * channelCount + ch] = out;

				hist2[ch] = hist1[ch];
				hist1[ch] = sample;

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
