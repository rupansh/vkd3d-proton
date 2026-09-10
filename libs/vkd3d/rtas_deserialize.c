/* DXR permits TLAS deserialization before referenced BLAS contents exist.
 * Vulkan additionally requires their VkAccelerationStructureKHR objects to
 * exist. Read the public serialized pointer table at the GPU execution boundary
 * and place those views. Serialization postbuild queries share this execution
 * boundary: their pointer counts also depend on current GPU contents. Neither
 * path runs for ordinary builds, indirect commands or DispatchRays. */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "vkd3d_private.h"

#define VKD3D_RTAS_METADATA_CHUNK_SIZE (64u * 1024u)

struct vkd3d_rtas_metadata_reader
{
    struct d3d12_command_queue *queue;
    struct vkd3d_device_memory_allocation memory;
    VkBuffer buffer;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkQueryPool query_pool;
    VkBuffer serialized_buffer;
    struct vkd3d_device_memory_allocation serialized_memory;
    void *mapped;
};

static HRESULT vkd3d_rtas_wait_boundary(struct d3d12_command_queue *queue, uint64_t value, bool fence_proxy)
{
    const struct vkd3d_vk_device_procs *vk_procs = &queue->device->vk_procs;
    VkSemaphoreWaitInfo info = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    VkResult vr;
    HRESULT hr;

    info.semaphoreCount = 1;
    info.pSemaphores = &queue->vkd3d_queue->submission_timeline;
    info.pValues = &value;
    do
    {
        if (FAILED(hr = d3d12_device_removed_reason(queue->device)))
            return hr;
        /* A finite wait permits device-removal cancellation without polling a
         * mapped GPU word or holding any submission/VA-map lock. No queue idle. */
        vr = fence_proxy ? vkd3d_queue_wait_submission_timeline(queue->vkd3d_queue, value, 100000000) :
                VK_CALL(vkWaitSemaphores(queue->device->vk_device, &info, 100000000));
    } while (vr == VK_TIMEOUT);
    return hresult_from_vk_result(vr);
}

static void vkd3d_rtas_metadata_reader_cleanup(struct vkd3d_rtas_metadata_reader *reader)
{
    struct d3d12_device *device = reader->queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

    VK_CALL(vkDestroyCommandPool(device->vk_device, reader->pool, NULL));
    VK_CALL(vkDestroyQueryPool(device->vk_device, reader->query_pool, NULL));
    VK_CALL(vkDestroyBuffer(device->vk_device, reader->serialized_buffer, NULL));
    if (reader->serialized_memory.vk_memory)
        vkd3d_free_device_memory(device, &reader->serialized_memory);
    VK_CALL(vkDestroyBuffer(device->vk_device, reader->buffer, NULL));
    if (reader->mapped)
        VK_CALL(vkUnmapMemory(device->vk_device, reader->memory.vk_memory));
    if (reader->memory.vk_memory)
        vkd3d_free_device_memory(device, &reader->memory);
}

static HRESULT vkd3d_rtas_metadata_reader_init(struct vkd3d_rtas_metadata_reader *reader,
        struct d3d12_command_queue *queue)
{
    struct d3d12_device *device = queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkCommandBufferAllocateInfo command = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkBufferCreateInfo buffer = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkResult vr;
    HRESULT hr;

    memset(reader, 0, sizeof(*reader));
    reader->queue = queue;
    buffer.size = VKD3D_RTAS_METADATA_CHUNK_SIZE;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if ((vr = VK_CALL(vkCreateBuffer(device->vk_device, &buffer, NULL, &reader->buffer))) < 0)
        return hresult_from_vk_result(vr);
    if (FAILED(hr = vkd3d_allocate_internal_buffer_memory(device, reader->buffer,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &reader->memory)))
        return hr;
    if ((vr = VK_CALL(vkMapMemory(device->vk_device, reader->memory.vk_memory,
            0, VK_WHOLE_SIZE, 0, &reader->mapped))) < 0)
        return hresult_from_vk_result(vr);
    pool.queueFamilyIndex = queue->vkd3d_queue->vk_family_index;
    if ((vr = VK_CALL(vkCreateCommandPool(device->vk_device, &pool, NULL, &reader->pool))) < 0)
        return hresult_from_vk_result(vr);
    command.commandPool = reader->pool;
    command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command.commandBufferCount = 1;
    return hresult_from_vk_result(VK_CALL(vkAllocateCommandBuffers(device->vk_device, &command, &reader->command)));
}

