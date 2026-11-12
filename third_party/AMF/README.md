# AMD AMF SDK headers

Public headers from `GPUOpen-LibrariesAndSDKs/AMF`, tag `v1.4.36`, path `amf/public/include/`
(MIT; each file carries the licence). Only the headers `src/ombra/amf_encoder.*` and
`amf_probe.cpp` include, plus what those pull in. The runtime (`amfrt64.dll`) ships with the AMD
driver and is loaded at run time; nothing is linked. `AMFInit` is called with the older of this
header version and the runtime's, so an older driver still works.
