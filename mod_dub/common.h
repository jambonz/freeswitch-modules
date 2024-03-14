#ifndef _COMMON_H_
#define _COMMON_H_

#include <boost/circular_buffer.hpp>
#include <string>
#include <vector>
#include <queue>

typedef struct HttpPayload {
  std::string url;
  std::string body;
  std::vector<std::string> headers;
} HttpPayload_t;

typedef boost::circular_buffer<int16_t> CircularBuffer_t;
typedef std::queue<HttpPayload_t> HttpPayloadQueue_t;
typedef int32_t downloadId_t;


#define INVALID_DOWNLOAD_ID (-1)

#endif