static HRESULT vkd3d_rtas_metadata_begin(struct vkd3d_rtas_metadata_reader *reader)
{
    struct d3d12_device *device = reader->queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkResult vr;

    /* Each previous use of this pool has an authenticated GPU completion. */
    if ((vr = VK_CALL(vkResetCommandPool(device->vk_device, reader->pool, 0))) < 0)
        return hresult_from_vk_result(vr);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return hresult_from_vk_result(VK_CALL(vkBeginCommandBuffer(reader->command, &begin)));
}

static void vkd3d_rtas_metadata_barrier(struct vkd3d_rtas_metadata_reader *reader,
        VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
        VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    const struct vkd3d_vk_device_procs *vk_procs = &reader->queue->device->vk_procs;
    VkMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};

    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    VK_CALL(vkCmdPipelineBarrier2(reader->command, &dependency));
}

static HRESULT vkd3d_rtas_metadata_submit(struct vkd3d_rtas_metadata_reader *reader,
        uint64_t *last_submission, bool *quarantine)
{
    struct d3d12_command_queue *queue = reader->queue;
    struct d3d12_device *device = queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkCommandBufferSubmitInfo command = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    VkSemaphoreSubmitInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    VkSubmitInfo2 submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    VkQueue vk_queue;
    VkFence proxy;
    VkResult vr;
    HRESULT hr;

    if ((vr = VK_CALL(vkEndCommandBuffer(reader->command))) < 0)
        return hresult_from_vk_result(vr);
    if (!(vk_queue = vkd3d_queue_acquire(queue->vkd3d_queue)))
        return DXGI_ERROR_DEVICE_REMOVED;
    wait.semaphore = queue->vkd3d_queue->submission_timeline;
    wait.value = *last_submission;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signal = wait;
    signal.value = ++queue->vkd3d_queue->submission_timeline_count;
    command.commandBuffer = reader->command;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    /* Participate in the engine's existing NVIDIA CPU-wait fence tracking.
     * Every value published on this timeline needs its matching proxy. */
    proxy = vkd3d_queue_get_signal_fence_proxy_locked(queue->vkd3d_queue);
    vr = VK_CALL(vkQueueSubmit2(vk_queue, 1, &submit, proxy));
    vkd3d_queue_release(queue->vkd3d_queue);
    if (vr != VK_SUCCESS)
    {
        /* Even an error-bearing submit is not a host-quiescence receipt. */
        *quarantine = true;
        return hresult_from_vk_result(vr);
    }
    *last_submission = signal.value;
    queue->last_submission_timeline_value = signal.value;
    if (FAILED(hr = vkd3d_rtas_wait_boundary(queue, signal.value, proxy != VK_NULL_HANDLE)))
        *quarantine = true;
    return hr;
}

