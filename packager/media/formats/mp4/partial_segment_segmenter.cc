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
      num_segments_(1),
      num_partials_in_seg_(1) {
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

Status PartialSegmentSegmenter::OpenPartialSegmentFile() {
  // Generate partial segment file name
  partial_name_ = GetPartialSegmentName(
      options().segment_template, sidx()->earliest_presentation_time,
      num_segments_, options().bandwidth, num_partials_in_seg_); 

  // Create the segment file
  partial_file_.reset(File::Open(partial_name_.c_str(), "a"));
  if (!partial_file_) {
    return Status(error::FILE_FAILURE,
                  "Cannot open segment file: " + partial_name_);
  }

  is_initial_partial_in_seg_ = false;

  return Status::OK;
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

  OpenPartialSegmentFile();
  if (muxer_listener()) {
    muxer_listener()->OnNewPartialSegmentHint(partial_name_);
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
  is_independent_ |= !key_frame_infos().empty();
  total_partial_size_ += chunk.buffer->Size();

  total_buffered_duration_ += static_cast<double>(chunk.duration) / 
                             static_cast<double>(GetReferenceTimeScale());

  std::unique_ptr<BufferWriter> buffer(new BufferWriter());
  buffer->AppendBuffer(*chunk.buffer);
  RETURN_IF_ERROR(buffer->WriteToFile(partial_file_.get()));
  buffered_chunks_.push_back(std::move(chunk));

  // Update progress
  UpdateProgress(total_buffered_duration_);

  if (total_buffered_duration_ >= options().hls_params.partial_segment_duration) {
    FinalizePartialSegment();
  }

  is_initial_chunk_in_seg_ = false;
  fragment_buffer()->Clear();
  
  return Status::OK;
}

Status PartialSegmentSegmenter::WriteChunk() {
  DCHECK(sidx());
  DCHECK(fragment_buffer());

  if (is_initial_partial_in_seg_) {
    OpenPartialSegmentFile();
    if (muxer_listener()) {
      muxer_listener()->OnNewPartialSegmentHint(partial_name_);
    }
  }

  // Buffer chunk
  ChunkData chunk;
  chunk.buffer.reset(new BufferWriter);
  chunk.buffer->AppendBuffer(*fragment_buffer());
  chunk.duration = GetChunkDuration();
  is_independent_ |= !key_frame_infos().empty();
  total_partial_size_ += chunk.buffer->Size();

  total_buffered_duration_ += static_cast<double>(chunk.duration) / 
                             static_cast<double>(GetReferenceTimeScale());
  RETURN_IF_ERROR(fragment_buffer()->WriteToFile(partial_file_.get()));                             
  buffered_chunks_.push_back(std::move(chunk));  
  
  // Update progress
  UpdateProgress(total_buffered_duration_);

  // Create partial segment when duration threshold is reached
  if (total_buffered_duration_ >= options().hls_params.partial_segment_duration) {
    FinalizePartialSegment();
  }

  return Status::OK;
}

Status PartialSegmentSegmenter::FinalizePartialSegment() {
  if (!partial_file_.release()->Close()) {
    return Status(
        error::FILE_FAILURE,
        "Cannot close file " + partial_name_ +
            ", possibly file permission issue or running out of disk space.");
  }
  
  if (muxer_listener()) {
    muxer_listener()->OnNewPartialSegment(partial_name_,
                                          sidx()->earliest_presentation_time,
                                          total_buffered_duration_, 
                                          total_partial_size_, 
                                          is_independent_);
  }

  // Reset buffer state
  key_frame_infos_clear();
  ResetPartialState();
  num_partials_in_seg_++;

  return Status::OK;
} 

Status PartialSegmentSegmenter::FinalizeSegment() {
  if (partial_file_) {
    FinalizePartialSegment();
  }
  
  // Create complete segment from partials
  RETURN_IF_ERROR(WriteSegmentFile());

  if (muxer_listener()) {
    muxer_listener()->OnNewSegment(
        file_name_,
        sidx()->earliest_presentation_time,
        GetSegmentDuration(),
        total_segment_size_,
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
  for (const auto& chunk : buffered_chunks_) {
    total_segment_size_ += chunk.buffer->Size();
    RETURN_IF_ERROR(chunk.buffer->WriteToFile(segment_file.get()));   
  }
  if (!segment_file.release()->Close()) {
    return Status(error::FILE_FAILURE,
                  "Cannot close segment: " + file_name_);
  }

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

void PartialSegmentSegmenter::ResetPartialState() {
  is_initial_chunk_in_seg_ = true;
  is_initial_partial_in_seg_ = true;
  total_buffered_duration_ = 0;
  total_partial_size_ = 0;
  is_independent_ = false;
}

void PartialSegmentSegmenter::ResetSegmentState() {
  ResetPartialState();
  num_partials_in_seg_ = 1;
  total_segment_size_ = 0;
  buffered_chunks_.clear();
}

}  // namespace mp4
}  // namespace media
}  // namespace shaka
