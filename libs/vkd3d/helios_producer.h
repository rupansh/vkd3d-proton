/* Included by command.c: exact Helios producer and ECL completion on the
 * existing submission worker. No sampled boundary, worker drain or GPU idle. */
#include "helios_producer_abi.h"

struct vkd3d_helios_binding
{
    struct helios_producer_api_v1 api;
    void *binding;
    uint32_t allocation;
};

struct vkd3d_helios_stream
{
    VkSemaphore semaphore;
    uint64_t cookie;
    uint32_t ctx, value;
};

struct vkd3d_helios_execution
{
    HANDLE admission_event;
    uint64_t value;
};

/* Caller owns queue_lock: stream values are reserved at the same commit that
 * appends work to the FIFO, including concurrent ECL and Present producers. */
static bool vkd3d_helios_ensure_stream_locked(struct d3d12_command_queue *queue,
        const struct helios_producer_api_v1 *api)
{
#ifdef _WIN32
    const struct vkd3d_vk_device_procs *vk_procs = &queue->device->vk_procs;
    struct helios_producer_api_v1 resolved;
    struct vkd3d_helios_stream *stream;
    VkSemaphoreTypeCreateInfo type = {0};
    VkExportSemaphoreCreateInfo export = {0};
    VkSemaphoreCreateInfo create = {0};
    helios_get_producer_api_fn get;
    HMODULE module = NULL;
    if (queue->helios_producer) return true;
    if (!api)
    {
        if (vk_procs->vkGetSemaphoreCounterValue)
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCWSTR)vk_procs->vkGetSemaphoreCounterValue, &module);
        get = module ? (helios_get_producer_api_fn)GetProcAddress(module, "helios_venus_producer_interface") : NULL;
        if (!get || get(HELIOS_PRODUCER_ABI, &resolved) != VK_SUCCESS ||
                resolved.version != HELIOS_PRODUCER_ABI || resolved.size != sizeof(resolved))
            return false;
        api = &resolved;
    }
    if (!(stream = vkd3d_calloc(1, sizeof(*stream)))) return false;
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    export.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    export.pNext = &type;
    export.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create.pNext = &export;
    if (VK_CALL(vkCreateSemaphore(queue->device->vk_device, &create, NULL, &stream->semaphore)) != VK_SUCCESS ||
            api->stream((uintptr_t)queue->device->vk_device, (uint64_t)(uintptr_t)stream->semaphore,
                &stream->ctx, &stream->cookie) != VK_SUCCESS)
    {
        if (stream->semaphore) VK_CALL(vkDestroySemaphore(queue->device->vk_device, stream->semaphore, NULL));
        vkd3d_free(stream);
        return false;
    }
    queue->helios_producer = stream;
    return true;
#else
    (void)queue; (void)api;
    return false;
#endif
}

static bool vkd3d_helios_commit_execute(struct d3d12_command_queue *queue,
        struct d3d12_command_queue_submission *sub, HANDLE admission_event,
        uint32_t *ctx, uint32_t *value, uint64_t *cookie)
{
#ifdef _WIN32
    struct vkd3d_helios_execution *op;
    struct vkd3d_helios_stream *stream;
    bool committed = false;
    if (!(op = vkd3d_calloc(1, sizeof(*op)))) return false;
    if (!DuplicateHandle(GetCurrentProcess(), admission_event, GetCurrentProcess(),
            &op->admission_event, SYNCHRONIZE, FALSE, 0))
    {
        vkd3d_free(op);
        return false;
    }
    if (d3d12_device_use_embedded_mutable_descriptors(queue->device))
        vkd3d_memcpy_non_temporal_barrier();
    pthread_mutex_lock(&queue->queue_lock);
    if (!vkd3d_atomic_uint32_load_explicit(&queue->helios_execution_cancelled, vkd3d_memory_order_acquire) &&
            vkd3d_helios_ensure_stream_locked(queue, NULL) &&
            queue->helios_producer->value != UINT32_MAX &&
            vkd3d_array_reserve((void **)&queue->submissions, &queue->submissions_size,
                queue->submissions_count + 1, sizeof(*queue->submissions)))
    {
        stream = queue->helios_producer;
        op->value = ++stream->value;
        sub->execute.helios_execution = op;
        /* Outputs are written before the worker can own/free op. */
        *ctx = stream->ctx; *value = (uint32_t)op->value; *cookie = stream->cookie;
        d3d12_command_queue_add_submission_locked(queue, sub);
        committed = true;
    }
    pthread_mutex_unlock(&queue->queue_lock);
    if (!committed)
    {
        CloseHandle(op->admission_event);
        vkd3d_free(op);
    }
    return committed;
#else
    (void)queue; (void)sub; (void)admission_event; (void)ctx; (void)value; (void)cookie;
    return false;
#endif
}

