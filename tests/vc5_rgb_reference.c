/* SPDX-License-Identifier: Apache-2.0 OR MIT
 * Standalone public-API probe; also builds against historical stock libraries. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "vc5_decoder.h"
int main(int argc,char **argv) {
    if (argc!=4 && argc!=5) return 1;
    FILE *input=fopen(argv[1],"rb"); if (!input) return 2;
    if (fseek(input,0,SEEK_END)) return 2;
    long length=ftell(input); if (length<=0 || fseek(input,0,SEEK_SET)) return 2;
    void *data=malloc((size_t)length); if (!data) return 2;
    if (fread(data,1,(size_t)length,input)!=(size_t)length || fclose(input)) return 2;
    vc5_decoder_parameters p={0}; vc5_decoder_parameters_set_default(&p);
    p.mem_alloc=malloc; p.mem_free=free;
    int scale=atoi(argv[3]);
    p.rgb_resolution=scale==4 ? GPR_RGB_RESOLUTION_QUARTER:
        (scale==8 ? GPR_RGB_RESOLUTION_EIGHTH:GPR_RGB_RESOLUTION_SIXTEENTH);
    p.rgb_bits=argc==5 ? atoi(argv[4]):8;
    if ((p.rgb_bits!=8 && p.rgb_bits!=16) || (scale!=4 && scale!=8 && scale!=16)) return 1;
    gpr_buffer vc5={data,(size_t)length}; gpr_rgb_buffer rgb={0};
    if (vc5_decoder_process(&p,&vc5,NULL,&rgb) || !rgb.buffer || !rgb.size) return 3;
    FILE *out=fopen(argv[2],"wb"); if (!out) return 4;
    int failed=fwrite(rgb.buffer,1,rgb.size,out)!=rgb.size || fclose(out);
    printf("%zu %zu %zu\n",rgb.width,rgb.height,rgb.size);
    free(rgb.buffer); free(data); return failed ? 4:0;
}
