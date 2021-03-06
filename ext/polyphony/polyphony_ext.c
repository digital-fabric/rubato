#include "polyphony.h"

void Init_Fiber();
void Init_Polyphony();
void Init_Backend();
void Init_Queue();
void Init_Event();
void Init_SocketExtensions();
void Init_Thread();

#ifdef POLYPHONY_PLAYGROUND
extern void playground();
#endif

void Init_polyphony_ext() {
  Init_Polyphony();

  Init_Backend();
  Init_Queue();
  Init_Event();
  Init_Fiber();
  Init_Thread();

  Init_SocketExtensions();

  #ifdef POLYPHONY_PLAYGROUND
  playground();
  #endif
}