void helios_vkd3d_cancel_execution(ID3D12CommandQueue *iface, HRESULT reason)
{
    struct d3d12_command_queue *queue = impl_from_ID3D12CommandQueue(iface);
    vkd3d_atomic_uint32_store_explicit(&queue->helios_execution_cancelled, 1, vkd3d_memory_order_release);
    if (FAILED(reason))
        d3d12_device_mark_as_removed(queue->device, reason, "Runtime execution submission failed.");
}

static bool vkd3d_helios_execution_wait(struct d3d12_command_queue *queue,
        struct vkd3d_helios_execution *op)
{
    if (!op) return true;
#ifdef _WIN32
    for (;;)
    {
        DWORD result;
        if (FAILED(d3d12_device_removed_reason(queue->device))) break;
        /* Ordinary context destruction drains before setting cancellation.
         * Failed submission/device teardown may cancel pending admissions. */
        result = WaitForSingleObject(op->admission_event, 0);
        if (result == WAIT_OBJECT_0) return true;
        if (result != WAIT_TIMEOUT ||
                vkd3d_atomic_uint32_load_explicit(&queue->helios_execution_cancelled, vkd3d_memory_order_acquire))
            break;
        /* No Vulkan queue lock is held here. Other queues can satisfy a runtime
         * wait-before-signal. A timeout checks cancellation; it is never success. */
        result = WaitForSingleObject(op->admission_event, 100);
        if (result == WAIT_OBJECT_0) return true;
        if (result != WAIT_TIMEOUT) break;
    }
#else
    (void)op;
#endif
    d3d12_device_mark_as_removed(queue->device, DXGI_ERROR_DEVICE_REMOVED,
            "Runtime queue admission cancelled or failed.");
    return false;
}

static void vkd3d_helios_execution_signal(struct d3d12_command_queue *queue,
        struct vkd3d_helios_execution *op)
{
    const struct vkd3d_vk_device_procs *vk_procs = &queue->device->vk_procs;
    VkSemaphoreSubmitInfo signals[2] = {{0}, {0}};
    VkSubmitInfo2 submit = {0};
    VkQueue vk_queue;
    VkResult vr = VK_ERROR_DEVICE_LOST;
    if (!op) return;
    /* This also joins an engine fallback copy submission. It is the same
     * worker ordering used by QUEUE_USING_CALLBACK, with no worker drain. */
    d3d12_command_queue_flush_waiters(queue, VKD3D_WAIT_SEMAPHORES_EXTERNAL | VKD3D_WAIT_SEMAPHORES_SERIALIZING);
    if (FAILED(d3d12_device_removed_reason(queue->device))) return;
    if ((vk_queue = vkd3d_queue_acquire(queue->vkd3d_queue)))
    {
        signals[0].sType = signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[0].stageMask = signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals[0].semaphore = queue->helios_producer->semaphore;
        signals[0].value = op->value;
        signals[1].semaphore = queue->vkd3d_queue->submission_timeline;
        signals[1].value = ++queue->vkd3d_queue->submission_timeline_count;
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.signalSemaphoreInfoCount = 2;
        submit.pSignalSemaphoreInfos = signals;
        vr = VK_CALL(vkQueueSubmit2(vk_queue, 1, &submit,
                vkd3d_queue_get_signal_fence_proxy_locked(queue->vkd3d_queue)));
        if (vr == VK_SUCCESS) queue->last_submission_timeline_value = signals[1].value;
        vkd3d_queue_release(queue->vkd3d_queue);
    }
    if (vr != VK_SUCCESS)
        d3d12_device_mark_as_removed(queue->device, hresult_from_vk_result(vr),
                "Exact execution completion signal failed, vr %d.", vr);
}

static void vkd3d_helios_execution_free(struct vkd3d_helios_execution *op)
{
    if (!op) return;
#ifdef _WIN32
    CloseHandle(op->admission_event);
#endif
    vkd3d_free(op);
}

struct vkd3d_helios_operation
{
    struct d3d12_command_queue *queue;
    struct d3d12_resource *resource;
    struct vkd3d_helios_binding *binding;
    struct vkd3d_helios_execution execution;
    bool submitted;
};

void vkd3d_helios_binding_cleanup(struct d3d12_resource *resource)
{
    struct vkd3d_helios_binding *binding = resource->helios_producer;
    if (!binding) return;
    binding->api.release(binding->binding);
    vkd3d_free(binding);
}

