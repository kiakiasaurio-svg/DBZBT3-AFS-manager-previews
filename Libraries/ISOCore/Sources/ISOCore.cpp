#include <ISOCore.h>

#include <cstring>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace
{
	constexpr uint64_t SECTOR_SIZE = 2048;

	inline uint16_t readLE16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
	inline uint32_t readLE32(const uint8_t *p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
	inline uint64_t readLE64(const uint8_t *p)
	{
		uint64_t v = 0;
		for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
		return v;
	}

	enum TagId : uint16_t
	{
		TAG_PRIMARY_VOLUME_DESC   = 1,
		TAG_ANCHOR_VOLUME_DESC    = 2,
		TAG_IMPL_USE_VOLUME_DESC  = 4,
		TAG_PARTITION_DESC        = 5,
		TAG_LOGICAL_VOLUME_DESC   = 6,
		TAG_UNALLOCATED_SPACE     = 7,
		TAG_TERMINATING_DESC      = 8,
		TAG_LOGICAL_VOLUME_INT    = 9,
		TAG_FILE_SET_DESC         = 256,
		TAG_FILE_IDENT_DESC       = 257,
		TAG_ALLOC_EXTENT_DESC     = 261,
		TAG_FILE_ENTRY            = 260,
		TAG_EXTENDED_FILE_ENTRY   = 266,
	};

	struct ShortAD { uint32_t length; uint32_t lba; };
	struct LongAD  { uint32_t length; uint32_t lba; uint16_t partRef; };

	ShortAD readShortAD(const uint8_t *p) { return {readLE32(p), readLE32(p+4)}; }
	LongAD  readLongAD (const uint8_t *p) { return {readLE32(p), readLE32(p+4), readLE16(p+8)}; }

	std::string readFileName(const uint8_t *p, uint8_t len)
	{
		if (len == 0) return {};
		uint8_t compId = p[0];
		std::string s;
		if (compId == 8) {
			for (uint8_t i = 1; i < len; ++i) s += static_cast<char>(p[i]);
		}
		else if (compId == 16) {
			for (uint8_t i = 1; i + 1 < len; i += 2) {
				uint16_t w = (static_cast<uint16_t>(p[i]) << 8) | p[i+1];
				s += static_cast<char>(w & 0xFF);
			}
		}
		return s;
	}

	std::string toLower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
		return s;
	}
}

ISO_File::Error::Error() : unableToOpen(false), notISO(false), unsupportedFormat(false), corruptedContent(false), fileNotFound(false), fileTooLarge(false), writeFailed(false) {}

ISO_File::ISO_File(const std::string &isoPath) : isoPath(isoPath)
{
	parse();
}

ISO_File::~ISO_File() = default;

ISO_File::Error ISO_File::getError() const { return error; }

bool ISO_File::isOpen() const
{
	return !error.unableToOpen && !error.notISO && !error.unsupportedFormat && !error.corruptedContent;
}

const std::vector<ISO_File::FileEntry> &ISO_File::getFiles() const { return files; }
const std::string &ISO_File::getPath() const { return isoPath; }

bool ISO_File::readSector(uint64_t lba, std::vector<uint8_t> &buf) const
{
	std::ifstream f(isoPath, std::ios::binary);
	if (!f.is_open()) return false;
	f.seekg(static_cast<std::streamoff>(lba * SECTOR_SIZE));
	buf.assign(SECTOR_SIZE, 0);
	f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(SECTOR_SIZE));
	return !f.fail();
}

