#include "kuku_cloud_path.h"
#include "kuku_rec_filename.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char path[KUKU_CLOUD_PATH_MAX], child[KUKU_CLOUD_PATH_MAX], parent[KUKU_CLOUD_PATH_MAX];
    assert(kuku_cloud_recording_path("20261003_091011.WAV", path, sizeof(path)));
    assert(!strcmp(path, KUKU_CLOUD_ROOT "/2026-10-03/20261003_091011.WAV"));
    assert(kuku_cloud_recording_path("20240229_235959_999.WAV", path, sizeof(path)));
    assert(strstr(path, "/2024-02-29/"));
    // The date is the recording start date; retry/upload time is not an input.
    char name[32];
    assert(kuku_rec_filename_build(1704124800, 1, 0, name, sizeof(name)));
    assert(!strcmp(name, "20240102_000000.WAV")); // UTC+8 midnight
    assert(kuku_cloud_recording_path(name, path, sizeof(path)));
    assert(strstr(path, "/2024-01-02/"));
    assert(kuku_cloud_recording_path("REC0003.WAV", path, sizeof(path)));
    assert(!strcmp(path, KUKU_CLOUD_ROOT "/undated/REC0003.WAV"));
    assert(kuku_cloud_recording_path("REC4294967295.WAV", path, sizeof(path)));
    const char *bad[] = {"20230229_123000.WAV", "21000229_123000.WAV", "20261301_000000.WAV",
        "20260100_000000.WAV", "20260101_240000.WAV", "20260101_006000.WAV",
        "20260101_000060.WAV", "20260101_000000_bad.WAV", "../REC0003.WAV",
        "RECxx.WAV", "20261003_091011.REC", "photo.jpg", "REC0001.WAV/evil"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        assert(!kuku_cloud_recording_path(bad[i], path, sizeof(path)));
        assert(!path[0]);
    }
    assert(kuku_cloud_child_path(KUKU_CLOUD_ROOT, "2026-10-03", path, sizeof(path)));
    assert(kuku_cloud_child_path(path, "images", child, sizeof(child)));
    assert(kuku_cloud_parent_path(child, parent, sizeof(parent)) && !strcmp(parent, path));
    assert(kuku_cloud_parent_path(path, parent, sizeof(parent)) && !strcmp(parent, KUKU_CLOUD_ROOT));
    assert(!kuku_cloud_parent_path(KUKU_CLOUD_ROOT, parent, sizeof(parent)));
    assert(!kuku_cloud_child_path(KUKU_CLOUD_ROOT, "..", path, sizeof(path)));
    assert(!kuku_cloud_child_path(KUKU_CLOUD_ROOT, "a/b", path, sizeof(path)));
    assert(!kuku_cloud_child_path(KUKU_CLOUD_ROOT "-other", "x", path, sizeof(path)));
    assert(!kuku_cloud_child_path(KUKU_CLOUD_ROOT "/../other", "x", path, sizeof(path)));
    assert(!kuku_cloud_recording_path(name, path, 8) && !path[0]);
    char long_name[256]; memset(long_name, 'x', sizeof(long_name)); long_name[255] = 0;
    assert(!kuku_cloud_child_path(KUKU_CLOUD_ROOT, long_name, path, sizeof(path)) && !path[0]);
    puts("Cloud date routing and navigation paths: PASS");
}
