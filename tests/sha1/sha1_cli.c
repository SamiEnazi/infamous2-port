/* sha1_cli FILE -> hex SHA-1, fed in odd-sized chunks to exercise buffering */
#include <stdio.h>
#include "../../port_src/sha1_portable.h"
int main(int argc, char** argv)
{
    FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
    CC_SHA1_CTX c; CC_SHA1_Init(&c);
    unsigned char buf[997]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) CC_SHA1_Update(&c, buf, (CC_LONG)n);
    unsigned char md[20]; CC_SHA1_Final(md, &c);
    for (int i = 0; i < 20; i++) printf("%02x", md[i]);
    printf("\n"); return 0;
}
