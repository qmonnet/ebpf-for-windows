// Copyright (c) Microsoft Corporation
// SPDX-License-Identifier: MIT

/**
 * @brief WDF based driver that does the following:
 * Registers as an eBPF extension program information provider and hook provider.
 */

#include <ntddk.h>
#include <wdf.h>

#include "ebpf_platform.h"

#include "sample_ext.h"
#include "sample_ext_helpers.h"
#include "sample_ext_ioctls.h"

#define SAMPLE_EBPF_EXT_DEVICE_NAME L"\\Device\\" SAMPLE_EBPF_EXT_NAME_W
#define SAMPLE_EBPF_EXT_SYMBOLIC_DEVICE_NAME L"\\GLOBAL??\\" SAMPLE_EBPF_EXT_DEVICE_BASE_NAME

#define SAMPLE_PID_TGID_VALUE 9999

// Driver global variables
static DEVICE_OBJECT* _sample_ebpf_ext_driver_device_object;
static BOOLEAN _sample_ebpf_ext_driver_unloading_flag = FALSE;

//
// Pre-Declarations
//
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL _sample_ebpf_ext_driver_io_device_control;
DRIVER_INITIALIZE DriverEntry;

_Ret_notnull_ DEVICE_OBJECT*
ebpf_driver_get_device_object()
{
    return _sample_ebpf_ext_driver_device_object;
}

static void
_sample_ebpf_ext_driver_io_device_control(
    _In_ const WDFQUEUE queue,
    _In_ const WDFREQUEST request,
    size_t output_buffer_length,
    size_t input_buffer_length,
    ULONG io_control_code);

static _Function_class_(EVT_WDF_DRIVER_UNLOAD) _IRQL_requires_same_
    _IRQL_requires_max_(PASSIVE_LEVEL) void _sample_ebpf_ext_driver_unload(_In_ const WDFDRIVER driver_object)
{
    UNREFERENCED_PARAMETER(driver_object);

    _sample_ebpf_ext_driver_unloading_flag = TRUE;

    sample_ebpf_extension_program_info_provider_unregister();

    sample_ebpf_extension_hook_provider_unregister();
}

//
// Create a basic WDF driver, set up the device object
// for a callout driver and register with NMR.
//
static NTSTATUS
_sample_ebpf_ext_driver_initialize_objects(
    _Inout_ DRIVER_OBJECT* driver_object,
    _In_ const UNICODE_STRING* registry_path,
    _Out_ WDFDRIVER* driver,
    _Out_ WDFDEVICE* device)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG driver_configuration;
    PWDFDEVICE_INIT device_initialize = NULL;
    WDF_IO_QUEUE_CONFIG io_queue_configuration;
    UNICODE_STRING sample_ebpf_ext_device_name;
    UNICODE_STRING sample_ebpf_ext_symbolic_device_name;
    BOOLEAN device_create_flag = FALSE;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_FILEOBJECT_CONFIG file_object_config;

    WDF_DRIVER_CONFIG_INIT(&driver_configuration, WDF_NO_EVENT_CALLBACK);

    driver_configuration.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    driver_configuration.EvtDriverUnload = _sample_ebpf_ext_driver_unload;

    status = WdfDriverCreate(driver_object, registry_path, WDF_NO_OBJECT_ATTRIBUTES, &driver_configuration, driver);

    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    device_initialize = WdfControlDeviceInitAllocate(
        *driver,
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL // only kernel/system and administrators.
    );
    if (!device_initialize) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    WdfDeviceInitSetDeviceType(device_initialize, FILE_DEVICE_NETWORK);

    WdfDeviceInitSetCharacteristics(device_initialize, FILE_DEVICE_SECURE_OPEN, FALSE);

    WdfDeviceInitSetCharacteristics(device_initialize, FILE_AUTOGENERATED_DEVICE_NAME, TRUE);

    RtlInitUnicodeString(&sample_ebpf_ext_device_name, SAMPLE_EBPF_EXT_DEVICE_NAME);
    status = WdfDeviceInitAssignName(device_initialize, &sample_ebpf_ext_device_name);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WDF_FILEOBJECT_CONFIG_INIT(
        &file_object_config,
        NULL,
        NULL,
        WDF_NO_EVENT_CALLBACK // No cleanup callback function.
    );
    WdfDeviceInitSetFileObjectConfig(device_initialize, &file_object_config, &attributes);

    status = WdfDeviceCreate(&device_initialize, WDF_NO_OBJECT_ATTRIBUTES, device);

    if (!NT_SUCCESS(status)) {
        // do not free if any other call
        // after WdfDeviceCreate fails.
        WdfDeviceInitFree(device_initialize);
        device_initialize = NULL;
        goto Exit;
    }

    device_create_flag = TRUE;

    // Create symbolic link for control object for user mode.
    RtlInitUnicodeString(&sample_ebpf_ext_symbolic_device_name, SAMPLE_EBPF_EXT_SYMBOLIC_DEVICE_NAME);
    status = WdfDeviceCreateSymbolicLink(*device, &sample_ebpf_ext_symbolic_device_name);

    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    // Parallel default queue.
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&io_queue_configuration, WdfIoQueueDispatchParallel);

    io_queue_configuration.EvtIoDeviceControl = _sample_ebpf_ext_driver_io_device_control;

    status = WdfIoQueueCreate(
        *device,
        &io_queue_configuration,
        WDF_NO_OBJECT_ATTRIBUTES,
        WDF_NO_HANDLE // Pointer to default queue.
    );
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    status = sample_ebpf_extension_program_info_provider_register();
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    status = sample_ebpf_extension_hook_provider_register();
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    WdfControlFinishInitializing(*device);

