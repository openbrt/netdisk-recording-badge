#include "kuku_console_line.h"

kuku_line_result_t kuku_console_line_feed(kuku_console_line_t *line, unsigned char byte) {
    if (byte=='\r' || byte=='\n') {
        bool bad=line->invalid;
        bool empty=line->used==0;
        line->text[line->used]='\0';
        line->used=0;
        line->invalid=false;
        return bad ? KUKU_LINE_REJECTED : empty ? KUKU_LINE_MORE : KUKU_LINE_READY;
    }
    if (byte==0 || line->used==sizeof(line->text)-1) line->invalid=true;
    if (!line->invalid) line->text[line->used++]=(char)byte;
    return KUKU_LINE_MORE;
}
