#ifndef __MOD_VAD_DETECT_H__
#define __MOD_VAD_DETECT_H__

#include <switch.h>

#define MY_BUG_NAME "vad_detect"
#define VAD_EVENT_DETECTION "vad_detect:detection"

typedef struct private_data
{
  char *bugname;
  char *strategy;
  char *sessionId;

  int stopping;

  switch_vad_t *vad;
  switch_thread_t* thread;

} private_t;

#endif