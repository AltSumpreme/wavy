#pragma once
/************************************************
 * Wavy Project - High-Fidelity Audio Streaming
 * ---------------------------------------------
 * 
 * Copyright (c) 2025 Oinkognito
 * All rights reserved.
 * 
 * This source code is part of the Wavy project, an advanced
 * local networking solution for high-quality audio streaming.
 * 
 * License:
 * This software is licensed under the BSD-3-Clause License.
 * You may use, modify, and distribute this software under
 * the conditions stated in the LICENSE file provided in the
 * project root.
 * 
 * Warranty Disclaimer:
 * This software is provided "AS IS," without any warranties
 * or guarantees, either expressed or implied, including but
 * not limited to fitness for a particular purpose.
 * 
 * Contributions:
 * Contributions to this project are welcome. By submitting 
 * code, you agree to license your contributions under the 
 * same BSD-3-Clause terms.
 * 
 * See LICENSE file for full details.
 ************************************************/

#include <fstream>
#include <iostream>
#include <libwavy/common/macros.hpp>
#include <libwavy/logger.hpp>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

auto DBG_WriteTransportSegmentsToFile(const std::vector<std::string>& transport_segments,
                                      const std::string&              filename) -> bool
{
  std::ofstream output_file(filename, std::ios::binary);
  if (!output_file)
  {
    LOG_ERROR << DECODER_LOG << "Failed to open output file: " << filename;
    return false;
  }

  for (const auto& segment : transport_segments)
  {
    output_file.write(segment.data(), segment.size());
  }

  output_file.close();
  LOG_INFO << DECODER_LOG << "Successfully wrote transport streams to " << filename;
  return true;
}

auto DBG_WriteDecodedAudioToFile(const std::vector<unsigned char>& transport_segment,
                                 const std::string&                filename) -> bool
{
  std::ofstream output_file(filename, std::ios::binary);
  if (!output_file)
  {
    LOG_ERROR << DECODER_LOG << "Failed to open output file: " << filename << std::endl;
    return false;
  }

  output_file.write(reinterpret_cast<const char*>(transport_segment.data()),
                    transport_segment.size());

  output_file.close();
  LOG_INFO << DECODER_LOG << "Successfully wrote decoded audio stream to " << filename << std::endl;
  return true;
}

// Custom AVIO read function
static auto readAVIO(void* opaque, uint8_t* buf, int buf_size) -> int
{
  auto*         segments      = static_cast<std::vector<std::string>*>(opaque);
  static size_t segment_index = 0;
  static size_t read_offset   = 0;

  if (segment_index >= segments->size())
  {
    return AVERROR_EOF; // No more data
  }

  size_t segment_size  = (*segments)[segment_index].size();
  size_t bytes_to_copy = std::min(static_cast<size_t>(buf_size), segment_size - read_offset);

  memcpy(buf, (*segments)[segment_index].data() + read_offset, bytes_to_copy);
  read_offset += bytes_to_copy;

  if (read_offset >= segment_size)
  {
    // Move to the next segment
    segment_index++;
    read_offset = 0;
  }

  return bytes_to_copy;
}

namespace libwavy::ffmpeg
{

/**
 * @class MediaDecoder
 * @brief Decodes transport stream audio for playback from a vector
 */
class MediaDecoder
{
public:
  MediaDecoder() { avformat_network_init(); }

  ~MediaDecoder() { avformat_network_deinit(); }

  auto is_lossless_codec(AVCodecID codec_id) -> bool
  {
    return (codec_id == AV_CODEC_ID_FLAC || codec_id == AV_CODEC_ID_ALAC ||
            codec_id == AV_CODEC_ID_WAVPACK);
  }

  void print_audio_metadata(AVFormatContext* formatCtx, AVCodecParameters* codecParams)
  {
    LOG_DEBUG << DECODER_LOG << "Audio File Metadata:";
    LOG_DEBUG << DECODER_LOG << "Codec: " << avcodec_get_name(codecParams->codec_id);
    LOG_DEBUG << DECODER_LOG << "Bitrate: " << (double)codecParams->bit_rate / 1000.0 << " kbps";
    LOG_DEBUG << DECODER_LOG << "Sample Rate: " << codecParams->sample_rate << " Hz";
    LOG_DEBUG << DECODER_LOG << "Channels: " << codecParams->ch_layout.nb_channels;
    LOG_DEBUG << DECODER_LOG << "Format: " << formatCtx->iformat->long_name;

    LOG_DEBUG << DECODER_LOG
              << (is_lossless_codec(codecParams->codec_id) ? "This is a lossless codec"
                                                           : "This is a lossy codec");
  }