bool ISO_File::parse()
{
	std::ifstream f(isoPath, std::ios::binary);
	if (!f.is_open()) { error.unableToOpen = true; return false; }
	f.close();

	// Scan sectors 16-32 for NSR02/NSR03 UDF recognition sequence
	// In PS2 ISOs this is typically at sector 17-19, at bytes[1:6]
	bool hasNSR = false;
	std::vector<uint8_t> sec;
	for (uint64_t s = 16; s <= 32; ++s) {
		if (!readSector(s, sec)) break;
		// NSR marker is at bytes[1..5] (type byte at [0], then "NSR0x")
		if (memcmp(sec.data() + 1, "NSR0", 4) == 0) { hasNSR = true; break; }
		// Some ISOs put it at bytes[0..4]
		if (memcmp(sec.data(), "NSR0", 4) == 0) { hasNSR = true; break; }
	}

	if (!hasNSR) { error.notISO = true; return false; }

	// Read AVDP at sector 256
	if (!readSector(256, sec)) { error.corruptedContent = true; return false; }
	uint16_t avdpTag = readLE16(sec.data());
	if (avdpTag != TAG_ANCHOR_VOLUME_DESC) { error.corruptedContent = true; return false; }

	// AVDP: bytes 16-23 = Main VDS (length uint32 at 16, LBA uint32 at 20)
	uint32_t mainVDSLBA = readLE32(sec.data() + 20);

	// Walk Main VDS to find Partition Descriptor and Logical Volume Descriptor
	uint32_t partitionStart = 0;
	bool foundPartition = false;

	for (uint32_t i = 0; i < 32; ++i) {
		if (!readSector(mainVDSLBA + i, sec)) break;
		uint16_t tagId = readLE16(sec.data());
		if (tagId == TAG_TERMINATING_DESC) break;
		if (tagId == TAG_PARTITION_DESC) {
			// Partition start at byte 188
			partitionStart = readLE32(sec.data() + 188);
			foundPartition = true;
		}
	}

	if (!foundPartition) { error.corruptedContent = true; return false; }

	// Find the FileSet Descriptor by scanning from partitionStart
	// (more reliable than parsing LVD's LogicalVolumeContentsUse which
	// varies across UDF versions and PS2 mastering tools)
	uint64_t fsdLBA = 0;
	bool foundFSD = false;

	for (uint32_t i = 0; i < 16; ++i) {
		uint64_t lba = static_cast<uint64_t>(partitionStart) + i;
		if (!readSector(lba, sec)) break;
		uint16_t tagId = readLE16(sec.data());
		if (tagId == TAG_FILE_SET_DESC) { fsdLBA = lba; foundFSD = true; break; }
		if (tagId == TAG_TERMINATING_DESC) break;
	}

	if (!foundFSD) { error.corruptedContent = true; return false; }

	// Read FileSet Descriptor — Root Dir ICB at offset 400 (LongAD 16 bytes)
	if (!readSector(fsdLBA, sec)) { error.corruptedContent = true; return false; }
	LongAD rootIcb = readLongAD(sec.data() + 400);
	uint64_t rootDirFELBA = static_cast<uint64_t>(partitionStart) + rootIcb.lba;

	// Read root directory File Entry
	if (!readSector(rootDirFELBA, sec)) { error.corruptedContent = true; return false; }
	uint16_t rootTagId = readLE16(sec.data());
	if (rootTagId != TAG_FILE_ENTRY && rootTagId != TAG_EXTENDED_FILE_ENTRY && rootTagId != TAG_ALLOC_EXTENT_DESC) {
		error.corruptedContent = true;
		return false;
	}

	// Get data allocation descriptors from File Entry
	// File Entry: icbFlags at 18, eaLen at 168, adLen at 172, ADs at 176+eaLen
	uint8_t adType = sec[18] & 0x07;
	uint32_t eaLen = readLE32(sec.data() + 168);
	uint32_t adLen = readLE32(sec.data() + 172);
	const uint8_t *adPtr = sec.data() + 176 + eaLen;

	uint64_t rootDataLBA = 0;
	uint64_t rootDataLen = 0;

	if (adType == 0) { // short ADs
		for (uint32_t off = 0; off + 8 <= adLen; off += 8) {
			ShortAD ad = readShortAD(adPtr + off);
			if (ad.length > 0) {
				rootDataLBA = static_cast<uint64_t>(partitionStart) + ad.lba;
				rootDataLen = ad.length;
				break;
			}
		}
	}
	else if (adType == 1) { // long ADs
		for (uint32_t off = 0; off + 16 <= adLen; off += 16) {
			LongAD ad = readLongAD(adPtr + off);
			if (ad.length > 0) {
				rootDataLBA = static_cast<uint64_t>(partitionStart) + ad.lba;
				rootDataLen = ad.length;
				break;
			}
		}
	}

	if (rootDataLBA == 0) { error.corruptedContent = true; return false; }

	parseDirectory(rootDataLBA, rootDataLen, partitionStart, "");
	return true;
}