Exit:
    if (!NT_SUCCESS(status)) {
        if (device_create_flag && device != NULL) {
            //
            // Release the reference on the newly created object, since
            // we couldn't initialize it.
            //
            WdfObjectDelete(*device);
        }
    }
    return status;
}

NTSTATUS
DriverEntry(_In_ DRIVER_OBJECT* driver_object, _In_ UNICODE_STRING* registry_path)
{
    NTSTATUS status;
    WDFDRIVER driver;
    WDFDEVICE device;

    // Request NX Non-Paged Pool when available.
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "sample_ebpf_ext: DriverEntry\n"));

    status = _sample_ebpf_ext_driver_initialize_objects(driver_object, registry_path, &driver, &device);

    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    _sample_ebpf_ext_driver_device_object = WdfDeviceWdmGetDeviceObject(device);

Exit:

    return status;
}

static VOID
_sample_ebpf_ext_driver_io_device_control(
    _In_ const WDFQUEUE queue,
    _In_ const WDFREQUEST request,
    size_t output_buffer_length,
    size_t input_buffer_length,
    ULONG io_control_code)
{
    NTSTATUS status = STATUS_SUCCESS;
    WDFDEVICE device;
    void* input_buffer = NULL;
    void* output_buffer = NULL;
    size_t actual_input_length = 0;
    size_t actual_output_length = 0;
    sample_program_context_t program_context = {0};
    uint32_t program_result = 0;
    ebpf_result_t result = EBPF_INVALID_ARGUMENT;

    device = WdfIoQueueGetDevice(queue);

    switch (io_control_code) {
    case IOCTL_SAMPLE_EBPF_EXT_CTL_RUN:
        if (input_buffer_length != 0) {
            // Retrieve the input buffer associated with the request object.
            status = WdfRequestRetrieveInputBuffer(
                request,             // Request object.
                input_buffer_length, // Length of input buffer.
                &input_buffer,       // Pointer to buffer.
                &actual_input_length // Length of buffer.
            );

            if (!NT_SUCCESS(status)) {
                KdPrintEx(
                    (DPFLTR_IHVDRIVER_ID,
                     DPFLTR_INFO_LEVEL,
                     "%s: Input buffer failure %d\n",
                     SAMPLE_EBPF_EXT_NAME_A,
                     status));
                goto Done;
            }

            if (input_buffer == NULL) {
                status = STATUS_INVALID_PARAMETER;
                goto Done;
            }

            if (input_buffer != NULL) {
                size_t minimum_request_size = 0;
                size_t minimum_reply_size = actual_input_length;

                if (actual_input_length < minimum_request_size) {
                    status = STATUS_INVALID_PARAMETER;
                    goto Done;
                }

                // Be aware: Input and output buffer point to the same memory.
                if (minimum_reply_size > 0) {
                    // Retrieve output buffer associated with the request object.
                    status = WdfRequestRetrieveOutputBuffer(
                        request, output_buffer_length, &output_buffer, &actual_output_length);
                    if (!NT_SUCCESS(status)) {
                        KdPrintEx(
                            (DPFLTR_IHVDRIVER_ID,
                             DPFLTR_INFO_LEVEL,
                             "%s: Output buffer failure %d\n",
                             SAMPLE_EBPF_EXT_NAME_A,
                             status));
                        goto Done;
                    }
                    if (output_buffer == NULL) {
                        status = STATUS_INVALID_PARAMETER;
                        goto Done;
                    }

                    if (actual_output_length < minimum_reply_size) {
                        status = STATUS_BUFFER_TOO_SMALL;
                        goto Done;
                    }
                }

                // Invoke the eBPF program. Pass the output buffer as program context data.
                program_context.data_start = output_buffer;
                program_context.data_end = (uint8_t*)output_buffer + output_buffer_length;
                program_context.pid_tgid = SAMPLE_PID_TGID_VALUE;
                result = sample_ebpf_extension_invoke_program(&program_context, &program_result);
            }
        } else {
            status = STATUS_INVALID_PARAMETER;
            goto Done;
        }
        break;
    case IOCTL_SAMPLE_EBPF_EXT_CTL_PROFILE: {
        size_t minimum_request_size = sizeof(sample_ebpf_ext_profile_request_t);
        size_t minimum_reply_size = sizeof(sample_ebpf_ext_profile_reply_t);
        sample_ebpf_ext_profile_request_t* profile_request;
        sample_ebpf_ext_profile_reply_t* profile_reply;

        if (input_buffer_length == 0) {
            status = STATUS_INVALID_PARAMETER;
            goto Done;
        }
        // Retrieve the input buffer associated with the request object.
        status = WdfRequestRetrieveInputBuffer(
            request,             // Request object.
            input_buffer_length, // Length of input buffer.
            &input_buffer,       // Pointer to buffer.
            &actual_input_length // Length of buffer.
        );

        if (!NT_SUCCESS(status)) {
            KdPrintEx(
                (DPFLTR_IHVDRIVER_ID,
                 DPFLTR_INFO_LEVEL,
                 "%s: Input buffer failure %d\n",
                 SAMPLE_EBPF_EXT_NAME_A,
                 status));
            goto Done;
        }

        if (input_buffer == NULL) {
            status = STATUS_INVALID_PARAMETER;
            goto Done;
        }

        if (actual_input_length < minimum_request_size) {
            status = STATUS_INVALID_PARAMETER;
            goto Done;
        }

        // Retrieve output buffer associated with the request object.
        status = WdfRequestRetrieveOutputBuffer(request, output_buffer_length, &output_buffer, &actual_output_length);
        if (!NT_SUCCESS(status)) {
            KdPrintEx(
                (DPFLTR_IHVDRIVER_ID,
                 DPFLTR_INFO_LEVEL,
                 "%s: Output buffer failure %d\n",
                 SAMPLE_EBPF_EXT_NAME_A,
                 status));
            goto Done;
        }

        if (output_buffer == NULL) {
            status = STATUS_INVALID_PARAMETER;
            goto Done;
        }

        if (actual_output_length < minimum_reply_size) {
            status = STATUS_BUFFER_TOO_SMALL;
            goto Done;
        }

        profile_request = input_buffer;
        profile_reply = output_buffer;

        result = sample_ebpf_extension_profile_program(profile_request, actual_input_length, profile_reply);

        break;
    }
    default:
        result = EBPF_INVALID_ARGUMENT;
        break;
    }

    if (NT_SUCCESS(status) && (result != EBPF_SUCCESS)) {
        status = STATUS_UNSUCCESSFUL;
        goto Done;
    }

Done:
    WdfRequestCompleteWithInformation(request, status, output_buffer_length);
    return;
}
