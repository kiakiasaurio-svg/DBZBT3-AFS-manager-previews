#include <ISOCore.h>

#include <cstring>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace
{
	constexpr uint64_t SECTOR_SIZE = 2048;

	inline uint16_t readLE16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
	inline uint32_t readLE32(const uint8_t *p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1])<<8) | (static_cast<uint32_t>(p[2])<<16) | (static_cast<uint32_t>(p[3])<<24); }
	inline uint64_t readLE64(const uint8_t *p) { uint64_t v=0; for(int i=7;i>=0;--i) v=(v<<8)|p[i]; return v; }

	enum TagId : uint16_t
	{
		TAG_ANCHOR_VOLUME_DESC  = 2,
		TAG_PARTITION_DESC      = 5,
		TAG_TERMINATING_DESC    = 8,
		TAG_FILE_SET_DESC       = 256,
		TAG_FILE_IDENT_DESC     = 257,
		TAG_ALLOC_EXTENT_DESC   = 261,
		TAG_FILE_ENTRY          = 260,
		TAG_EXTENDED_FILE_ENTRY = 266,
	};

	struct ShortAD { uint32_t length; uint32_t lba; };
	struct LongAD  { uint32_t length; uint32_t lba; };

	ShortAD readShortAD(const uint8_t *p){ return {readLE32(p), readLE32(p+4)}; }
	LongAD  readLongAD (const uint8_t *p){ return {readLE32(p), readLE32(p+4)}; }

	std::string readFileName(const uint8_t *p, uint8_t len)
	{
		if (!len) return {};
		uint8_t comp = p[0];
		std::string s;
		if (comp == 8) {
			for (uint8_t i=1;i<len;++i) s+=static_cast<char>(p[i]);
		} else if (comp == 16) {
			for (uint8_t i=1;i+1<len;i+=2) {
				uint16_t w=(static_cast<uint16_t>(p[i])<<8)|p[i+1];
				s+=static_cast<char>(w&0xFF);
			}
		}
		return s;
	}

	std::string toLower(std::string s)
	{
		std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return std::tolower(c);});
		return s;
	}

	/* Return true if a tag ID represents a file/directory storage object */
	bool isFileEntryTag(uint16_t t)
	{
		return t == TAG_FILE_ENTRY || t == TAG_EXTENDED_FILE_ENTRY || t == TAG_ALLOC_EXTENT_DESC;
	}
}

ISO_File::Error::Error() : unableToOpen(false), notISO(false), unsupportedFormat(false), corruptedContent(false), fileNotFound(false), fileTooLarge(false), writeFailed(false) {}

ISO_File::ISO_File(const std::string &p) : isoPath(p) { parse(); }
ISO_File::~ISO_File() = default;

ISO_File::Error ISO_File::getError() const { return error; }
bool ISO_File::isOpen() const { return !error.unableToOpen && !error.notISO && !error.corruptedContent; }
const std::vector<ISO_File::FileEntry> &ISO_File::getFiles() const { return files; }
const std::string &ISO_File::getPath() const { return isoPath; }

bool ISO_File::readSector(uint64_t lba, std::vector<uint8_t> &buf) const
{
	std::ifstream f(isoPath, std::ios::binary);
	if (!f.is_open()) return false;
	f.seekg(static_cast<std::streamoff>(lba*SECTOR_SIZE));
	buf.assign(SECTOR_SIZE, 0);
	f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(SECTOR_SIZE));
	return !f.fail();
}

bool ISO_File::parse()
{
	std::ifstream f(isoPath, std::ios::binary);
	if (!f.is_open()) { error.unableToOpen = true; return false; }
	f.close();

	// Verify UDF recognition sequence (NSR02/NSR03) in sectors 16-32
	// The marker "NSR0x" appears at bytes[0..4] or bytes[1..5] depending on mastering tool
	std::vector<uint8_t> sec;
	bool hasNSR = false;
	for (uint64_t s = 16; s <= 32 && !hasNSR; ++s) {
		if (!readSector(s, sec)) break;
		if (memcmp(sec.data(),   "NSR0", 4) == 0) { hasNSR = true; break; }
		if (memcmp(sec.data()+1, "NSR0", 4) == 0) { hasNSR = true; break; }
	}
	if (!hasNSR) { error.notISO = true; return false; }

	// Read AVDP at sector 256
	if (!readSector(256, sec)) { error.corruptedContent = true; return false; }
	if (readLE16(sec.data()) != TAG_ANCHOR_VOLUME_DESC) { error.corruptedContent = true; return false; }

	// AVDP bytes 20-23 = Main VDS start LBA
	uint32_t mainVDSLBA = readLE32(sec.data() + 20);

	// Walk Main VDS for Partition Descriptor (we need partitionStart for context,
	// but each FileSet will compute its own partBase from its tagLoc)
	uint32_t partitionStart = 0;
	for (uint32_t i = 0; i < 32; ++i) {
		if (!readSector(mainVDSLBA + i, sec)) break;
		uint16_t tagId = readLE16(sec.data());
		if (tagId == TAG_TERMINATING_DESC) break;
		if (tagId == TAG_PARTITION_DESC) {
			partitionStart = readLE32(sec.data() + 188);
			break;
		}
	}
	if (partitionStart == 0) { error.corruptedContent = true; return false; }

	// Scan for ALL FileSets starting from partitionStart.
	// PS2 UDF Bridge ISOs often contain two separate FileSets:
	//   - FileSet 1 (ISO9660 view): contains VIDEO_TS, AUDIO_TS
	//   - FileSet 2 (UDF view): contains the actual game data (DATA/, BIN/, etc.)
	// Each FileSet defines its own logical partition base:
	//   partBase = absLBA_of_FileSet - tagLoc_of_FileSet
	// This is the correct formula for PS2 UDF Bridge ISOs.
	for (uint64_t i = 0; i < 512; ++i) {
		uint64_t lba = static_cast<uint64_t>(partitionStart) + i;
		if (!readSector(lba, sec)) break;
		uint16_t tagId = readLE16(sec.data());
		if (tagId != TAG_FILE_SET_DESC) continue;

		uint32_t tagLoc = readLE32(sec.data() + 12);
		uint64_t partBase = lba - static_cast<uint64_t>(tagLoc);

		// Root Dir ICB is a LongAD at offset 400 in the FileSet Descriptor
		uint32_t rootIcbLBA = readLE32(sec.data() + 404);
		uint64_t rootFELBA  = partBase + rootIcbLBA;

		if (!readSector(rootFELBA, sec)) continue;
		uint16_t rootTag = readLE16(sec.data());
		if (!isFileEntryTag(rootTag)) continue;

		// Get directory data LBA and length from the root File Entry
		// eaLen at 168, adLen at 172, ADs start at 176+eaLen
		uint32_t eaLen = readLE32(sec.data() + 168);
		uint32_t adLen = readLE32(sec.data() + 172);
		uint8_t  adType = sec[18] & 0x07;
		const uint8_t *adPtr = sec.data() + 176 + eaLen;

		uint64_t dirLBA = 0, dirLen = 0;
		if (adType == 0) { // short ADs
			for (uint32_t off = 0; off + 8 <= adLen; off += 8) {
				ShortAD ad = readShortAD(adPtr + off);
				if (ad.length > 0 && ad.lba > 0) { dirLBA = partBase + ad.lba; dirLen = ad.length; break; }
			}
		} else if (adType == 1) { // long ADs
			for (uint32_t off = 0; off + 16 <= adLen; off += 16) {
				LongAD ad = readLongAD(adPtr + off);
				if (ad.length > 0 && ad.lba > 0) { dirLBA = partBase + ad.lba; dirLen = ad.length; break; }
			}
		}

		if (dirLBA > 0) parseDirectory(dirLBA, dirLen, partBase, "");
	}

	return true;
}

