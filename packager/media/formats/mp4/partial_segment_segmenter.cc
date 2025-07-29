// Copyright 2025 Samsung Co.Ltd. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/formats/mp4/partial_segment_segmenter.h>

#include <algorithm>

#include <absl/log/check.h>

#include <packager/file.h>
#include <packager/file/file_closer.h>
#include <packager/macros/logging.h>
#include <packager/macros/status.h>
#include <packager/media/base/buffer_writer.h>
#include <packager/media/base/media_handler.h>
#include <packager/media/base/muxer_options.h>
#include <packager/media/base/muxer_util.h>
#include <packager/media/event/muxer_listener.h>
#include <packager/media/formats/mp4/box_definitions.h>
#include <packager/media/formats/mp4/fragmenter.h>
#include <packager/media/formats/mp4/key_frame_info.h>

namespace shaka {
namespace media {
namespace mp4 {

PartialSegmentSegmenter::PartialSegmentSegmenter(
    const MuxerOptions& options,
    std::unique_ptr<FileType> ftyp,
    std::unique_ptr<Movie> moov)
    : Segmenter(options, std::move(ftyp), std::move(moov)),
      styp_(new SegmentType),
      num_segments_(0),
      num_partials_in_seg_(0) {
  // Use the same brands for styp as ftyp.
  styp_->major_brand = Segmenter::ftyp()->major_brand;
  styp_->compatible_brands = Segmenter::ftyp()->compatible_brands;
  // Replace 'cmfc' with 'cmfs' for CMAF segments compatibility.
  std::replace(styp_->compatible_brands.begin(), styp_->compatible_brands.end(),
               FOURCC_cmfc, FOURCC_cmfs);
}

PartialSegmentSegmenter::~PartialSegmentSegmenter() {}

bool PartialSegmentSegmenter::GetInitRange(size_t* offset, size_t* size) {
  VLOG(1) << "PartialSegmentSegmenter outputs init segment: "
          << options().output_file_name;
  return false;
}

bool PartialSegmentSegmenter::GetIndexRange(size_t* offset, size_t* size) {
  VLOG(1) << "PartialSegmentSegmenter does not have index range.";
  return false;
}

std::vector<Range> PartialSegmentSegmenter::GetSegmentRanges() {
  VLOG(1) << "PartialSegmentSegmenter does not have media segment ranges.";
  return std::vector<Range>();
}

Status PartialSegmentSegmenter::DoInitialize() {
  return WriteInitSegment();
}

Status PartialSegmentSegmenter::DoFinalize() {
  // Update init segment with media duration set.
  RETURN_IF_ERROR(WriteInitSegment());
  SetComplete();
  return Status::OK;
}

Status PartialSegmentSegmenter::DoFinalizeSegment(int64_t segment_number) {
  return FinalizeSegment();
}

Status PartialSegmentSegmenter::DoFinalizeChunk(int64_t segment_number) {
  if (is_initial_chunk_in_seg_) {
    return WriteInitialChunk(segment_number);
  }
  return WriteChunk();
}

Status PartialSegmentSegmenter::WriteInitSegment() {
  DCHECK(ftyp());
  DCHECK(moov());
  // Generate the output file with init segment.
  std::unique_ptr<File, FileCloser> file(
      File::Open(options().output_file_name.c_str(), "w"));
  if (!file) {
    return Status(error::FILE_FAILURE,
                  "Cannot open file for write " + options().output_file_name);
  }
  std::unique_ptr<BufferWriter> buffer(new BufferWriter);
  ftyp()->Write(buffer.get());
  moov()->Write(buffer.get());
  return buffer->WriteToFile(file.get());
}

Status PartialSegmentSegmenter::WriteInitialChunk(int64_t segment_number) {
  DCHECK(sidx());
  DCHECK(fragment_buffer());
  DCHECK(styp_);
  DCHECK(!sidx()->references.empty());

  // Set presentation time
  sidx()->earliest_presentation_time =
      sidx()->references[0].earliest_presentation_time;

  // Generate segment file name (will be created at FinalizeSegment)
  if (options().segment_template.empty()) {
    file_name_ = options().output_file_name.c_str();
  } else {
    file_name_ = GetSegmentName(options().segment_template,
                               sidx()->earliest_presentation_time,
                               num_segments_, options().bandwidth);
  }

  // Initialize first partial segment
  ChunkData chunk;
  chunk.buffer.reset(new BufferWriter);
  // Write the styp header to the beginning of the segment.
  styp_->Write(chunk.buffer.get());

  const size_t segment_header_size = chunk.buffer->Size();
  segment_size_ = segment_header_size + fragment_buffer()->Size();
  DCHECK_NE(segment_size_, 0u);
  if (muxer_listener()) {
    for (const KeyFrameInfo& key_frame_info : key_frame_infos()) {
      muxer_listener()->OnKeyFrame(
          key_frame_info.timestamp,
          segment_header_size + key_frame_info.start_byte_offset,
          key_frame_info.size);
    }
  }

  chunk.buffer->AppendBuffer(*fragment_buffer());
  chunk.duration = GetChunkDuration();
  chunk.is_independent = !key_frame_infos().empty();

  total_buffered_duration_ += static_cast<double>(chunk.duration) / 
                             static_cast<double>(GetReferenceTimeScale());
  buffered_chunks_.push_back(std::move(chunk));
  fragment_buffer()->Clear();

  if (total_buffered_duration_ >= options().hls_params.partial_segment_duration) {
    RETURN_IF_ERROR(WritePartialSegment());
  }

  is_initial_chunk_in_seg_ = false;
  
  return Status::OK;
}

Status PartialSegmentSegmenter::WriteChunk() {
  DCHECK(fragment_buffer());

  // Buffer chunk
  ChunkData chunk;
  chunk.buffer.reset(new BufferWriter);
  chunk.buffer->AppendBuffer(*fragment_buffer());
  chunk.duration = GetChunkDuration();
  chunk.is_independent = !key_frame_infos().empty();

  total_buffered_duration_ += static_cast<double>(chunk.duration) / 
                             static_cast<double>(GetReferenceTimeScale());
  buffered_chunks_.push_back(std::move(chunk));
  fragment_buffer()->Clear();

  // Create partial segment when duration threshold is reached
  if (total_buffered_duration_ >= options().hls_params.partial_segment_duration) {
    RETURN_IF_ERROR(WritePartialSegment());
  }

  return Status::OK;
}

Status PartialSegmentSegmenter::WritePartialSegment() {
  if (buffered_chunks_.empty()) {
    return Status::OK;
  }

  // Generate partial segment file name
  std::string partial_name;
  const size_t extension_pos = file_name_.find_last_of('.');
  if (extension_pos != std::string::npos) {
    partial_name = file_name_.substr(0, extension_pos) + 
                   ".part" + std::to_string(num_partials_in_seg_) +
                   file_name_.substr(extension_pos);
  } else {
    partial_name = file_name_ + ".part" + std::to_string(num_partials_in_seg_);
  }
  
  // Create partial segment file
  std::unique_ptr<File, FileCloser> partial_file(
      File::Open(partial_name.c_str(), "a"));
  if (!partial_file) {
    return Status(error::FILE_FAILURE,
                  "Cannot create partial segment: " + partial_name);
  }

  // Write all buffered chunks
  uint64_t partial_size = 0;
  bool is_independent = false;
  for (const auto& chunk : buffered_chunks_) {
    partial_size += chunk.buffer->Size();
    is_independent |= chunk.is_independent;
    RETURN_IF_ERROR(chunk.buffer->WriteToFile(partial_file.get()));    
  }

  if (!partial_file.release()->Close()) {
    return Status(error::FILE_FAILURE,
                  "Cannot close partial segment: " + partial_name);
  }

  // Update state
  partial_files_.push_back(partial_name);
  total_partial_size_ += partial_size;
  num_partials_in_seg_++;

  // Update progress
  UpdateProgress(total_buffered_duration_);

  if (muxer_listener()) {
    muxer_listener()->OnNewPartialSegment(partial_name,
                                          total_buffered_duration_,
                                          partial_size,
                                          is_independent);
  }

  // Reset buffer state
  buffered_chunks_.clear();
  key_frame_infos_clear();
  total_buffered_duration_ = 0;

  return Status::OK;
}

Status PartialSegmentSegmenter::FinalizeSegment() {
  // Write remaining chunks as partial segment
  if (!buffered_chunks_.empty()) {
    RETURN_IF_ERROR(WritePartialSegment());
  }

  // Create complete segment from partials
  RETURN_IF_ERROR(WriteSegmentFile());

  if (muxer_listener()) {
    muxer_listener()->OnNewSegment(
        file_name_,
        sidx()->earliest_presentation_time,
        GetSegmentDuration(),
        total_partial_size_,
        num_segments_);
  }

  // Cleanup
  // CleanupPartialSegments();
  ResetSegmentState();
  num_segments_++;

  return Status::OK;
}

Status PartialSegmentSegmenter::WriteSegmentFile() {
  // Create segment file
  std::unique_ptr<File, FileCloser> segment_file(
      File::Open(file_name_.c_str(), "w"));
  if (!segment_file) {
    return Status(error::FILE_FAILURE,
                  "Cannot create segment file: " + file_name_);
  }

  // Copy all partial segments
  for (const auto& partial : partial_files_) {
    std::unique_ptr<File, FileCloser> partial_file(
        File::Open(partial.c_str(), "r"));
    if (!partial_file) {
      return Status(error::FILE_FAILURE,
                    "Cannot open partial file: " + partial);
    }

    std::vector<uint8_t> buffer(1024 * 1024);  // 1MB buffer
    while (true) {
      int64_t bytes_read = partial_file->Read(buffer.data(), buffer.size());
      if (bytes_read < 0) {
        return Status(error::FILE_FAILURE,
                      "Error reading partial file: " + partial);
      }
      if (bytes_read == 0) break;

      if (segment_file->Write(buffer.data(), bytes_read) != bytes_read) {
        return Status(error::FILE_FAILURE,
                      "Error writing to segment file: " + file_name_);
      }
    }
    partial_file.release()->Close();
  }
  segment_file.release()->Close();

  return Status::OK;
}

uint64_t PartialSegmentSegmenter::GetSegmentDuration() {
  DCHECK(sidx());

  uint64_t segment_duration = 0;
  // ISO/IEC 23009-1:2012: the value shall be identical to sum of the the
  // values of all Subsegment_duration fields in the first 'idx' box.
  for (size_t i = 0; i < sidx()->references.size(); ++i)
    segment_duration += sidx()->references[i].subsegment_duration;

  return segment_duration;
}

uint64_t PartialSegmentSegmenter::GetChunkDuration() {
  DCHECK(sidx());
  if (sidx()->references.empty())
    return 0;
  
  return sidx()->references.back().subsegment_duration;
}

void PartialSegmentSegmenter::CleanupPartialSegments() {
  for (const auto& partial : partial_files_) {
    File::Delete(partial.c_str());
  }
}

void PartialSegmentSegmenter::ResetSegmentState() {
  is_initial_chunk_in_seg_ = true;
  num_partials_in_seg_ = 0;
  total_partial_size_ = 0;
  partial_files_.clear();
  buffered_chunks_.clear();
  total_buffered_duration_ = 0;
}

}  // namespace mp4
}  // namespace media
}  // namespace shaka