static HRESULT vkd3d_rtas_metadata_read(struct vkd3d_rtas_metadata_reader *reader,
        VkBuffer source, VkDeviceSize offset, VkDeviceSize size, uint64_t *last_submission, bool *quarantine)
{
    const struct vkd3d_vk_device_procs *vk_procs = &reader->queue->device->vk_procs;
    VkBufferCopy2 copy = {VK_STRUCTURE_TYPE_BUFFER_COPY_2};
    VkCopyBufferInfo2 info = {VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
    HRESULT hr;

    if (FAILED(hr = vkd3d_rtas_metadata_begin(reader)))
        return hr;
    vkd3d_rtas_metadata_barrier(reader, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    copy.srcOffset = offset;
    copy.size = size;
    info.srcBuffer = source;
    info.dstBuffer = reader->buffer;
    info.regionCount = 1;
    info.pRegions = &copy;
    VK_CALL(vkCmdCopyBuffer2(reader->command, &info));
    vkd3d_rtas_metadata_barrier(reader, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    return vkd3d_rtas_metadata_submit(reader, last_submission, quarantine);
}

static const struct vkd3d_unique_resource *vkd3d_rtas_address_resource(struct d3d12_device *device,
        D3D12_GPU_VIRTUAL_ADDRESS address, VkDeviceSize minimum_size)
{
    const struct vkd3d_unique_resource *resource;

    if (!address || (address & (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT - 1)))
        return NULL;
    resource = vkd3d_va_map_deref(&device->memory_allocator.va_map, address);
    if (!resource || !resource->va || address < resource->va ||
            address - resource->va >= resource->size || minimum_size > resource->size - (address - resource->va))
        return NULL;
    return resource;
}

static HRESULT vkd3d_rtas_prepare_deserialization(struct d3d12_command_queue *queue,
        const struct vkd3d_rtas_metadata *op, uint64_t *last_submission, bool *quarantine)
{
    struct d3d12_device *device = queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkAccelerationStructureVersionInfoKHR version = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_VERSION_INFO_KHR};
    D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER header;
    VkAccelerationStructureCompatibilityKHR compatibility;
    const struct vkd3d_unique_resource *source, *destination;
    struct vkd3d_rtas_metadata_reader reader;
    uint64_t offset, available, count, batch, i, address, chunks = 0;
    HRESULT hr;

    /* The application's GPU resource lifetime covers the execution; allocator
     * ownership covers its recorded command buffers. Resolve VAs again here,
     * and read contents only after the copy's exact GPU-completion receipt.
     * Neither addresses nor pointer tables are cached across execution/replay. */
    source = vkd3d_rtas_address_resource(device, op->src, sizeof(header));
    destination = vkd3d_rtas_address_resource(device, op->dst, 1);
    if (!source || !destination)
        return E_INVALIDARG;
    if (FAILED(hr = vkd3d_rtas_metadata_reader_init(&reader, queue)))
        goto done;
    /* Until the header is known, only its 56 public bytes belong to this
     * operation. Speculatively reading the rest of a shared heap could race
     * unrelated resources. The second copy reads only the validated table. */
    available = sizeof(header);
    if (FAILED(hr = vkd3d_rtas_metadata_read(&reader, source->vk_buffer,
            op->src - source->va, available, last_submission, quarantine)))
        goto done;
    chunks++;
    memcpy(&header, reader.mapped, sizeof(header));
    count = header.NumBottomLevelAccelerationStructurePointersAfterHeader;
    if (header.SerializedSizeInBytesIncludingHeader < sizeof(header) ||
            header.SerializedSizeInBytesIncludingHeader > source->size - (op->src - source->va) ||
            count > (header.SerializedSizeInBytesIncludingHeader - sizeof(header)) / sizeof(uint64_t) ||
            !header.DeserializedSizeInBytes ||
            header.DeserializedSizeInBytes > destination->size - (op->dst - destination->va))
    {
        ERR("Malformed serialized AS header or insufficient destination backing.\n");
        hr = E_INVALIDARG;
        goto done;
    }
    version.pVersionData = (const uint8_t *)&header.DriverMatchingIdentifier;
    VK_CALL(vkGetDeviceAccelerationStructureCompatibilityKHR(device->vk_device, &version, &compatibility));
    if (compatibility != VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR)
    {
        ERR("Serialized AS producer/version is incompatible.\n");
        hr = E_INVALIDARG;
        goto done;
    }
    offset = sizeof(header);
    available -= offset;
    while (count)
    {
        batch = min(count, available / sizeof(uint64_t));
        for (i = 0; i < batch; i++)
        {
            memcpy(&address, (const char *)reader.mapped + (chunks == 1 ? sizeof(header) : 0) + i * sizeof(address),
                    sizeof(address));
            /* Null entries are legal. Duplicate entries reuse the VA view map.
             * UNKNOWN creates an object without claiming that GPU contents have
             * been built or that their type is stable across aliasing/replay. */
            if (!address)
                continue;
            if (!vkd3d_rtas_address_resource(device, address,
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT))
            {
                ERR("Serialized AS references invalid BLAS address %#"PRIx64".\n", address);
                hr = E_INVALIDARG;
                goto done;
            }
            if (!vkd3d_va_map_place_acceleration_structure(&device->memory_allocator.va_map,
                    device, address, VKD3D_RTAS_KIND_UNKNOWN))
            {
                hr = E_OUTOFMEMORY;
                goto done;
            }
        }
        count -= batch;
        offset += batch * sizeof(uint64_t);
        if (!count)
            break;
        available = min(count * sizeof(uint64_t), VKD3D_RTAS_METADATA_CHUNK_SIZE);
        if (FAILED(hr = vkd3d_rtas_metadata_read(&reader, source->vk_buffer,
                op->src - source->va + offset, available, last_submission, quarantine)))
            goto done;
        chunks++;
    }
    TRACE("AS deserialization prepared dst %#"PRIx64", %"PRIu64" references, %"PRIu64" metadata chunks.\n",
            op->dst, header.NumBottomLevelAccelerationStructurePointersAfterHeader, chunks);
    hr = S_OK;

done:
    if (!*quarantine)
        vkd3d_rtas_metadata_reader_cleanup(&reader);
    else
        ERR("AS metadata resources quarantined after uncertain GPU completion.\n");
    return hr;
}

static HRESULT vkd3d_rtas_serialization_query(struct d3d12_command_queue *queue,
        const struct vkd3d_rtas_metadata *op, uint64_t *last_submission, bool *quarantine)
{
    struct d3d12_device *device = queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkQueryPoolCreateInfo query = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    VkBufferCreateInfo buffer = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkCopyAccelerationStructureToMemoryInfoKHR serialize =
            {VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR};
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION_DESC result;
    D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER header;
    const struct vkd3d_unique_resource *destination;
    struct vkd3d_rtas_metadata_reader reader;
    VkAccelerationStructureKHR source;
    VkDeviceAddress address;
    VkBufferCopy2 copy = {VK_STRUCTURE_TYPE_BUFFER_COPY_2};
    VkCopyBufferInfo2 copy_info = {VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
    uint64_t size, count;
    VkResult vr;
    HRESULT hr;

    destination = vkd3d_va_map_deref(&device->memory_allocator.va_map, op->dst);
    if (!destination || (op->dst & 7) || op->dst < destination->va ||
            op->dst - destination->va > destination->size ||
            sizeof(result) > destination->size - (op->dst - destination->va) ||
            !vkd3d_rtas_address_resource(device, op->src, 1))
        return E_INVALIDARG;
    if (!(source = vkd3d_va_map_place_acceleration_structure(&device->memory_allocator.va_map,
            device, op->src, VKD3D_RTAS_KIND_UNKNOWN)))
        return E_OUTOFMEMORY;
    if (FAILED(hr = vkd3d_rtas_metadata_reader_init(&reader, queue)))
        goto done;
    query.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR;
    query.queryCount = 1;
    if ((vr = VK_CALL(vkCreateQueryPool(device->vk_device, &query, NULL, &reader.query_pool))) < 0)
    {
        hr = hresult_from_vk_result(vr);
        goto done;
    }
    if (FAILED(hr = vkd3d_rtas_metadata_begin(&reader)))
        goto done;
    vkd3d_rtas_metadata_barrier(&reader, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    VK_CALL(vkCmdResetQueryPool(reader.command, reader.query_pool, 0, 1));
    VK_CALL(vkCmdWriteAccelerationStructuresPropertiesKHR(reader.command, 1, &source,
            query.queryType, reader.query_pool, 0));
    VK_CALL(vkCmdCopyQueryPoolResults(reader.command, reader.query_pool, 0, 1,
            reader.buffer, 0, sizeof(size), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    vkd3d_rtas_metadata_barrier(&reader, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    if (FAILED(hr = vkd3d_rtas_metadata_submit(&reader, last_submission, quarantine)))
        goto done;
    memcpy(&size, reader.mapped, sizeof(size));
    if (size < sizeof(header) || size > UINT64_MAX - 255)
    {
        hr = E_INVALIDARG;
        goto done;
    }

    /* The query result sizes the allocation. Never estimate an opaque driver's
     * serialization overhead from the source allocation or a recorded build. */
    buffer.size = size + 255;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if ((vr = VK_CALL(vkCreateBuffer(device->vk_device, &buffer, NULL, &reader.serialized_buffer))) < 0)
    {
        hr = hresult_from_vk_result(vr);
        goto done;
    }
    if (FAILED(hr = vkd3d_allocate_internal_buffer_memory(device, reader.serialized_buffer,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &reader.serialized_memory)))
        goto done;
    address = vkd3d_get_buffer_device_address(device, reader.serialized_buffer);
    if (!address || address > UINT64_MAX - 255)
    {
        hr = E_INVALIDARG;
        goto done;
    }
    serialize.src = source;
    serialize.dst.deviceAddress = align(address, 256);
    serialize.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR;
    if (FAILED(hr = vkd3d_rtas_metadata_begin(&reader)))
        goto done;
    VK_CALL(vkCmdCopyAccelerationStructureToMemoryKHR(reader.command, &serialize));
    vkd3d_rtas_metadata_barrier(&reader, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    copy.srcOffset = serialize.dst.deviceAddress - address;
    copy.dstOffset = 0;
    copy.size = sizeof(header);
    copy_info.srcBuffer = reader.serialized_buffer;
    copy_info.dstBuffer = reader.buffer;
    copy_info.regionCount = 1;
    copy_info.pRegions = &copy;
    VK_CALL(vkCmdCopyBuffer2(reader.command, &copy_info));
    vkd3d_rtas_metadata_barrier(&reader, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    if (FAILED(hr = vkd3d_rtas_metadata_submit(&reader, last_submission, quarantine)))
        goto done;
    memcpy(&header, reader.mapped, sizeof(header));
    count = header.NumBottomLevelAccelerationStructurePointersAfterHeader;
    /* KHR_opacity_micromap's public block marker identifies a BLAS, which has
     * zero BLAS references. Its block count is not a DXR BLAS-pointer count.
     * Ordinary BLAS headers have a zero count; TLAS headers carry references. */
    if ((count >> 32) == UINT32_MAX)
        count = 0;
    if (header.SerializedSizeInBytesIncludingHeader != size || !header.DeserializedSizeInBytes ||
            count > (size - sizeof(header)) / sizeof(uint64_t))
    {
        ERR("Invalid public AS serialization query header.\n");
        hr = E_INVALIDARG;
        goto done;
    }
    result.SerializedSizeInBytes = size;
    result.NumBottomLevelAccelerationStructurePointers = count;
    if (FAILED(hr = vkd3d_rtas_metadata_begin(&reader)))
        goto done;
    VK_CALL(vkCmdUpdateBuffer(reader.command, destination->vk_buffer,
            op->dst - destination->va, sizeof(result), &result));
    /* DXR exposes the result as a UAV write. Bridge the helper's transfer
     * access to all suffix consumers, including a subsequent GPU copy. */
    vkd3d_rtas_metadata_barrier(&reader, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    hr = vkd3d_rtas_metadata_submit(&reader, last_submission, quarantine);
    TRACE("AS serialization query src %#"PRIx64", %"PRIu64" bytes, %"PRIu64" references.\n",
            op->src, size, count);
done:
    if (!*quarantine)
        vkd3d_rtas_metadata_reader_cleanup(&reader);
    else
        ERR("AS query resources quarantined after uncertain GPU completion.\n");
    return hr;
}

HRESULT vkd3d_rtas_execute_metadata(struct d3d12_command_queue *queue,
        const struct vkd3d_rtas_metadata *op, uint64_t *last_submission, bool *quarantine)
{
    switch (op->op)
    {
        case VKD3D_RTAS_METADATA_DESERIALIZE:
            return vkd3d_rtas_prepare_deserialization(queue, op, last_submission, quarantine);
        case VKD3D_RTAS_METADATA_SERIALIZATION_QUERY:
            return vkd3d_rtas_serialization_query(queue, op, last_submission, quarantine);
        default:
            return E_INVALIDARG;
    }
}
