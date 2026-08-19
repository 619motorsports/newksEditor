#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <d3dcompiler.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s shader.fxo\n", argv[0]);
    return 2;
  }
  FILE *file = fopen(argv[1], "rb");
  if (!file) {
    perror(argv[1]);
    return 1;
  }
  if (fseek(file, 0, SEEK_END) != 0) return 1;
  long length = ftell(file);
  if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) return 1;
  void *bytes = malloc((size_t)length);
  if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) return 1;
  fclose(file);

  ID3DBlob *output = NULL;
  HRESULT result = D3DDisassemble(bytes, (SIZE_T)length, 0, NULL, &output);
  free(bytes);
  if (FAILED(result)) {
    fprintf(stderr, "D3DDisassemble failed: 0x%08lx\n", (unsigned long)result);
    return 1;
  }
  fwrite(ID3D10Blob_GetBufferPointer(output), 1, ID3D10Blob_GetBufferSize(output), stdout);
  ID3D10Blob_Release(output);
  return 0;
}