  auto decode(std::vector<std::string>& ts_segments, std::vector<unsigned char>& output_audio)
    -> bool
  {
    AVFormatContext* input_ctx        = nullptr;
    AVCodecContext*  codec_ctx        = nullptr;
    int              audio_stream_idx = -1;

    if (!init_input_context(ts_segments, input_ctx))
      return false;

    detect_format(input_ctx);

    if (!find_audio_stream(input_ctx, audio_stream_idx))
    {
      cleanup(input_ctx);
      return false;
    }

    AVCodecParameters* codec_params = input_ctx->streams[audio_stream_idx]->codecpar;
    print_audio_metadata(input_ctx, codec_params);

    if (!setup_codec(codec_params, codec_ctx))
    {
      cleanup(input_ctx);
      return false;
    }

    if (!process_packets(input_ctx, codec_ctx, audio_stream_idx, codec_params, output_audio))
    {
      cleanup(input_ctx, codec_ctx);
      return false;
    }

    DBG_WriteDecodedAudioToFile(output_audio, "final.pcm");

    cleanup(input_ctx, codec_ctx);
    return true;
  }

private:
  auto init_input_context(std::vector<std::string>& ts_segments, AVFormatContext*& ctx) -> bool
  {
    ctx                   = avformat_alloc_context();
    const size_t avio_buf = 32768;
    auto*        buffer   = static_cast<unsigned char*>(av_malloc(avio_buf));
    if (!buffer)
      return false;

    AVIOContext* avio_ctx =
      avio_alloc_context(buffer, avio_buf, 0, &ts_segments, &readAVIO, nullptr, nullptr);
    if (!avio_ctx)
      return false;

    ctx->pb = avio_ctx;
    if (avformat_open_input(&ctx, nullptr, nullptr, nullptr) < 0)
      return false;
    if (avformat_find_stream_info(ctx, nullptr) < 0)
      return false;

    return true;
  }

  void detect_format(AVFormatContext* ctx)
  {
    std::string fmt = ctx->iformat->name;
    if (fmt == macros::MPEG_TS)
      av_log(nullptr, AV_LOG_DEBUG, "Input is an MPEG transport stream\n");
    else if (fmt.find(macros::MP4_TS) != std::string::npos)
      av_log(nullptr, AV_LOG_DEBUG, "Input is a fragmented MP4 (m4s)\n");
    else
      av_log(nullptr, AV_LOG_WARNING, "Unknown or unsupported format detected\n");
  }

  auto find_audio_stream(AVFormatContext* ctx, int& index) -> bool
  {
    for (unsigned int i = 0; i < ctx->nb_streams; i++)
    {
      if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      {
        index = i;
        return true;
      }
    }
    av_log(nullptr, AV_LOG_ERROR, "Cannot find audio stream\n");
    return false;
  }

  auto setup_codec(AVCodecParameters* codec_params, AVCodecContext*& codec_ctx) -> bool
  {
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec)
      return false;

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx)
      return false;

    if (codec_params->extradata_size > 0)
    {
      codec_ctx->extradata = static_cast<uint8_t*>(
        av_malloc(codec_params->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
      memcpy(codec_ctx->extradata, codec_params->extradata, codec_params->extradata_size);
      codec_ctx->extradata_size = codec_params->extradata_size;
    }

    if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0)
      return false;
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0)
      return false;

    return true;
  }

  auto process_packets(AVFormatContext* ctx, AVCodecContext* codec_ctx, int stream_idx,
                       AVCodecParameters* codec_params, std::vector<unsigned char>& output) -> bool
  {
    AVPacket* packet  = av_packet_alloc();
    AVFrame*  frame   = av_frame_alloc();
    bool      is_flac = (codec_params->codec_id == AV_CODEC_ID_FLAC);

    while (av_read_frame(ctx, packet) >= 0)
    {
      if (packet->stream_index != stream_idx)
      {
        av_packet_unref(packet);
        continue;
      }

      if (!is_flac)
      {
        output.insert(output.end(), packet->data, packet->data + packet->size);
      }
      else
      {
        if (avcodec_send_packet(codec_ctx, packet) == 0)
        {
          while (avcodec_receive_frame(codec_ctx, frame) == 0)
          {
            int    sampleSize = av_get_bytes_per_sample(codec_ctx->sample_fmt);
            size_t dataSize;
            WAVY__SAFE_MULTIPLY(frame->nb_samples, codec_ctx->ch_layout.nb_channels, dataSize);
            WAVY__SAFE_MULTIPLY(dataSize, sampleSize, dataSize);
            output.insert(output.end(), frame->data[0], frame->data[0] + dataSize);
          }
        }
      }

      av_packet_unref(packet);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    return true;
  }

  void cleanup(AVFormatContext* ctx, AVCodecContext* codec_ctx = nullptr)
  {
    if (codec_ctx)
      avcodec_free_context(&codec_ctx);
    if (ctx)
    {
      if (ctx->pb)
        avio_context_free(&ctx->pb);
      avformat_close_input(&ctx);
    }
  }
};

} // namespace libwavy::ffmpeg
