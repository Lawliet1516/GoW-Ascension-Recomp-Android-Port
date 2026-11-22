// host test for render/libs/video/rsx_spv_clipblock.h:  spv_clipblock_test in.spv out.spv   -> prints "rewritten" or "unchanged"; exit 0 either way, 2 on I/O error
#include "../../../../render/libs/video/rsx_spv_clipblock.h"
#include <cstdio>
#include <cstring>
int main(int argc, char** argv)
{
    if (argc < 3) return 2;
    FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
    std::vector<unsigned int> w; unsigned int x; while (fread(&x, 4, 1, f) == 1) w.push_back(x); fclose(f);
    const std::vector<unsigned int> before = w;
    const int r = rsx_spv::put_clipdistance_in_block(w);
    if (!r && w != before) { printf("BUG: returned 0 but changed the module\n"); return 1; }
    f = fopen(argv[2], "wb"); if (!f) return 2; fwrite(w.data(), 4, w.size(), f); fclose(f);
    printf("%s\n", r ? "rewritten" : "unchanged"); return 0;
}
