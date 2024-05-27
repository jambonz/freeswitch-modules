#ifndef __MOD_VAD_DETECT_H__
#define __MOD_VAD_DETECT_H__

#include <switch.h>

#define MY_BUG_NAME "vad_detect"
#define VAD_EVENT_START_TALKING "vad_detect:start_talking"
#define VAD_EVENT_STOP_TALKING "vad_detect:stop_talking"

typedef struct private_data
{
  char *bugname;
  char *action;

  switch_vad_t *vad;

} private_t;

#endif