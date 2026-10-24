// This is the source code of freshGram.
#include "ayu/features/recognition/recognition_audio.h"

#include "ffmpeg/ffmpeg_utility.h"

#include <QtCore/QFile>

#include <atomic>

namespace FreshRecognition {
namespace {

constexpr auto kSampleRate = 16000;
constexpr auto kMaxSeconds = 60 * 60;

struct PacketDeleter {
	void operator()(AVPacket *value) const {
		av_packet_free(&value);
	}
};
using PacketPointer = std::unique_ptr<AVPacket, PacketDeleter>;

int ReadFromFile(void *opaque, uint8_t *buffer, int bufferSize) {
	const auto file = static_cast<QFile*>(opaque);
	const auto read = file->read(reinterpret_cast<char*>(buffer), bufferSize);
	if (read > 0) {
		return int(read);
	}
	return (read == 0) ? AVERROR_EOF : AVERROR_EXTERNAL;
}

int64_t SeekInFile(void *opaque, int64_t offset, int whence) {
	const auto file = static_cast<QFile*>(opaque);
	switch (whence) {
	case AVSEEK_SIZE: return file->size();
	case SEEK_SET: return file->seek(offset) ? file->pos() : -1;
	case SEEK_CUR: return file->seek(file->pos() + offset) ? file->pos() : -1;
	case SEEK_END: return file->seek(file->size() + offset) ? file->pos() : -1;
	}
	return -1;
}

// Appends the resampled data of one decoded frame (or the flushed tail
// of the resampler when the frame is null).
bool AppendFrame(
		SwrContext *resampler,
		const AVFrame *frame,
		std::vector<float> &samples) {
	const auto inSamples = frame ? frame->nb_samples : 0;
	const auto capacity = swr_get_out_samples(resampler, inSamples);
	if (capacity <= 0) {
		return true;
	}
	const auto was = samples.size();
	samples.resize(was + capacity);
	auto out = reinterpret_cast<uint8_t*>(samples.data() + was);
	const auto converted = swr_convert(
		resampler,
		&out,
		capacity,
		frame ? const_cast<const uint8_t**>(frame->extended_data) : nullptr,
		inSamples);
	if (converted < 0) {
		samples.resize(was);
		return false;
	}
	samples.resize(was + converted);
	return true;
}

} // namespace

DecodedAudio DecodeAudioFile(
		const QString &path,
		const std::atomic<bool> &cancelled) {
	auto result = DecodedAudio();
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		result.error = u"Could not open the file."_q;
		return result;
	}
	auto format = FFmpeg::MakeFormatPointer(
		static_cast<void*>(&file),
		&ReadFromFile,
		nullptr,
		&SeekInFile);
	if (!format) {
		result.error = u"Unsupported media format."_q;
		return result;
	}
	if (FFmpeg::AvErrorWrap error = avformat_find_stream_info(
			format.get(),
			nullptr)) {
		result.error = error.text();
		return result;
	}
	const auto index = av_find_best_stream(
		format.get(),
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (index < 0) {
		result.error = u"No audio track."_q;
		return result;
	}
	auto codec = FFmpeg::MakeCodecPointer({
		.stream = format->streams[index],
	});
	if (!codec) {
		result.error = u"Unsupported audio codec."_q;
		return result;
	}

	auto packet = PacketPointer(av_packet_alloc());
	auto frame = FFmpeg::MakeFramePointer();
	auto resampler = FFmpeg::SwresamplePointer();
	auto mono = AVChannelLayout();
	av_channel_layout_default(&mono, 1);
	const auto limit = size_t(kSampleRate) * kMaxSeconds;

	const auto drain = [&]() -> bool {
		while (true) {
			const auto code = avcodec_receive_frame(codec.get(), frame.get());
			if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) {
				return true;
			} else if (code < 0) {
				return false;
			}
			if (!resampler) {
				auto layout = frame->ch_layout;
				if (!av_channel_layout_check(&layout)
					|| layout.nb_channels <= 0) {
					av_channel_layout_default(
						&layout,
						std::max(1, codec->ch_layout.nb_channels));
				}
				resampler = FFmpeg::MakeSwresamplePointer(
					&layout,
					AVSampleFormat(frame->format),
					frame->sample_rate,
					&mono,
					AV_SAMPLE_FMT_FLT,
					kSampleRate);
				if (!resampler) {
					return false;
				}
			}
			if (!AppendFrame(resampler.get(), frame.get(), result.samples)) {
				return false;
			}
			if (result.samples.size() > limit) {
				result.samples.resize(limit);
				return true;
			}
		}
	};

	auto decoded = true;
	while (decoded && av_read_frame(format.get(), packet.get()) >= 0) {
		if (cancelled.load()) {
			result.error = u"Cancelled."_q;
			return result;
		}
		const auto good = (packet->stream_index == index);
		if (good) {
			decoded = (avcodec_send_packet(codec.get(), packet.get()) >= 0)
				&& drain();
		}
		av_packet_unref(packet.get());
		if (result.samples.size() >= limit) {
			break;
		}
	}
	if (decoded && result.samples.size() < limit) {
		avcodec_send_packet(codec.get(), nullptr);
		decoded = drain();
		if (decoded && resampler) {
			decoded = AppendFrame(resampler.get(), nullptr, result.samples);
		}
	}
	if (result.samples.empty()) {
		result.error = u"Could not decode the audio."_q;
	}
	return result;
}

} // namespace FreshRecognition
