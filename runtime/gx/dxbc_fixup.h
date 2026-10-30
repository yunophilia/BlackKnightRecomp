#pragma once
#include <cstddef>
#include <cstdint>

// DXVK 2.6.2 (Proton 10.0) renders dual-source blending wrong when a pixel shader declares
// its outputs before its inputs, which is how newer vkd3d-shader (Wine's d3dcompiler_47)
// writes them: blended draws showed texture coordinates instead of colours. Microsoft's
// compiler declares inputs first. This moves the output declarations after the inputs and
// rewrites the container checksum. Blobs already in that order are left untouched.
// Returns true if the blob was changed.
bool FixDxbcDeclarationOrder(void* data, size_t size);

// The DXBC container checksum (MD5 variant over everything after the checksum field).
void DxbcChecksum(const void* data, size_t size, uint32_t out[4]);
