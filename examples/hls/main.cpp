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

#include <libwavy/ffmpeg/hls/entry.hpp>

// This will create a singular mp3 file's HLS segments

auto main(int argc, char* argv[]) -> int
{
  logger::init_logging();
  try
  {
    if (argc > 2)
    {
      libwavy::hls::HLS_Segmenter seg;
      seg.create_hls_segments(argv[1], argv[2]);
    }
    else
    {
      LOG_ERROR << argv[0] << " <input-file> " << "<output-dir>";
      return 1;
    }
  }
  catch (const std::exception& e)
  {
    LOG_ERROR << "Something went wrong!";
    return 1;
  }

  return 0;
}
