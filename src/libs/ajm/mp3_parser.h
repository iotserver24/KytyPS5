#pragma once

#include <cstddef>
#include <cstdint>

namespace Libs::Audio::Ajm {

struct AjmMp3FrameHeader {
	uint32_t header              = 0;
	uint32_t frame_size          = 0;
	uint32_t num_channels        = 0;
	uint32_t samples_per_channel = 0;
	uint32_t bitrate             = 0;
	uint32_t sample_rate         = 0;
	uint8_t  crc                 = 0;
	uint8_t  mode                = 0;
	uint8_t  mode_extension      = 0;
	uint8_t  copyright           = 0;
	uint8_t  original            = 0;
	uint8_t  emphasis            = 0;
};

inline uint32_t AjmReadBe32(const uint8_t* data) {
	return (static_cast<uint32_t>(data[0]) << 24u) | (static_cast<uint32_t>(data[1]) << 16u) |
	       (static_cast<uint32_t>(data[2]) << 8u) | static_cast<uint32_t>(data[3]);
}

inline bool AjmParseMp3Header(const uint8_t* data, size_t size, AjmMp3FrameHeader* out) {
	if (data == nullptr || size < 4 || out == nullptr) {
		return false;
	}

	const uint32_t header = AjmReadBe32(data);
	if ((header & 0xffe00000u) != 0xffe00000u) {
		return false;
	}

	const auto version_id     = (header >> 19u) & 0x3u;
	const auto layer_id       = (header >> 17u) & 0x3u;
	const auto protection_bit = (header >> 16u) & 0x1u;
	const auto bitrate_index  = (header >> 12u) & 0xfu;
	const auto sample_index   = (header >> 10u) & 0x3u;
	const auto padding        = (header >> 9u) & 0x1u;
	const auto channel_mode   = (header >> 6u) & 0x3u;
	const auto mode_extension = (header >> 4u) & 0x3u;
	const auto copyright      = (header >> 3u) & 0x1u;
	const auto original       = (header >> 2u) & 0x1u;
	const auto emphasis       = header & 0x3u;

	if (version_id == 1 || layer_id != 1 || sample_index == 3 || bitrate_index == 0 ||
	    bitrate_index == 15) {
		return false;
	}

	static constexpr uint32_t SAMPLE_RATES[4][3] = {
	    {11025, 12000, 8000},
	    {0, 0, 0},
	    {22050, 24000, 16000},
	    {44100, 48000, 32000},
	};
	static constexpr uint32_t BITRATES_MPEG1[16] = {
	    0,      32000,  40000,  48000,  56000,  64000,  80000,  96000,
	    112000, 128000, 160000, 192000, 224000, 256000, 320000, 0,
	};
	static constexpr uint32_t BITRATES_MPEG2[16] = {
	    0,     8000,  16000, 24000,  32000,  40000,  48000,  56000,
	    64000, 80000, 96000, 112000, 128000, 144000, 160000, 0,
	};

	const auto sample_rate = SAMPLE_RATES[version_id][sample_index];
	const auto bitrate =
	    (version_id == 3 ? BITRATES_MPEG1[bitrate_index] : BITRATES_MPEG2[bitrate_index]);
	if (sample_rate == 0 || bitrate == 0) {
		return false;
	}

	const bool mpeg1 = (version_id == 3);

	out->header = header;
	out->frame_size =
	    (mpeg1 ? (144u * bitrate) / sample_rate : (72u * bitrate) / sample_rate) + padding;
	out->num_channels        = (channel_mode == 3 ? 1u : 2u);
	out->samples_per_channel = (mpeg1 ? 1152u : 576u);
	out->bitrate             = bitrate;
	out->sample_rate         = sample_rate;
	out->crc                 = static_cast<uint8_t>(protection_bit);
	out->mode                = static_cast<uint8_t>(channel_mode);
	out->mode_extension      = static_cast<uint8_t>(mode_extension);
	out->copyright           = static_cast<uint8_t>(copyright);
	out->original            = static_cast<uint8_t>(original);
	out->emphasis            = static_cast<uint8_t>(emphasis);
	return true;
}

} // namespace Libs::Audio::Ajm
