#pragma once
#include <string>

#include "protocol.hpp"

// interface only. the implementation lives in metrics/ (Person 2)
namespace metrics {

// call once per program, before anything else logs
void init(const std::string& csv_path);

// one row per packet:  local_us,dir,type,stream,seq,frame,frag,bytes,timestamp_us
// dir is 'S' for sent, 'R' for received
void packet(char dir, const proto::Header& h, size_t bytes);

// one row per event:   local_us,E,name,value   e.g. ("frame_shown", 1)
void event(const char* name, double value);

}  // namespace metrics
