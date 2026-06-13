#ifndef ADXCORE_H
#define ADXCORE_H

#include <cstdint>
#include <string>
#include <vector>

#include <Shared.h>

/*
 * ADXCore
 *
 * Minimal decoder for CRI "ADX" audio files using the standard
 * type-03 (fixed coefficients) 4-bit ADPCM encoding, with optional
 * looping metadata (ignored on decode, the whole stream is decoded once).
 *
 * The class is intentionally self-contained and stateless after
 * construction: it reads the whole ADX file into memory, decodes it
 * to signed 16-bit PCM and can either return the PCM buffer (for
 * in-memory playback) or write a standard RIFF/WAVE file to disk.
 */
class ADX_File
{
public:
	class Error
	{
	public:
		Error();

	public:
		bool unableToOpen;
		bool notADX;
		bool unsupportedFormat;
		bool corruptedContent;
		bool badStream;

		/* encoder-specific errors (used by encodeFromWAV) */
		bool notWAV;
		bool unsupportedWAV;
		bool writeFailed;
	};

public:
	/* Load and decode an ADX file from disk */
	explicit ADX_File(const std::string &adxName);

	~ADX_File();

	Error getError() const;

	uint16_t getChannelCount() const;

	uint32_t getSampleRate() const;

	uint32_t getSampleCount() const;

	uint16_t getBitsPerSample() const;

	/* Decoded interleaved signed 16-bit PCM samples */
	const std::vector<int16_t> &getPCM() const;

	/* Write the decoded audio as a standard RIFF/WAVE (PCM16) file */
	bool exportWAV(const std::string &path) const;

public:
	/* Convenience one-shot helper: decode adxPath and write a WAV to wavPath.
	 * Returns true on success, false otherwise (check ADX_File::Error for detail
	 * via the optional outError pointer). */
	static bool convertToWAV(const std::string &adxPath, const std::string &wavPath, Error *outError = nullptr);

	/* Quick (header-only) check whether a file looks like a CRI ADX file */
	static bool isADX(const std::string &path);

	/* Encode a 16-bit PCM WAV file (mono or stereo, any sample rate) into a
	 * standard CRI ADX (type 03, 4-bit ADPCM) file. highpassFrequency controls
	 * the prediction filter and defaults to the value commonly used by CRI tools.
	 * Returns true on success, false otherwise (check ADX_File::Error for detail
	 * via the optional outError pointer). */
	static bool encodeFromWAV(const std::string &wavPath, const std::string &adxPath, uint16_t highpassFrequency = 500, Error *outError = nullptr);

private:
	bool decode(const std::vector<uint8_t> &raw);

private:
	Error error;

	uint16_t channelCount;
	uint32_t sampleRate;
	uint32_t sampleCount;
	uint16_t bitsPerSample;

	std::vector<int16_t> pcm; // interleaved
};

#endif // ADXCORE_H
