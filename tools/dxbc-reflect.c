#define COBJMACROS
#define CINTERFACE
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s shader.dxbc\n", argv[0]);
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

  ID3D11ShaderReflection *reflection = NULL;
  HRESULT result = D3DReflect(bytes, (SIZE_T)length, &IID_ID3D11ShaderReflection, (void **)&reflection);
  free(bytes);
  if (FAILED(result)) {
    fprintf(stderr, "D3DReflect failed: 0x%08lx\n", (unsigned long)result);
    return 1;
  }
  D3D11_SHADER_DESC shader;
  if (FAILED(reflection->lpVtbl->GetDesc(reflection, &shader))) return 1;
  printf("constant-buffers %u resources %u\n", shader.ConstantBuffers, shader.BoundResources);
  for (UINT index = 0; index < shader.ConstantBuffers; index++) {
    ID3D11ShaderReflectionConstantBuffer *buffer = reflection->lpVtbl->GetConstantBufferByIndex(reflection, index);
    D3D11_SHADER_BUFFER_DESC description;
    if (FAILED(buffer->lpVtbl->GetDesc(buffer, &description))) continue;
    printf("cbuffer %u %s size=%u variables=%u\n", index, description.Name, description.Size, description.Variables);
    for (UINT variableIndex = 0; variableIndex < description.Variables; variableIndex++) {
      ID3D11ShaderReflectionVariable *variable = buffer->lpVtbl->GetVariableByIndex(buffer, variableIndex);
      D3D11_SHADER_VARIABLE_DESC variableDescription;
      if (FAILED(variable->lpVtbl->GetDesc(variable, &variableDescription))) continue;
      printf("  %s offset=%u size=%u", variableDescription.Name, variableDescription.StartOffset, variableDescription.Size);
      if (variableDescription.DefaultValue && variableDescription.Size >= sizeof(float)) {
        const float *values = (const float *)variableDescription.DefaultValue;
        UINT count = variableDescription.Size / sizeof(float);
        printf(" default=");
        for (UINT valueIndex = 0; valueIndex < count; valueIndex++) {
          printf("%s%.9g", valueIndex ? "," : "", values[valueIndex]);
        }
      }
      printf("\n");
    }
  }
  for (UINT index = 0; index < shader.BoundResources; index++) {
    D3D11_SHADER_INPUT_BIND_DESC resource;
    if (FAILED(reflection->lpVtbl->GetResourceBindingDesc(reflection, index, &resource))) continue;
    printf("resource %s type=%u bind=%u count=%u\n", resource.Name, resource.Type, resource.BindPoint, resource.BindCount);
  }
  reflection->lpVtbl->Release(reflection);
  return 0;
}
