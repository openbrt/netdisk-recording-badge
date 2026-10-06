#include "kuku_console_line.h"
#include <assert.h>
#include <string.h>

int main(void) {
    kuku_console_line_t line={0};
    // Emulate short USB reads with a command crossing several 64-byte packets.
    const char *command="ENDURANCE ARM http://192.168.1.123:8765/telemetry "
                        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    for (size_t offset=0;offset<strlen(command);) {
        size_t end=offset+7;
        if (end>strlen(command)) end=strlen(command);
        while (offset<end) assert(kuku_console_line_feed(&line,command[offset++])==KUKU_LINE_MORE);
        // Idle/EOF between packets must not submit a partial command.
        assert(line.used==offset);
    }
    assert(kuku_console_line_feed(&line,'\r')==KUKU_LINE_READY);
    assert(!strcmp(line.text,command));
    assert(kuku_console_line_feed(&line,'\n')==KUKU_LINE_MORE);
    for (unsigned i=0;i<KUKU_CONSOLE_LINE_CAP-1;i++)
        assert(kuku_console_line_feed(&line,'A')==KUKU_LINE_MORE);
    assert(kuku_console_line_feed(&line,'\n')==KUKU_LINE_READY);
    assert(strlen(line.text)==KUKU_CONSOLE_LINE_CAP-1);
    for (unsigned i=0;i<600;i++) kuku_console_line_feed(&line,'B');
    assert(kuku_console_line_feed(&line,'\n')==KUKU_LINE_REJECTED);
    kuku_console_line_feed(&line,'R'); kuku_console_line_feed(&line,0);
    assert(kuku_console_line_feed(&line,'\n')==KUKU_LINE_REJECTED);
    for (const char *p="STATE";*p;p++) kuku_console_line_feed(&line,*p);
    assert(kuku_console_line_feed(&line,'\n')==KUKU_LINE_READY);
    assert(!strcmp(line.text,"STATE"));
    return 0;
}
