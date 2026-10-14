/* decode_dng.cpp
 *
 * Minimal example: decode a .GPR still to a .DNG using the GPR SDK.
 *
 * Usage:
 *     decode_dng <input.gpr> <output.dng>
 *
 * Why .cpp: the gpr_sdk public headers use raw `bool` without
 * including <stdbool.h>, and pull noise_model.h from vc5_common
 * which isn't on the gpr_sdk public include path. The SDK is
 * already C++ and links against the Adobe DNG SDK, so the C++
 * toolchain is required anyway. The SDK functions themselves
 * are exposed via extern "C".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpr.h"
#include "gpr_buffer.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <input.gpr> <output.dng>\n", argv[0]);
        return 1;
    }
    const char *in_path  = argv[1];
    const char *out_path = argv[2];

    gpr_allocator allocator;
    allocator.Alloc = malloc;
    allocator.Free  = free;

    gpr_buffer input_buffer  = { NULL, 0 };
    gpr_buffer output_buffer = { NULL, 0 };

    if (read_from_file(&input_buffer, in_path, allocator.Alloc, allocator.Free) != 0) {
        fprintf(stderr, "failed to read %s\n", in_path);
        return 1;
    }

    gpr_parameters params;
    gpr_parameters_set_defaults(&params);

    /* Decoder reads compression params from the GPR's metadata. We only
       need to provide a parameters struct so the SDK can populate the
       Exif/profile/tuning info it parses from the input. */
    if (!gpr_convert_gpr_to_dng(&allocator, &params, &input_buffer, &output_buffer)) {
        fprintf(stderr, "gpr_convert_gpr_to_dng failed\n");
        allocator.Free(input_buffer.buffer);
        gpr_parameters_destroy(&params, allocator.Free);
        return 1;
    }

    if (write_to_file(&output_buffer, out_path) != 0) {
        fprintf(stderr, "failed to write %s\n", out_path);
        allocator.Free(input_buffer.buffer);
        allocator.Free(output_buffer.buffer);
        gpr_parameters_destroy(&params, allocator.Free);
        return 1;
    }

    printf("decoded %s -> %s (%zu bytes)\n",
           in_path, out_path, output_buffer.size);

    allocator.Free(input_buffer.buffer);
    allocator.Free(output_buffer.buffer);
    gpr_parameters_destroy(&params, allocator.Free);
    return 0;
}
