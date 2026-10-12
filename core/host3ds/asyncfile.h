#pragma once
// A file written through a memory ring that a thread on core 1 drains to the card (asyncfile.c): the caller never
// waits for the SD card. One at a time; fclose waits until everything is on the card.
#include <stdio.h>
FILE* asyncfile_open(const char* path, const char* mode);
