#include "libs/audio.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {

namespace Ajm = Libs::Audio::Ajm;
using Info    = Ajm::AjmDecMp3FrameInfo;
using Ofl     = Ajm::AjmDecMp3OflType;
using Bytes   = std::vector<uint8_t>;

#define CHECK(expression)                                                                          \
	do {                                                                                           \
		if (!(expression)) {                                                                       \
			std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                  \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

static_assert(sizeof(Info) == 40);
static_assert(offsetof(Info, frame_size) == 0);
static_assert(offsetof(Info, num_channels) == 8);
static_assert(offsetof(Info, samples_per_channel) == 12);
static_assert(offsetof(Info, bitrate) == 16);
static_assert(offsetof(Info, sample_rate) == 20);
static_assert(offsetof(Info, encoder_delay) == 24);
static_assert(offsetof(Info, num_frames) == 28);
static_assert(offsetof(Info, total_samples) == 32);
static_assert(offsetof(Info, ofl_type) == 36);
static_assert(sizeof(Ofl) == 4 && static_cast<uint32_t>(Ofl::Fgh) == 3);

constexpr uint32_t                kInvalid = 0x80930005u;
constexpr Info                    kNormal {417, 2, 1152, 128000, 44100, 0, 0, 0, Ofl::None};
constexpr std::array<uint8_t, 10> kFgh {0xb4, 0x02, 0x40, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x54};

struct Result {
	int  status;
	Info info;
};

Result Parse(const uint8_t* data, size_t size, int flag, int poison = 0xa5) {
	struct {
		uint64_t before;
		Info     info;
		uint64_t after;
	} output {0x0123456789abcdefull, {}, 0xfedcba9876543210ull};
	std::memset(&output.info, poison, sizeof(output.info));
	const int status =
	    Ajm::AjmDecMp3ParseFrame(data, static_cast<uint32_t>(size), flag, &output.info);
	CHECK(output.before == 0x0123456789abcdefull);
	CHECK(output.after == 0xfedcba9876543210ull);
	CHECK(status == 0 || static_cast<uint32_t>(status) == kInvalid);
	return {status, output.info};
}

Result Parse(const Bytes& data, int flag, int poison = 0xa5) {
	return Parse(data.data(), data.size(), flag, poison);
}

void CheckFrame(const Result& result, Info expected, bool fgh = false) {
	if (fgh) {
		expected.encoder_delay = 576;
		expected.total_samples = 0x12345678;
		expected.ofl_type      = Ofl::Fgh;
	}
	CHECK(result.status == 0);
	CHECK(result.info.frame_size == expected.frame_size);
	CHECK(result.info.num_channels == expected.num_channels);
	CHECK(result.info.samples_per_channel == expected.samples_per_channel);
	CHECK(result.info.bitrate == expected.bitrate);
	CHECK(result.info.sample_rate == expected.sample_rate);
	CHECK(result.info.encoder_delay == expected.encoder_delay);
	CHECK(result.info.num_frames == expected.num_frames);
	CHECK(result.info.total_samples == expected.total_samples);
	CHECK(result.info.ofl_type == expected.ofl_type);
}

Bytes MakeFrame(uint32_t header = 0xfffb9000u, size_t size = 4) {
	CHECK(size >= 4);
	Bytes data(size, 0);
	for (size_t i = 0; i < 4; ++i) {
		data[i] = static_cast<uint8_t>(header >> (24 - i * 8));
	}
	return data;
}

void PutFgh(Bytes& data, size_t offset) {
	CHECK(offset <= data.size() && data.size() - offset >= kFgh.size());
	std::copy(kFgh.begin(), kFgh.end(), data.begin() + offset);
}

void SetPartLength(Bytes& data, size_t bit_offset, uint32_t length) {
	CHECK(length < 4096 && bit_offset + 12 <= data.size() * 8);
	for (size_t i = 0; i < 12; ++i) {
		const size_t bit  = bit_offset + i;
		const auto   mask = static_cast<uint8_t>(1u << (7 - bit % 8));
		data[bit / 8]     = static_cast<uint8_t>((data[bit / 8] & ~mask) |
		                                         (((length >> (11 - i)) & 1u) ? mask : 0));
	}
}

void CheckTruncations(const Bytes& frame, const Info& expected, size_t fgh_end) {
	for (size_t size = 0; size <= frame.size(); ++size) {
		const Bytes data(frame.begin(), frame.begin() + size);
		for (int flag: {0, 1}) {
			const auto result = Parse(data, flag);
			if (size < 4) {
				CHECK(static_cast<uint32_t>(result.status) == kInvalid);
			} else {
				CheckFrame(result, expected, flag != 0 && size >= fgh_end);
			}
		}
	}
}

void TestValidationAndAlignment() {
	const auto header = MakeFrame();
	for (int flag: {0, 1, -7}) {
		CHECK(static_cast<uint32_t>(Ajm::AjmDecMp3ParseFrame(header.data(), 4, flag, nullptr)) ==
		      kInvalid);
		CHECK(static_cast<uint32_t>(Ajm::AjmDecMp3ParseFrame(nullptr, 0, flag, nullptr)) ==
		      kInvalid);
		for (uint32_t size: {0u, 4u, UINT32_MAX}) {
			CHECK(static_cast<uint32_t>(Parse(nullptr, size, flag).status) == kInvalid);
		}
		for (uint32_t invalid: {0u, 0x7ffb9000u, 0xffdb9000u, 0xffeb9000u, 0xfff99000u, 0xfffd9000u,
		                        0xffff9000u, 0xfffb0000u, 0xfffbf000u, 0xfffb9c00u}) {
			CHECK(static_cast<uint32_t>(Parse(MakeFrame(invalid), flag).status) == kInvalid);
		}
		for (Bytes prefix: {Bytes {0}, Bytes {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0}}) {
			prefix.insert(prefix.end(), header.begin(), header.end());
			CHECK(static_cast<uint32_t>(Parse(prefix, flag).status) == kInvalid);
		}
	}
	CheckTruncations(MakeFrame(0xfffb9000u, 417), kNormal, 418);
	auto padded       = kNormal;
	padded.frame_size = 418;
	CheckFrame(Parse(MakeFrame(0xfffb9200u), 1), padded);
	auto frame = MakeFrame(0xfffb9000u, 417);
	PutFgh(frame, 36);
	for (size_t offset: {1u, 2u, 3u, 7u}) {
		Bytes unaligned(frame.size() + offset, 0);
		std::copy(frame.begin(), frame.end(), unaligned.begin() + offset);
		CheckFrame(Parse(unaligned.data() + offset, frame.size(), 1), kNormal, true);
	}
}

void TestHeaderTables() {
	constexpr uint32_t versions[] {3, 2, 0};
	constexpr uint32_t rates[][3] {
	    {44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
	constexpr uint32_t kbps[][14] {{32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
	                               {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
	for (size_t version = 0; version < 3; ++version) {
		for (uint32_t rate = 0; rate < 3; ++rate) {
			for (uint32_t index = 1; index <= 14; ++index) {
				for (uint32_t options = 0; options < 16; ++options) {
					const uint32_t mode        = options & 3u;
					const uint32_t padding     = (options >> 2u) & 1u;
					const uint32_t protection  = options >> 3u;
					const uint32_t bitrate     = kbps[version == 0 ? 0 : 1][index - 1] * 1000;
					const uint32_t samples     = version == 0 ? 1152 : 576;
					const uint32_t sample_rate = rates[version][rate];
					const uint32_t header = 0xffe20000u | (versions[version] << 19u) |
					                        (protection << 16u) | (index << 12u) | (rate << 10u) |
					                        (padding << 9u) | (mode << 6u);
					const Info     expected {uint64_t {bitrate} * samples / (sample_rate * 8u) +
					                             padding,
					                         mode == 3 ? 1u : 2u,
					                         samples,
					                         bitrate,
					                         sample_rate,
					                         0,
					                         0,
					                         0,
					                         Ofl::None};
					for (size_t size: {4u, 64u}) {
						for (int flag: {0, 1}) {
							CheckFrame(Parse(MakeFrame(header, size), flag), expected);
						}
					}
				}
			}
		}
	}
}

void TestOflLayoutsAndBounds() {
	struct Layout {
		uint32_t header;
		Info     expected;
		size_t   side_size, part_offset, part_stride, parts;
	};
	const Layout layouts[] {
	    {0xfffb9000u, kNormal, 32, 20, 59, 4},
	    {0xfffb90c0u, {417, 1, 1152, 128000, 44100, 0, 0, 0, Ofl::None}, 17, 18, 59, 2},
	    {0xfff39000u, {261, 2, 576, 80000, 22050, 0, 0, 0, Ofl::None}, 17, 10, 63, 2},
	    {0xfff390c0u, {261, 1, 576, 80000, 22050, 0, 0, 0, Ofl::None}, 9, 9, 63, 1},
	    {0xffe39000u, {522, 2, 576, 80000, 11025, 0, 0, 0, Ofl::None}, 17, 10, 63, 2},
	    {0xffe390c0u, {522, 1, 576, 80000, 11025, 0, 0, 0, Ofl::None}, 9, 9, 63, 1}};
	for (const auto& layout: layouts) {
		for (bool crc: {false, true}) {
			const uint32_t header     = layout.header & (crc ? ~0x10000u : ~0u);
			const size_t   end        = layout.expected.frame_size;
			const size_t   side_start = crc ? 6 : 4;
			const size_t   ancillary  = side_start + layout.side_size;
			auto           make       = [&](size_t size) {
				auto data = MakeFrame(header, size);
				if (crc) {
					data[4] = 0xb4;
					data[5] = 0x80;
				}
				data[ancillary - 1] = 0xb4;
				return data;
			};
			for (size_t position: {ancillary, end - kFgh.size()}) {
				auto data = make(end);
				PutFgh(data, position);
				CheckTruncations(data, layout.expected, position + kFgh.size());
				for (int flag: {1, 2, -7}) {
					CheckFrame(Parse(data, flag), layout.expected, true);
				}
			}
			for (size_t position: {end - 9, end, end + ancillary}) {
				auto data = make(end * 2);
				std::copy_n(data.begin(), 4, data.begin() + end);
				PutFgh(data, position);
				CheckFrame(Parse(data, 1), layout.expected);
			}
			auto     nonbyte   = make(end);
			uint32_t main_bits = 0;
			for (size_t part = 0; part < layout.parts; ++part) {
				const auto length = static_cast<uint32_t>(9 + 8 * part);
				SetPartLength(nonbyte,
				              side_start * 8 + layout.part_offset + part * layout.part_stride,
				              length);
				main_bits += length;
			}
			const size_t record = ancillary + (main_bits + 7) / 8;
			std::fill(nonbyte.begin() + ancillary, nonbyte.begin() + record, 0xb4);
			PutFgh(nonbyte, record);
			CheckTruncations(nonbyte, layout.expected, record + kFgh.size());
			auto overlong = make(4096);
			for (size_t part = 0; part < layout.parts; ++part) {
				SetPartLength(overlong,
				              side_start * 8 + layout.part_offset + part * layout.part_stride,
				              4095);
			}
			PutFgh(overlong, ancillary + (4095 * layout.parts + 7) / 8);
			CheckFrame(Parse(overlong, 1), layout.expected);
			CheckTruncations(Bytes(overlong.begin(), overlong.begin() + end), layout.expected,
			                 end + 1);
		}
	}
}

void TestUnsupportedTagsAndFirstRecord() {
	for (const char* tag: {"Xing", "Info", "VBRI"}) {
		auto data = MakeFrame(0xfffb9000u, 64);
		std::memcpy(data.data() + 36, tag, 4);
		PutFgh(data, 40);
		CheckTruncations(data, kNormal, data.size() + 1);
	}
	for (size_t corrupt = 1; corrupt < kFgh.size(); ++corrupt) {
		auto data = MakeFrame(0xfffb9000u, 64);
		PutFgh(data, 36);
		data[36 + corrupt] ^= 1;
		PutFgh(data, 46);
		CheckTruncations(data, kNormal, data.size() + 1);
	}
}

void TestRandomBoundedInputs() {
	std::mt19937 random(0x4d503346u);
	for (size_t iteration = 0; iteration < 8192; ++iteration) {
		Bytes data(iteration < 256 ? iteration : random() % 4097);
		for (auto& byte: data) {
			byte = static_cast<uint8_t>(random());
		}
		if (data.size() >= 4 && iteration % 2 == 0) {
			data[0] = 0xff;
			data[1] |= 0xe0;
		}
		const Bytes original = data;
		const int   flag     = static_cast<int>(iteration % 3) - 1;
		const auto  first    = Parse(data, flag);
		const auto  second   = Parse(data, flag, 0x5a);
		CHECK(data == original);
		CHECK(first.status == second.status);
		if (first.status == 0) {
			CHECK(std::memcmp(&first.info, &second.info, sizeof(Info)) == 0);
			CHECK(first.info.frame_size >= 24 && first.info.frame_size <= 1441);
			CHECK(first.info.num_channels == 1 || first.info.num_channels == 2);
			CHECK(first.info.samples_per_channel == 576 || first.info.samples_per_channel == 1152);
			CHECK(first.info.bitrate >= 8000 && first.info.bitrate <= 320000);
			CHECK(first.info.sample_rate >= 8000 && first.info.sample_rate <= 48000);
			CHECK(first.info.num_frames == 0);
			CHECK(first.info.ofl_type == Ofl::None || first.info.ofl_type == Ofl::Fgh);
			CHECK(flag != 0 || first.info.ofl_type == Ofl::None);
			if (first.info.ofl_type == Ofl::None) {
				CHECK(first.info.encoder_delay == 0 && first.info.total_samples == 0);
			} else {
				CHECK(first.info.encoder_delay <= 65535);
			}
		}
	}
}

} // namespace

int main() {
	TestValidationAndAlignment();
	TestHeaderTables();
	TestOflLayoutsAndBounds();
	TestUnsupportedTagsAndFirstRecord();
	TestRandomBoundedInputs();
	std::puts("AjmMp3ParserTests: all cases passed");
	return 0;
}
