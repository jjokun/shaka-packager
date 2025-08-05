#include "packager/media/formats/mp2t/scte35_section_parser.h"
#include "packager/media/base/bit_reader.h"
#include <packager/media/base/rcheck.h>
#include <cstring>

namespace shaka {
namespace media {
namespace mp2t {

Scte35SectionParser::Scte35SectionParser(const OnNewCueEventCB& cb)
    : new_cue_event_cb_(cb) {}
Scte35SectionParser::~Scte35SectionParser() {}


bool Scte35SectionParser::ParsePsiSection(BitReader* bit_reader) {
    int table_id = 0;
    RCHECK(bit_reader->ReadBits(8, &table_id));
    if (table_id != 0xFC) return false;

    int section_syntax_indicator = 0, private_indicator = 0, reserved = 0, section_length = 0;
    RCHECK(bit_reader->ReadBits(1, &section_syntax_indicator));
    RCHECK(bit_reader->ReadBits(1, &private_indicator));
    RCHECK(bit_reader->ReadBits(2, &reserved));
    RCHECK(bit_reader->ReadBits(12, &section_length));

    int protocol_version = 0;
    RCHECK(bit_reader->ReadBits(8, &protocol_version));

    int encrypted_packet = 0;
    RCHECK(bit_reader->ReadBits(1, &encrypted_packet));
    int encryption_algorithm = 0;
    RCHECK(bit_reader->ReadBits(6, &encryption_algorithm));
    uint64_t pts_adjustment = 0;
    RCHECK(bit_reader->ReadBits(33, &pts_adjustment));
    int cw_index = 0;
    RCHECK(bit_reader->ReadBits(8, &cw_index));
    int tier = 0;
    RCHECK(bit_reader->ReadBits(12, &tier));
    int splice_command_length = 0;
    RCHECK(bit_reader->ReadBits(12, &splice_command_length));
    int splice_command_type = 0;
    RCHECK(bit_reader->ReadBits(8, &splice_command_type));

    LOG(INFO) << "SCTE-35 splice_command_type: " << std::hex << splice_command_type;

    if (splice_command_type == 0x05) { // splice_insert
        uint32_t splice_event_id = 0;
        RCHECK(bit_reader->ReadBits(32, &splice_event_id));
        int cancel = 0;
        RCHECK(bit_reader->ReadBits(1, &cancel));
        RCHECK(bit_reader->ReadBits(7, &reserved));

        bool out_of_network = false;
        double break_duration = 0.0;
        double splice_time = 0.0;
        bool has_splice_time = false;

        if (!cancel) {
            int out_of_network_indicator = 0, program_splice_flag = 0, duration_flag = 0, splice_immediate_flag = 0;
            RCHECK(bit_reader->ReadBits(1, &out_of_network_indicator));
            RCHECK(bit_reader->ReadBits(1, &program_splice_flag));
            RCHECK(bit_reader->ReadBits(1, &duration_flag));
            RCHECK(bit_reader->ReadBits(1, &splice_immediate_flag));
            RCHECK(bit_reader->ReadBits(4, &reserved));
            out_of_network = out_of_network_indicator;

            // splice_time() 파싱
            if (program_splice_flag && !splice_immediate_flag) {
                int time_specified_flag = 0;
                RCHECK(bit_reader->ReadBits(1, &time_specified_flag));
                if (time_specified_flag) {
                    RCHECK(bit_reader->ReadBits(6, &reserved));
                    RCHECK(bit_reader->ReadBits(33, &splice_time));
                    has_splice_time = true;
                } else {
                    RCHECK(bit_reader->ReadBits(7, &reserved));
                }
            }

            // (program_splice_flag, splice_immediate_flag 등은 필요시 추가 파싱)
            if (duration_flag) {
                int auto_return = 0;
                RCHECK(bit_reader->ReadBits(1, &auto_return));
                RCHECK(bit_reader->ReadBits(6, &reserved));
                uint64_t duration90k = 0;
                RCHECK(bit_reader->ReadBits(33, &duration90k));
                break_duration = duration90k / 90000.0;
            }
        }

        // descriptor, CRC skip
        int descriptor_loop_length = 0;
        RCHECK(bit_reader->ReadBits(16, &descriptor_loop_length));
        for (int i = 0; i < descriptor_loop_length; ++i) {
            int byte = 0;
            RCHECK(bit_reader->ReadBits(8, &byte));
        }
        int crc32 = 0;
        RCHECK(bit_reader->ReadBits(32, &crc32));

        // 콜백 호출
        if (new_cue_event_cb_) {
            double pts_to_use = has_splice_time ? splice_time : last_pts_;            
            
            auto cue_event = std::make_shared<CueEvent>();
            cue_event->type = out_of_network ? CueEventType::kCueOut : CueEventType::kCueIn;
            cue_event->time_in_seconds = pts_to_use;
            cue_event->break_duration = break_duration;
            cue_event->out_of_network = out_of_network;
            // SCTE-35 정보를 문자열로 조합
            std::ostringstream oss;
            oss << "CueEvent {";
            oss << "pts=" << static_cast<uint64_t>(pts_to_use) << ", ";
            oss << "splice_event_id=" << splice_event_id << ", ";
            oss << "break_duration=" << break_duration << ", ";
            oss << "out_of_network=" << (out_of_network ? "true" : "false") << ", ";
            oss << "cancel=" << (cancel ? "true" : "false") << "}";
            cue_event->cue_data = oss.str();
            
            // 콜백 사용
            new_cue_event_cb_(cue_event);            
        }
    } else {
        // splice_command_type이 0x05가 아니면 payload skip
        LOG(INFO) << "SCTE-35 splice_command skip...";
    }
    return true;
}

void Scte35SectionParser::ResetPsiSection() {
  last_pts_ = 0.0;
}

}  // namespace mp2t
}  // namespace media
}  // namespace shaka 