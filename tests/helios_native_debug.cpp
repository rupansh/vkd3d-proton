/* Keep Windows SDK debug interfaces separate from vkd3d's generated headers. */
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <climits>

extern "C" unsigned int helios_native_debug_errors(void *opaque)
{
    /* The pinned native runner requires this variable and enables the layer.
     * The generic Windows suite also links this fixture, with debug optional. */
    if (!getenv("HELIOS_NATIVE_EXPECTED_UMD12_SHA256"))
        return 0;
    auto *device = static_cast<ID3D12Device *>(opaque);
    ID3D12InfoQueue *info;
    unsigned int errors = 0;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&info))))
        return UINT_MAX;
    const UINT64 count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < count; i++)
    {
        SIZE_T size = 0;
        if (FAILED(info->GetMessage(i, nullptr, &size)))
        {
            errors++;
            continue;
        }
        auto *message = static_cast<D3D12_MESSAGE *>(malloc(size));
        if (!message || FAILED(info->GetMessage(i, message, &size)))
            errors++;
        else if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        {
            fprintf(stderr, "D3D12_VALIDATION,%u,%u,%s\n", message->ID, message->Severity, message->pDescription);
            errors++;
        }
        free(message);
    }
    info->ClearStoredMessages();
    info->Release();
    return errors;
}