bool ISO_File::parseDirectory(uint64_t lba, uint64_t length, uint32_t partitionStart, const std::string &parentPath)
{
	uint64_t bytesLeft = length;
	uint64_t curLBA = lba;

	while (bytesLeft > 0) {
		std::vector<uint8_t> sec;
		if (!readSector(curLBA, sec)) break;

		uint64_t sectorBytes = std::min(bytesLeft, SECTOR_SIZE);
		uint64_t pos = 0;

		while (pos + 38 <= sectorBytes) {
			const uint8_t *p = sec.data() + pos;
			uint16_t tagId = readLE16(p);

			if (tagId != TAG_FILE_IDENT_DESC) {
				// Advance by minimum alignment (4 bytes) looking for next FID
				pos += 4;
				continue;
			}

			uint8_t  fileChar    = p[18];
			uint8_t  fidLen      = p[19];
			LongAD   icb         = readLongAD(p + 20);
			uint16_t implUseLen  = readLE16(p + 36);

			bool isDir    = (fileChar & 0x02) != 0;
			bool isParent = (fileChar & 0x08) != 0;

			uint64_t totalLen = 38 + implUseLen + fidLen;
			totalLen = (totalLen + 3) & ~3ULL;

			if (!isParent && fidLen > 0 && icb.length > 0) {
				const uint8_t *namePtr = p + 38 + implUseLen;
				std::string name = readFileName(namePtr, fidLen);

				if (!name.empty()) {
					std::string fullPath = parentPath.empty() ? name : (parentPath + "/" + name);
					uint64_t icbAbsLBA = static_cast<uint64_t>(partitionStart) + icb.lba;

					if (isDir) {
						// Read File Entry for this subdir
						std::vector<uint8_t> fe;
						if (readSector(icbAbsLBA, fe)) {
							uint16_t feTag = readLE16(fe.data());
							if (feTag == TAG_FILE_ENTRY || feTag == TAG_EXTENDED_FILE_ENTRY) {
								uint8_t adType   = fe[18] & 0x07;
								uint32_t feEaLen = readLE32(fe.data() + 168);
								uint32_t feAdLen = readLE32(fe.data() + 172);
								const uint8_t *feAdPtr = fe.data() + 176 + feEaLen;

								uint64_t dirLBA = 0, dirLen = 0;
								if (adType == 0) {
									for (uint32_t off = 0; off + 8 <= feAdLen; off += 8) {
										ShortAD ad = readShortAD(feAdPtr + off);
										if (ad.length > 0) { dirLBA = static_cast<uint64_t>(partitionStart) + ad.lba; dirLen = ad.length; break; }
									}
								}
								else if (adType == 1) {
									for (uint32_t off = 0; off + 16 <= feAdLen; off += 16) {
										LongAD ad = readLongAD(feAdPtr + off);
										if (ad.length > 0) { dirLBA = static_cast<uint64_t>(partitionStart) + ad.lba; dirLen = ad.length; break; }
									}
								}
								if (dirLBA > 0) parseDirectory(dirLBA, dirLen, partitionStart, fullPath);
							}
						}
					}
					else {
						// File: read its File Entry for size and data LBA
						std::vector<uint8_t> fe;
						if (readSector(icbAbsLBA, fe)) {
							uint16_t feTag = readLE16(fe.data());
							if (feTag == TAG_FILE_ENTRY || feTag == TAG_EXTENDED_FILE_ENTRY) {
								uint64_t fileSize = readLE64(fe.data() + 56);
								uint8_t adType    = fe[18] & 0x07;
								uint32_t feEaLen  = readLE32(fe.data() + 168);
								uint32_t feAdLen  = readLE32(fe.data() + 172);
								const uint8_t *feAdPtr = fe.data() + 176 + feEaLen;

								uint64_t dataLBA = 0;
								if (adType == 0) {
									for (uint32_t off = 0; off + 8 <= feAdLen; off += 8) {
										ShortAD ad = readShortAD(feAdPtr + off);
										if (ad.length > 0) { dataLBA = static_cast<uint64_t>(partitionStart) + ad.lba; break; }
									}
								}
								else if (adType == 1) {
									for (uint32_t off = 0; off + 16 <= feAdLen; off += 16) {
										LongAD ad = readLongAD(feAdPtr + off);
										if (ad.length > 0) { dataLBA = static_cast<uint64_t>(partitionStart) + ad.lba; break; }
									}
								}

								if (dataLBA > 0) {
									FileEntry entry;
									entry.name     = name;
									entry.fullPath = fullPath;
									entry.size     = fileSize;
									entry.lba      = dataLBA;
									entry.offset   = dataLBA * SECTOR_SIZE;
									files.push_back(entry);
								}
							}
						}
					}
				}
			}

			if (totalLen == 0) break;
			pos += totalLen;
		}

		bytesLeft -= std::min(bytesLeft, SECTOR_SIZE);
		++curLBA;
	}

	return true;
}

