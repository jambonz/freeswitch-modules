#ifndef _COMMON_H_
#define _COMMON_H_

#include <boost/circular_buffer.hpp>


typedef boost::circular_buffer<int16_t> CircularBuffer_t;
typedef int32_t downloadId_t;

#define INVALID_DOWNLOAD_ID (-1)

#endif