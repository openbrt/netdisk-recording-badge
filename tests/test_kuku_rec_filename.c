#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kuku_rec_filename.h"

int main(void) {
    char name[32];

    // 2024-01-01 00:00:00 UTC is 08:00:00 in China Standard Time.
    assert(kuku_rec_filename_build((time_t)1704067200, 42, 0,
                                   name, sizeof(name)));
    assert(strcmp(name, "20240101_080000.WAV") == 0);

    assert(kuku_rec_filename_build((time_t)1704067200, 42, 7,
                                   name, sizeof(name)));
    assert(strcmp(name, "20240101_080000_07.WAV") == 0);

    assert(kuku_rec_filename_build((time_t)0, 42, 0, name, sizeof(name)));
    assert(strcmp(name, "REC00000042.WAV") == 0);

    char small[8] = "dirty";
    assert(!kuku_rec_filename_build((time_t)1704067200, 42, 0,
                                    small, sizeof(small)));
    assert(small[0] == 0);

    puts("test_kuku_rec_filename: PASS");
    return 0;
}