const ISO_File::FileEntry *ISO_File::findFile(const std::string &name) const
{
	std::string lower = toLower(name);
	for (const auto &e : files) {
		if (toLower(e.name) == lower || toLower(e.fullPath) == lower) return &e;
	}
	return nullptr;
}

bool ISO_File::extractFile(const FileEntry &entry, const std::string &destPath, Error *outError) const
{
	Error localErr;
	std::ifstream src(isoPath, std::ios::binary);
	if (!src.is_open()) { localErr.unableToOpen = true; if (outError) *outError = localErr; return false; }

	std::ofstream dst(destPath, std::ios::binary | std::ios::trunc);
	if (!dst.is_open()) { localErr.writeFailed = true; if (outError) *outError = localErr; return false; }

	src.seekg(static_cast<std::streamoff>(entry.offset));
	uint64_t remaining = entry.size;
	std::vector<char> buf(SECTOR_SIZE);

	while (remaining > 0) {
		uint64_t toRead = std::min(remaining, SECTOR_SIZE);
		std::fill(buf.begin(), buf.end(), 0);
		src.read(buf.data(), static_cast<std::streamsize>(toRead));
		dst.write(buf.data(), static_cast<std::streamsize>(toRead));
		remaining -= toRead;
	}

	src.close(); dst.close();
	if (outError) *outError = localErr;
	return !dst.fail();
}

bool ISO_File::replaceFile(const FileEntry &entry, const std::string &srcPath, Error *outError)
{
	Error localErr;

	std::ifstream src(srcPath, std::ios::binary | std::ios::ate);
	if (!src.is_open()) { localErr.unableToOpen = true; if (outError) *outError = localErr; return false; }

	uint64_t srcSize = static_cast<uint64_t>(src.tellg());
	src.seekg(0);

	// Allocated space = sector-aligned original size
	uint64_t allocBytes = ((entry.size + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;
	if (srcSize > allocBytes) { localErr.fileTooLarge = true; if (outError) *outError = localErr; src.close(); return false; }

	std::fstream iso(isoPath, std::ios::in | std::ios::out | std::ios::binary);
	if (!iso.is_open()) { localErr.writeFailed = true; if (outError) *outError = localErr; src.close(); return false; }

	iso.seekp(static_cast<std::streamoff>(entry.offset));
	std::vector<char> buf(SECTOR_SIZE, 0);
	uint64_t remaining = srcSize;

	while (remaining > 0) {
		uint64_t toRead = std::min(remaining, SECTOR_SIZE);
		std::fill(buf.begin(), buf.end(), 0);
		src.read(buf.data(), static_cast<std::streamsize>(toRead));
		iso.write(buf.data(), static_cast<std::streamsize>(SECTOR_SIZE));
		remaining -= toRead;
	}

	// Zero-pad remaining allocated sectors
	uint64_t writtenSectors = (srcSize + SECTOR_SIZE - 1) / SECTOR_SIZE;
	uint64_t totalSectors   = allocBytes / SECTOR_SIZE;
	std::fill(buf.begin(), buf.end(), 0);
	for (uint64_t s = writtenSectors; s < totalSectors; ++s)
		iso.write(buf.data(), static_cast<std::streamsize>(SECTOR_SIZE));

	src.close(); iso.close();
	if (outError) *outError = localErr;
	return !iso.fail();
}

bool ISO_File::replaceFileByName(const std::string &name, const std::string &srcPath, Error *outError)
{
	const FileEntry *entry = findFile(name);
	if (!entry) { if (outError) outError->fileNotFound = true; return false; }
	return replaceFile(*entry, srcPath, outError);
}
