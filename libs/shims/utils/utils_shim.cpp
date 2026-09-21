#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>

#include <utils/String8.h>

extern "C" pid_t androidGetTid() {
	return gettid();
}