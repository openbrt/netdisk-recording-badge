#include "kuku_baidu_auth_link.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    char url[96];
    assert(kuku_baidu_auth_link("aB12cd34", url, sizeof(url)));
    assert(!strcmp(url, "https://openapi.baidu.com/device?display=page&code=aB12cd34"));
    assert(kuku_baidu_auth_link("123456789012345", url, sizeof(url)));
    assert(!kuku_baidu_auth_link("1234567890123456", url, sizeof(url)));
    assert(!url[0]);
    const char *invalid[] = { "", "abc&redirect=x", "ab/cd", "ab cd", "ab\ncd", "ab%20cd", "授权码" };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        strcpy(url, "stale");
        assert(!kuku_baidu_auth_link(invalid[i], url, sizeof(url)));
        assert(!url[0]);
    }
    assert(!kuku_baidu_auth_link(NULL, url, sizeof(url)));
    assert(!kuku_baidu_auth_link("abcd1234", NULL, sizeof(url)));
    assert(!kuku_baidu_auth_link("abcd1234", url, 0));
    assert(!kuku_baidu_auth_link("abcd1234", url, 16));
    assert(!url[0]);
    puts("Baidu authorization link: PASS");
    return 0;
}