bool ISO_File::parseDirectory(uint64_t lba, uint64_t length, uint64_t partBase, const std::string &parentPath)
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

			if (tagId != TAG_FILE_IDENT_DESC) { pos += 4; continue; }

			uint8_t  fileChar   = p[18];
			uint8_t  fidLen     = p[19];
			uint32_t icbLBA     = readLE32(p + 24);
			uint16_t implUseLen = readLE16(p + 36);

			bool isDir    = (fileChar & 0x02) != 0;
			bool isParent = (fileChar & 0x08) != 0;

			uint64_t totalLen = ((38 + implUseLen + fidLen) + 3) & ~3ULL;

			if (!isParent && fidLen > 0 && icbLBA > 0) {
				std::string name = readFileName(p + 38 + implUseLen, fidLen);
				if (!name.empty()) {
					std::string fullPath = parentPath.empty() ? name : (parentPath + "/" + name);
					uint64_t icbAbsLBA = partBase + icbLBA;

					std::vector<uint8_t> fe;
					if (readSector(icbAbsLBA, fe) && isFileEntryTag(readLE16(fe.data()))) {
						uint32_t feEaLen  = readLE32(fe.data() + 168);
						uint32_t feAdLen  = readLE32(fe.data() + 172);
						uint8_t  feAdType = fe[18] & 0x07;
						const uint8_t *feAdPtr = fe.data() + 176 + feEaLen;

						uint64_t dataLBA = 0, dataLen = 0;
						if (feAdType == 0) {
							for (uint32_t off = 0; off + 8 <= feAdLen; off += 8) {
								ShortAD ad = readShortAD(feAdPtr + off);
								if (ad.length > 0 && ad.lba > 0) { dataLBA = partBase + ad.lba; dataLen = ad.length; break; }
							}
						} else if (feAdType == 1) {
							for (uint32_t off = 0; off + 16 <= feAdLen; off += 16) {
								LongAD ad = readLongAD(feAdPtr + off);
								if (ad.length > 0 && ad.lba > 0) { dataLBA = partBase + ad.lba; dataLen = ad.length; break; }
							}
						}

						if (isDir && dataLBA > 0) {
							parseDirectory(dataLBA, dataLen, partBase, fullPath);
						} else if (!isDir) {
							uint64_t fileSize = readLE64(fe.data() + 56);
							if (fileSize > 0 && dataLBA > 0) {
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
	if (!src.is_open()) { localErr.unableToOpen=true; if(outError)*outError=localErr; return false; }
	std::ofstream dst(destPath, std::ios::binary|std::ios::trunc);
	if (!dst.is_open()) { localErr.writeFailed=true; if(outError)*outError=localErr; return false; }

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
	std::ifstream src(srcPath, std::ios::binary|std::ios::ate);
	if (!src.is_open()) { localErr.unableToOpen=true; if(outError)*outError=localErr; return false; }
	uint64_t srcSize = static_cast<uint64_t>(src.tellg());
	src.seekg(0);

	uint64_t allocBytes = ((entry.size + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;
	if (srcSize > allocBytes) { localErr.fileTooLarge=true; if(outError)*outError=localErr; src.close(); return false; }

	std::fstream iso(isoPath, std::ios::in|std::ios::out|std::ios::binary);
	if (!iso.is_open()) { localErr.writeFailed=true; if(outError)*outError=localErr; src.close(); return false; }

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
	if (!entry) { if(outError) outError->fileNotFound=true; return false; }
	return replaceFile(*entry, srcPath, outError);
}
