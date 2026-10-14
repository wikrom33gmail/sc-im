#ifndef WHICHKEY_H
#define WHICHKEY_H

#include "buffer.h"

void wk_show(struct block * pending); // show popup of available keys for the typed prefix
void wk_hide(void);                   // clear popup if visible

#endif
