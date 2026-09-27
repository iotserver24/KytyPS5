#include "libs/ajm/mp3_parser.h"
#include "libs/audio.h"

#include <algorithm>
#include <cstring>

namespace Libs::Audio::Ajm {

static_assert(sizeof(AjmDecMp3FrameInfo) == 0x28);
static_assert(offsetof(AjmDecMp3FrameInfo, num_channels) == 0x08);
static_assert(offsetof(AjmDecMp3FrameInfo, encoder_delay) == 0x18);
static_assert(offsetof(AjmDecMp3FrameInfo, ofl_type) == 0x24);

namespace {

uint32_t ReadPartLength(const uint8_t* side_info, size_t bit_offset) {
	uint32_t length = 0;
	for (size_t bit = bit_offset; bit < bit_offset + 12; ++bit) {
		length = (length << 1u) | ((side_info[bit / 8] >> (7u - bit % 8)) & 1u);
	}
	return length;
}

bool HasFghChecksum(const uint8_t* record) {
	uint8_t crc = 0xff;
	for (size_t byte = 0; byte < 9; ++byte) {
		for (int bit = 7; bit >= 0; --bit) {
			const bool same = ((crc >> 7u) & 1u) == ((record[byte] >> bit) & 1u);
			crc             = static_cast<uint8_t>((crc << 1u) ^ (same ? 0x45u : 0u));
		}
	}
	return crc == record[9];
}

void ParseOfl(const uint8_t* data, size_t size, const AjmMp3FrameHeader& header,
              AjmDecMp3FrameInfo& info) {
	const size_t end        = std::min<size_t>(size, header.frame_size);
	const size_t side_start = 4 + (header.crc == 0 ? 2 : 0);
	const bool   mpeg1      = ((header.header >> 19u) & 3u) == 3;
	const bool   mono       = header.num_channels == 1;
	const size_t side_size  = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
	if (end < side_start + side_size) {
		return;
	}

	size_t       bit_offset = mpeg1 ? (mono ? 18 : 20) : (mono ? 9 : 10);
	const size_t part_size  = mpeg1 ? 59 : 63;
	const auto   parts      = header.num_channels * (mpeg1 ? 2 : 1);
	uint32_t     main_bits  = 0;
	for (uint32_t part = 0; part < parts; ++part, bit_offset += part_size) {
		main_bits += ReadPartLength(data + side_start, bit_offset);
	}

	size_t offset = side_start + side_size + (main_bits + 7u) / 8u;
	if (offset > end) {
		return;
	}
	if (end - offset >= 4 &&
	    (std::memcmp(data + offset, "Xing", 4) == 0 || std::memcmp(data + offset, "Info", 4) == 0 ||
	     std::memcmp(data + offset, "VBRI", 4) == 0)) {
		return;
	}

	for (; end - offset >= 10; ++offset) {
		const auto* record = data + offset;
		if (record[0] != 0xb4) {
			continue;
		}
		if (HasFghChecksum(record)) {
			info.encoder_delay = (static_cast<uint32_t>(record[1]) << 8u) | record[2];
			info.total_samples = AjmReadBe32(record + 3);
			info.ofl_type      = AjmDecMp3OflType::Fgh;
		}
		return;
	}
}

} // namespace

int KYTY_SYSV_ABI AjmDecMp3ParseFrame(const uint8_t* data, uint32_t size, int parse_ofl,
                                      AjmDecMp3FrameInfo* frame_info) {
	AjmMp3FrameHeader header;
	if (frame_info == nullptr || !AjmParseMp3Header(data, size, &header)) {
		return static_cast<int32_t>(0x80930005u);
	}

	AjmDecMp3FrameInfo info {};
	info.frame_size          = header.frame_size;
	info.num_channels        = header.num_channels;
	info.samples_per_channel = header.samples_per_channel;
	info.bitrate             = header.bitrate;
	info.sample_rate         = header.sample_rate;
	if (parse_ofl != 0) {
		ParseOfl(data, size, header, info);
	}
	*frame_info = info;
	return 0;
}

} // namespace Libs::Audio::Ajm
