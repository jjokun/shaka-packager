// Copyright 2025 Samsung Co.Ltd. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_FORMATS_MP4_PARTIAL_SEGMENT_SEGMENTER_H_
#define PACKAGER_MEDIA_FORMATS_MP4_PARTIAL_SEGMENT_SEGMENTER_H_

#include <packager/file.h>
#include <packager/file/file_closer.h>
#include <packager/macros/classes.h>
#include <packager/media/formats/mp4/segmenter.h>

namespace shaka {
namespace media {
namespace mp4 {

struct SegmentType;

struct ChunkData {
  std::unique_ptr<BufferWriter> buffer;
  uint64_t duration = 0;
};

/// Segmenter for LL-HLS profiles.
/// Each segment constist of partial segments, and each segment contains one
/// chunk. A chunk is the smallest unit and is constructed of a single moof and
/// mdat atom. A chunk is be generated for each recieved @b MediaSample. The
/// generated chunks are written as they are created to files defined by
/// @b MuxerOptions.segment_template if specified; otherwise, the chunks are
/// appended to the main output file specified by @b
/// MuxerOptions.output_file_name.
class PartialSegmentSegmenter : public Segmenter {
 public:
  PartialSegmentSegmenter(const MuxerOptions& options,
                          std::unique_ptr<FileType> ftyp,
                          std::unique_ptr<Movie> moov);
  ~PartialSegmentSegmenter() override;

  /// @name Segmenter implementation overrides.
  /// @{
  bool GetInitRange(size_t* offset, size_t* size) override;
  bool GetIndexRange(size_t* offset, size_t* size) override;
  std::vector<Range> GetSegmentRanges() override;
  /// @}

 private:
  // Segmenter implementation overrides.
  Status DoInitialize() override;
  Status DoFinalize() override;
  Status DoFinalizeSegment(int64_t segment_number) override;
  Status DoFinalizeChunk(int64_t segment_number) override;

  // Write segment to file.
  Status WriteInitSegment();
  Status WriteChunk();
  Status WriteInitialChunk(int64_t segment_number);
  Status FinalizeSegment();
  Status FinalizePartialSegment(const std::string& partial_name,
                                uint64_t earliest_presentation_time,
                                double duration,
                                uint64_t size,
                                bool is_independent);

  uint64_t GetSegmentDuration();

  std::unique_ptr<SegmentType> styp_;
  uint32_t num_segments_;
  uint32_t num_partials_in_seg_;
  bool is_initial_chunk_in_seg_ = true;
  bool ll_hls_m3u8_values_initialized_ = false;
  std::unique_ptr<File, FileCloser> partial_file_;
  std::string file_name_;
  std::string partial_name_;
  uint64_t total_partial_size_ = 0;         // Total size of the current partial segment.
  uint64_t total_segment_size_ = 0;      // Total size of the current segment.
  size_t segment_size_ = 0u;
  uint32_t num_chunks_in_seg_ = 0;  // Number of chunks in the current segment.
  bool is_independent_ = false;  // Is the current partial segment independent.

  std::vector<ChunkData> buffered_chunks_;
  double total_buffered_duration_ = 0;

  Status WriteSegmentFile();
  
  uint64_t GetChunkDuration();

  void ResetSegmentState();

  DISALLOW_COPY_AND_ASSIGN(PartialSegmentSegmenter);
};

}  // namespace mp4
}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_FORMATS_MP4_PARTIAL_SEGMENT_SEGMENTER_H_