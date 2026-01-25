#include <stdio.h>

void build__create_outfile  ( const char filename
                            , char output_filename
                            ) {
    FILE *output_file = NULL;
    
    /* create */
    strncpy(output_filename, filename, len - 3);
    output_filename[len - 3] = '\0';
    strcat(output_filename, ".o");
    
    /* open */
    output_file = fopen(output_filename, "wb");
    if (!output_file) {
        fprintf(strerr, "ERROR: Failed to create output file\n");
        return;
    }
    
    /* close */
    fclose(output_file);
    
    /* read */
    output_file = fopen(output_filename, "rb");
    if (output_file) {
        fseek(output_file, 0, SEEK_END);
        size_t file_size = ftell(output_file);
        fclose(output_file);
    }
}
