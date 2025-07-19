#ifndef PACKAGER_MEDIA_FORMATS_MP2T_SCTE35_SECTION_PARSER_H_
#define PACKAGER_MEDIA_FORMATS_MP2T_SCTE35_SECTION_PARSER_H_

#include <functional>
#include <stdint.h>
#include <vector>
#include <memory>

#include "packager/media/base/media_handler.h"
#include "packager/media/formats/mp2t/ts_section_psi.h"
#include "packager/media/chunking/sync_point_queue.h"

namespace shaka {
namespace media {

  // SCTE-35 cue insert Callback.
  typedef std::function<void(std::shared_ptr<CueEvent>)> OnNewCueEventCB;

namespace mp2t {

class Scte35SectionParser : public TsSectionPsi {
 public:
  Scte35SectionParser(const OnNewCueEventCB& cb);
  ~Scte35SectionParser() override;

  // TsSectionPsi 구현
  bool ParsePsiSection(BitReader* bit_reader) override;
  void ResetPsiSection() override;

 private:
  OnNewCueEventCB new_cue_event_cb_;
  double last_pts_ = -1.0;
};

}  // namespace mp2t
}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_FORMATS_MP2T_SCTE35_SECTION_PARSER_H_ 