void vkd3d_helios_queue_cleanup(struct d3d12_command_queue *queue)
{
    const struct vkd3d_vk_device_procs *vk_procs = &queue->device->vk_procs;
    if (!queue->helios_producer) return;
    /* Release joins the submission and fence workers before reaching here. */
    VK_CALL(vkDestroySemaphore(queue->device->vk_device, queue->helios_producer->semaphore, NULL));
    vkd3d_free(queue->helios_producer);
}

static void vkd3d_helios_operation_done(struct vkd3d_fence_worker *worker, void *userdata, bool complete)
{
    struct vkd3d_helios_operation *op = *(struct vkd3d_helios_operation **)userdata;
    (void)worker;
    if (!complete)
    {
        op->binding->api.abort(op->binding->binding);
        ERR("Helios producer operation failed; submitted=%u.\n", op->submitted);
    }
    /* An unproven submitted read/write retains its resource on device loss,
     * matching the engine's pending-resource safety rule. Never fake retirement. */
    if (complete || !op->submitted)
        d3d12_resource_decref(op->resource);
#ifdef _WIN32
    CloseHandle(op->execution.admission_event);
#endif
    vkd3d_free(op);
}

static void vkd3d_helios_operation_submit(void *userdata)
{
    struct vkd3d_helios_operation *op = userdata;
    struct d3d12_command_queue *queue = op->queue;
    const struct vkd3d_vk_device_procs *vk_procs = &queue->device->vk_procs;
    struct vkd3d_fence_wait_info wait = {0};
    VkSemaphoreSubmitInfo signals[2] = {{0}, {0}};
    VkSubmitInfo2 submit = {0};
    VkQueue vk_queue;
    VkResult vr = VK_ERROR_DEVICE_LOST;
    HRESULT hr;

    if (!vkd3d_helios_execution_wait(queue, &op->execution))
    {
        vkd3d_helios_operation_done(NULL, &op, false);
        return;
    }

    /* CALLBACK already flushed external and serializing waiters. This lock
     * only excludes concurrent users of this exact Vulkan queue; it never
     * recursively drains the worker. Both split-submit and prior-wait work
     * precede this ALL_COMMANDS signal and the engine's own timeline marker. */
    vk_queue = vkd3d_queue_acquire(queue->vkd3d_queue);
    if (vk_queue)
    {
        signals[0].sType = signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[0].stageMask = signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals[0].semaphore = queue->helios_producer->semaphore;
        signals[0].value = op->execution.value;
        signals[1].semaphore = queue->vkd3d_queue->submission_timeline;
        signals[1].value = ++queue->vkd3d_queue->submission_timeline_count;
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.signalSemaphoreInfoCount = 2;
        submit.pSignalSemaphoreInfos = signals;
        if (d3d12_device_removed_reason(queue->device) == S_OK)
            vr = VK_CALL(vkQueueSubmit2(vk_queue, 1, &submit,
                    vkd3d_queue_get_signal_fence_proxy_locked(queue->vkd3d_queue)));
        if (vr == VK_SUCCESS)
            queue->last_submission_timeline_value = signals[1].value;
        vkd3d_queue_release(queue->vkd3d_queue);
    }
    if (vr != VK_SUCCESS)
    {
        d3d12_device_mark_as_removed(queue->device, hresult_from_vk_result(vr),
                "Helios producer signal failed, vr %d.", vr);
        vkd3d_helios_operation_done(NULL, &op, false);
        return;
    }
    op->submitted = true;
    wait.vk_semaphore = queue->helios_producer->semaphore;
    wait.vk_semaphore_value = op->execution.value;
    *(struct vkd3d_helios_operation **)vkd3d_waiting_fence_set_callback(&wait,
            vkd3d_helios_operation_done, sizeof(op)) = op;
    /* This worker callback is lifetime cleanup only. KMD completion follows
     * the ICD's registered ring boundary, never this user-mode notification. */
    hr = vkd3d_enqueue_timeline_semaphore(&queue->fence_worker, &wait, NULL);
    if (FAILED(hr)) ERR("Helios producer cleanup enqueue failed, hr %#x.\n", (unsigned int)hr);
}

