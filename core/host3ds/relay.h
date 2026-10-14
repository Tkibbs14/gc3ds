// Dev relay client: remote test runs from anywhere (see relay.c and core/tools/relay.sh).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    unsigned id;
    char build[17];  // the build the job wants ("-": any)
    char args[1000];
} RelayJob;

bool relay_configured(void);  // sdmc:/gc3ds/relay.txt exists and names a url and token
bool relay_auto(void);        // relay.txt has "auto 1": remote mode starts by itself
bool relay_start(char* err, size_t n);
void relay_reserve(void);  // takes the sockets buffer now (call before the large allocations)
// sha256 prefix of our own .3dsx (the id the relay gives builds), computed once.
const char* relay_self_id(const char* selfPath);
int relay_next(RelayJob* j, char* err, size_t n);  // 1: a job, 0: none queued, -1: error
bool relay_fetch_build(const char* id, const char* path, char* err, size_t n);
// Log lines from now on go to this job. firstSeq: 0, or RELAY_RESUME_SEQ for a job resumed after a restart (the relay
// upserts chunks by number, so the new instance must not reuse the numbers the old one sent: job 111 lost its header).
#define RELAY_RESUME_SEQ 100000
void relay_job_begin(unsigned id, int firstSeq);
FILE* relay_tee(FILE* f);           // a FILE writing to f and to the current job's log
void relay_job_end(const char* status);  // sends the rest of the log, then the status (done, failed, stopped)
void relay_stop(void);
void relay_flush(void);  // sends what the current job's log holds now (before a restart into a new build)
// Live control of a running job: commands the PC sent (relay.sh cmd), one at a time; false when none is waiting.
bool relay_take_cmd(char* out, size_t n);
// Uploads a top-screen frame (a BMP in memory, copied) to the current job, from the relay thread.
void relay_frame(const void* bmp, unsigned bytes);
