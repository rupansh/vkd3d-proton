RWByteAddressBuffer Result : register(u0);
#if defined(ROOT_VIEWS)
cbuffer Values : register(b0) { uint value; };
cbuffer Tail : register(b1) { uint tail; };
[numthreads(1, 1, 1)]
void main() { Result.Store2(0, uint2(value, tail)); }
#else
cbuffer First : register(b0) { uint first; };
cbuffer BeforeBoundary : register(b62) { uint before_boundary; };
cbuffer AfterBoundary : register(b63) { uint after_boundary; };
cbuffer Last : register(b126) { uint last; };
[numthreads(1, 1, 1)]
void main() { Result.Store4(0, uint4(first, before_boundary, after_boundary, last)); }
#endif