HRESULT helios_vkd3d_enqueue_producer(ID3D12CommandQueue *iface, ID3D12Resource *resource_iface,
        uint32_t allocation, HANDLE admission_event, uint32_t *ctx, uint32_t *value, uint64_t *cookie)
{
#ifdef _WIN32
    struct d3d12_command_queue *queue;
    struct d3d12_resource *resource;
    struct vkd3d_helios_binding *binding;
    struct vkd3d_helios_stream *stream;
    struct vkd3d_helios_operation *op = NULL;
    struct d3d12_command_queue_submission sub = {0};
    helios_get_producer_api_fn get;
    uint64_t epoch;
    HRESULT hr = E_FAIL;
    HMODULE module;
    const struct vkd3d_vk_device_procs *vk_procs;
    if (!iface || !resource_iface || !allocation || !admission_event || !ctx || !value || !cookie)
        return E_INVALIDARG;
    *ctx = *value = 0; *cookie = 0;
    queue = impl_from_ID3D12CommandQueue(iface);
    resource = impl_from_ID3D12Resource(resource_iface);
    if (queue->device != resource->device) return E_INVALIDARG;
    if (FAILED(hr = d3d12_device_removed_reason(queue->device))) return hr;
    hr = E_FAIL;
    vk_procs = &queue->device->vk_procs;
    /* The installed ICD has a content-hashed name. The live device pins its
     * dispatch module; do not choose an ICD by basename or module list order.
     * An intervening layer without the interface fails explicitly. */
    module = NULL;
    if (vk_procs->vkGetSemaphoreCounterValue)
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)vk_procs->vkGetSemaphoreCounterValue, &module);
    get = module ? (helios_get_producer_api_fn)GetProcAddress(module, "helios_venus_producer_interface") : NULL;
    if (!get) return E_NOINTERFACE;

    /* Each queue has its own increasing stream. Resource initialization is
     * serialized by its existing private-data mutex across different queues. */
    pthread_mutex_lock(&queue->queue_lock);
    if (vkd3d_atomic_uint32_load_explicit(&queue->helios_execution_cancelled, vkd3d_memory_order_acquire))
        goto unlock_queue;
    pthread_mutex_lock(&resource->private_store.mutex);
    binding = resource->helios_producer;
    if (!binding)
    {
        if (!(binding = vkd3d_calloc(1, sizeof(*binding)))) goto unlock_resource;
        if (get(HELIOS_PRODUCER_ABI, &binding->api) != VK_SUCCESS ||
            binding->api.version != HELIOS_PRODUCER_ABI || binding->api.size != sizeof(binding->api) ||
            binding->api.bind((uintptr_t)queue->device->vk_device, allocation, &binding->binding) != VK_SUCCESS)
        {
            vkd3d_free(binding);
            goto unlock_resource;
        }
        binding->allocation = allocation;
        resource->helios_producer = binding;
    }
    if (binding->allocation != allocation) goto unlock_resource;
    pthread_mutex_unlock(&resource->private_store.mutex);

    if (!vkd3d_helios_ensure_stream_locked(queue, &binding->api)) goto fail;
    stream = queue->helios_producer;
    if (stream->value == UINT32_MAX || !(op = vkd3d_calloc(1, sizeof(*op)))) goto fail;
    if (!DuplicateHandle(GetCurrentProcess(), admission_event, GetCurrentProcess(),
            &op->execution.admission_event, SYNCHRONIZE, FALSE, 0))
    {
        vkd3d_free(op);
        goto fail;
    }
    /* Reserve before committing, since the generic enqueue assumes allocation
     * succeeds. The second reserve in add_submission_locked cannot allocate. */
    if (!vkd3d_array_reserve((void **)&queue->submissions, &queue->submissions_size,
            queue->submissions_count + 1, sizeof(*queue->submissions)))
    {
        CloseHandle(op->execution.admission_event);
        vkd3d_free(op);
        goto fail;
    }
    op->queue = queue;
    op->resource = resource;
    op->binding = binding;
    op->execution.value = ++stream->value;
    d3d12_resource_incref(resource);
    sub.type = VKD3D_SUBMISSION_QUEUE_USING_CALLBACK;
    sub.callback.callback = vkd3d_helios_operation_submit;
    sub.callback.userdata = op;
    d3d12_command_queue_add_submission_locked(queue, &sub);
    /* Already committed to the FIFO. Publication and enqueue share this lock;
     * Release puts STOP after this callback and joins before freeing the queue.
     * No COM self-reference can make the worker join itself on its last release. */
    if (binding->api.publish(binding->binding, (uint64_t)(uintptr_t)stream->semaphore,
            op->execution.value, &epoch) != VK_SUCCESS) goto fail;
    *ctx = stream->ctx; *value = (uint32_t)op->execution.value; *cookie = stream->cookie;
    hr = S_OK;
    goto unlock_queue;
fail:
    binding->api.abort(binding->binding);
    ERR("Helios producer publication failed.\n");
    goto unlock_queue;
unlock_resource:
    pthread_mutex_unlock(&resource->private_store.mutex);
unlock_queue:
    pthread_mutex_unlock(&queue->queue_lock);
    return hr;
#else
    (void)iface; (void)resource_iface; (void)allocation; (void)admission_event; (void)ctx; (void)value; (void)cookie;
    return E_NOTIMPL;
#endif
}
