// placeholder so everything links from day one.
// Person 2 replaces this with the real CSV logger.
#include "metrics.hpp"

namespace metrics {

void init(const std::string&) {}
void packet(char, const proto::Header&, size_t) {}
void event(const char*, double) {}

}  // namespace metrics
