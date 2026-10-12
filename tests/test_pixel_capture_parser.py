#!/usr/bin/env python3
"""Production capture parser: bounds, redraws, split escapes, real text."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path


def function(source, name):
    match = re.search(r'^void ' + name + r'\(|^bool ' + name + r'\(', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}\n', match.start()) + 3]


MAIN = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef struct { char bytes[32768]; size_t used; int rows; } output_t;
static bool publish(const char *line,size_t len,void *context) {
    output_t *out=context; assert(out->used+len<sizeof(out->bytes));
    memcpy(out->bytes+out->used,line,len); out->used+=len;out->rows++;
    return true;
}
static void run(const char *text,size_t len,size_t chunk,output_t *out) {
    pixel_capture_parser_t p; pixel_capture_parser_init(&p);
    for(size_t at=0;at<len;) {
        size_t n=chunk<len-at?chunk:len-at;
        pixel_capture_parser_feed(&p,text+at,n,publish,out);at+=n;
    }
    pixel_capture_parser_finish(&p,publish,out);
}
int main(void) {
    /* At these lengths tab padding reaches the exact buffer limit. */
    for(size_t prefix=4092;prefix<=4096;prefix++) {
        char input[8192];memset(input,'x',prefix);input[prefix]='\t';input[prefix+1]='\n';
        for(size_t chunk=1;chunk<=8192;chunk*=2) {
            output_t out={0};run(input,prefix+2,chunk,&out);
            size_t buffered=prefix<=4095?prefix:prefix%4095;
            size_t padding=4-buffered%4;
            /* Length 4096 flushes 4095 and starts a new bounded chunk. */
            assert(out.used==prefix+padding);
            for(size_t i=0;i<prefix;i++)assert(out.bytes[i]=='x');
            for(size_t i=prefix;i<out.used;i++)assert(out.bytes[i]==' ');
        }
    }
    const char *progress="spinner\rupdated\r\n\x1b[2Kdiscarded\nkept\n";
    for(size_t chunk=1;chunk<=64;chunk*=2) {
        output_t out={0};run(progress,strlen(progress),chunk,&out);
        assert(out.used==11&&!memcmp(out.bytes,"updatedkept",11));
    }
    const char *esc="before\x1b]title\x1b\\after\n";
    output_t out={0};run(esc,strlen(esc),1,&out);
    assert(out.used==11&&!memcmp(out.bytes,"beforeafter",11));
    const char *semantic="{\"result\":\"preserve JSON\"}\ncode: x = 1\n";
    out=(output_t){0};run(semantic,strlen(semantic),3,&out);
    assert(out.rows==2&&out.used==strlen(semantic)-2);
    puts("PASS: capture boundary tabs, arbitrary chunks, redraw exclusion, split escapes and semantic text");
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    parser.add_argument('--include', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (args.source or root / 'src/pixel_tui.c').read_text()
    include = args.include or root / 'include'
    declaration = '#include "pixel_tui.h"\n' + 'enum { CAPTURE_TEXT, CAPTURE_ESC, CAPTURE_CSI, CAPTURE_STRING, CAPTURE_STRING_ESC };\n'
    extracted = ''.join(function(source, name) for name in
                        ('pixel_capture_parser_init', 'pixel_capture_parser_feed', 'pixel_capture_parser_finish'))
    with tempfile.TemporaryDirectory(prefix='dsco-capture-parser-') as tmp:
        folder = Path(tmp)
        fixture = folder / 'parser.c'
        fixture.write_text('#include <string.h>\n' + declaration + extracted + MAIN)
        binary = folder / 'parser'
        subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-g', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-I', str(include), str(fixture),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=3)


if __name__ == '__main__':
    main()
