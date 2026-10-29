/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Experimental native Windows driver boundary for Wine Vulkan. */
#include "vulkan_private.h"
#include <string.h>
#include <stdio.h>

/* Prototype driver boundary. All public entry points, argument conversions,
 * object handles and ordinary command dispatch are Wine's implementation. */
static HMODULE system_loader;
static PFN_vkGetInstanceProcAddr host_gipa;
static PFN_vkGetDeviceProcAddr host_gdpa;
static PFN_vkGetPhysicalDeviceProperties host_properties;
static struct vulkan_funcs driver;
struct native_instance {struct vulkan_instance obj; SRWLOCK lock; struct rb_tree objects;};
static BOOL same_file(const WCHAR *expected,const WCHAR *actual)
{
    HANDLE a=CreateFileW(expected,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    HANDLE b=CreateFileW(actual,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    BY_HANDLE_FILE_INFORMATION ia,ib;BOOL equal=FALSE;
    if(a!=INVALID_HANDLE_VALUE&&b!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(a,&ia)&&GetFileInformationByHandle(b,&ib))equal=ia.dwVolumeSerialNumber==ib.dwVolumeSerialNumber&&ia.nFileIndexHigh==ib.nFileIndexHigh&&ia.nFileIndexLow==ib.nFileIndexLow;
    if(a!=INVALID_HANDLE_VALUE)CloseHandle(a);if(b!=INVALID_HANDLE_VALUE)CloseHandle(b);return equal;
}

static int object_compare(const void *key,const struct rb_entry *entry)
{
    const struct vulkan_object *object=RB_ENTRY_VALUE(entry,struct vulkan_object,entry);
    uint64_t value=*(const uint64_t *)key;
    return value<object->host_handle?-1:value>object->host_handle;
}
static void insert_object(struct vulkan_instance *instance,struct vulkan_object *object)
{
    struct native_instance *impl=(void *)instance;
    AcquireSRWLockExclusive(&impl->lock);rb_put(&impl->objects,&object->host_handle,&object->entry);ReleaseSRWLockExclusive(&impl->lock);
}
static void remove_object(struct vulkan_instance *instance,struct vulkan_object *object)
{
    struct native_instance *impl=(void *)instance;
    AcquireSRWLockExclusive(&impl->lock);rb_remove(&impl->objects,&object->entry);ReleaseSRWLockExclusive(&impl->lock);
}
static uint64_t find_object(struct vulkan_instance *instance,uint64_t handle)
{
    struct native_instance *impl=(void *)instance;struct rb_entry *entry;uint64_t result=0;
    AcquireSRWLockShared(&impl->lock);entry=rb_get(&impl->objects,&handle);if(entry)result=RB_ENTRY_VALUE(entry,struct vulkan_object,entry)->client_handle;ReleaseSRWLockShared(&impl->lock);return result;
}
static VkResult VKAPI_CALL enumerate_version(uint32_t *version){*version=VK_API_VERSION_1_0;return VK_SUCCESS;}
static VkResult VKAPI_CALL enumerate_extensions(const char *layer,uint32_t *count,VkExtensionProperties *out)
{
    PFN_vkEnumerateInstanceExtensionProperties enumerate=(void *)host_gipa(NULL,"vkEnumerateInstanceExtensionProperties");
    uint32_t total=0,capacity=out?*count:0,written=0;VkResult result;VkExtensionProperties *properties;
    if((result=enumerate(layer,&total,NULL)))return result;if(total>4096)return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(!(properties=calloc(total?total:1,sizeof(*properties))))return VK_ERROR_OUT_OF_HOST_MEMORY;
    if((result=enumerate(layer,&total,properties))){free(properties);return result;}
    for(uint32_t i=0;i<total;i++){
        /* Wine's public loader synthesizes this client-platform extension. */
        if(!strcmp(properties[i].extensionName,VK_KHR_WIN32_SURFACE_EXTENSION_NAME))continue;
        if(out&&written<capacity)out[written]=properties[i];written++;
    }
    free(properties);*count=out?min(written,capacity):written;return out&&written>capacity?VK_INCOMPLETE:VK_SUCCESS;
}
static BOOL implemented_driver_entry(const char *function_name)
{
#define USE_NATIVE_ENTRY(name) if(!strcmp(function_name,#name)&&!driver.p_##name)return FALSE;
#include "native-entries.h"
#undef USE_NATIVE_ENTRY
    return TRUE;
}
static void VKAPI_CALL physical_properties(VkPhysicalDevice physical,VkPhysicalDeviceProperties *properties)
{host_properties(physical,properties);properties->apiVersion=min(properties->apiVersion,VK_API_VERSION_1_0);}
static PFN_vkVoidFunction VKAPI_CALL get_instance_proc(VkInstance instance,const char *name)
{if(!instance&&!strcmp(name,"vkEnumerateInstanceVersion"))return (PFN_vkVoidFunction)enumerate_version;if(!instance&&!strcmp(name,"vkEnumerateInstanceExtensionProperties"))return (PFN_vkVoidFunction)enumerate_extensions;return implemented_driver_entry(name)?host_gipa(instance,name):NULL;}
static PFN_vkVoidFunction VKAPI_CALL get_device_proc(VkDevice device,const char *name)
{return implemented_driver_entry(name)?host_gdpa(device,name):NULL;}

static VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *info,const VkAllocationCallbacks *allocator,VkInstance *out)
{
    struct native_instance *impl;struct vulkan_instance *instance;VkInstance client=*out,host=0;
    VkPhysicalDevice *physical=NULL;uint32_t count=0;VkResult result;
    if(info->pApplicationInfo&&info->pApplicationInfo->apiVersion>VK_API_VERSION_1_0)return VK_ERROR_INCOMPATIBLE_DRIVER;
    result=((PFN_vkCreateInstance)host_gipa(NULL,"vkCreateInstance"))(info,NULL,&host);if(result)return result;
    PFN_vkEnumeratePhysicalDevices enumerate=(void *)host_gipa(host,"vkEnumeratePhysicalDevices");
    PFN_vkDestroyInstance destroy=(void *)host_gipa(host,"vkDestroyInstance");
    if((result=enumerate(host,&count,NULL)))goto failed;
    if(count>client->physical_device_count){client->physical_device_count=count;result=VK_INCOMPLETE;goto failed;}
    if(!(physical=calloc(count?count:1,sizeof(*physical)))){result=VK_ERROR_OUT_OF_HOST_MEMORY;goto failed;}
    if((result=enumerate(host,&count,physical)))goto failed;
    if(!(impl=calloc(1,sizeof(*impl)+count*sizeof(struct vulkan_physical_device)))){result=VK_ERROR_OUT_OF_HOST_MEMORY;goto failed;}
    instance=&impl->obj;rb_init(&impl->objects,object_compare);InitializeSRWLock(&impl->lock);
    vulkan_object_init_ptr(&instance->obj,(UINT_PTR)host,&client->obj);instance->extensions=client->extensions;
    instance->p_insert_object=insert_object;instance->p_remove_object=remove_object;instance->p_client_handle_from_host=find_object;
#define USE_VK_FUNC(name) instance->p_##name=(void *)host_gipa(host,#name);
    ALL_VK_INSTANCE_FUNCS
#undef USE_VK_FUNC
    instance->p_vkGetPhysicalDeviceProperties=physical_properties;
    instance->physical_devices=(void *)(impl+1);instance->physical_device_count=count;client->physical_device_count=count;
    for(uint32_t i=0;i<count;i++){
        struct vulkan_physical_device *device=&instance->physical_devices[i];VkExtensionProperties *extensions=NULL;uint32_t extension_count=0;
        vulkan_object_init_ptr(&device->obj,(UINT_PTR)physical[i],&client->physical_device[i].obj);device->instance=instance;
        instance->p_vkGetPhysicalDeviceMemoryProperties(physical[i],&device->memory_properties);
        if(!instance->p_vkEnumerateDeviceExtensionProperties(physical[i],NULL,&extension_count,NULL)&&extension_count&&(extensions=calloc(extension_count,sizeof(*extensions)))){
            if(!instance->p_vkEnumerateDeviceExtensionProperties(physical[i],NULL,&extension_count,extensions))for(uint32_t j=0;j<extension_count;j++){
                if(!strcmp(extensions[j].extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME))device->extensions.has_VK_KHR_swapchain=1;
                if(!strcmp(extensions[j].extensionName,VK_KHR_MAINTENANCE1_EXTENSION_NAME))device->extensions.has_VK_KHR_maintenance1=1;
                if(!strcmp(extensions[j].extensionName,VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME))device->extensions.has_VK_KHR_shader_draw_parameters=1;
                if(!strcmp(extensions[j].extensionName,VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))device->extensions.has_VK_EXT_vertex_attribute_divisor=1;
            }
            free(extensions);
        }
        client->physical_device[i].extensions=device->extensions;insert_object(instance,&device->obj);
    }
    insert_object(instance,&instance->obj);free(physical);return VK_SUCCESS;
failed:free(physical);destroy(host,NULL);return result;
}
static void VKAPI_CALL destroy_instance(VkInstance client,const VkAllocationCallbacks *allocator)
{
    if(!client)return;struct vulkan_instance *instance=vulkan_instance_from_handle(client);
    instance->p_vkDestroyInstance(instance->host.instance,NULL);free(instance);
}
static VkResult VKAPI_CALL create_device(VkPhysicalDevice physical,const VkDeviceCreateInfo *info,const VkAllocationCallbacks *allocator,VkDevice *out)
{
    struct vulkan_physical_device *pd=vulkan_physical_device_from_handle(physical);struct vulkan_instance *instance=pd->instance;
    struct vulkan_device *device;VkDevice client=*out,host;uint32_t count=0;VkResult result;
    for(uint32_t i=0;i<info->queueCreateInfoCount;i++)count+=info->pQueueCreateInfos[i].queueCount;
    if(!(device=calloc(1,offsetof(struct vulkan_device,queues[count]))))return VK_ERROR_OUT_OF_HOST_MEMORY;
    result=instance->p_vkCreateDevice(pd->host.physical_device,info,NULL,&host);if(result){free(device);return result;}
    vulkan_object_init_ptr(&device->obj,(UINT_PTR)host,&client->obj);device->physical_device=pd;device->extensions=client->extensions;
#define USE_VK_FUNC(name) device->p_##name=(void *)host_gdpa(host,#name);
    ALL_VK_DEVICE_FUNCS
#undef USE_VK_FUNC
    for(uint32_t i=0;i<info->queueCreateInfoCount;i++)for(uint32_t j=0;j<info->pQueueCreateInfos[i].queueCount;j++){
        uint32_t index=device->queue_count++;struct vulkan_queue *q=&device->queues[index];VkQueue host_queue;
        q->info.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;q->info.flags=info->pQueueCreateInfos[i].flags;q->info.queueFamilyIndex=info->pQueueCreateInfos[i].queueFamilyIndex;q->info.queueIndex=j;
        device->p_vkGetDeviceQueue(host,q->info.queueFamilyIndex,j,&host_queue);vulkan_object_init_ptr(&q->obj,(UINT_PTR)host_queue,&client->queues[index].obj);q->device=device;insert_object(instance,&q->obj);
    }
    insert_object(instance,&device->obj);return VK_SUCCESS;
}
static void VKAPI_CALL destroy_device(VkDevice client,const VkAllocationCallbacks *allocator)
{
    if(!client)return;struct vulkan_device *d=vulkan_device_from_handle(client);struct vulkan_instance *instance=d->physical_device->instance;
    d->p_vkDestroyDevice(d->host.device,NULL);for(uint32_t i=0;i<d->queue_count;i++)remove_object(instance,&d->queues[i].obj);remove_object(instance,&d->obj);free(d);
}
static void VKAPI_CALL get_queue(VkDevice client,uint32_t family,uint32_t index,VkQueue *queue)
{
    struct vulkan_device *d=vulkan_device_from_handle(client);*queue=VK_NULL_HANDLE;
    for(uint32_t i=0;i<d->queue_count;i++)if(d->queues[i].info.queueFamilyIndex==family&&d->queues[i].info.queueIndex==index){*queue=d->queues[i].client.queue;return;}
}

#define CREATE_WRAPPED(name,Type,field,Info) \
static VkResult VKAPI_CALL create_##field(VkDevice client,const Info *info,const VkAllocationCallbacks *allocator,Type *out) \
{struct vulkan_device *d=vulkan_device_from_handle(client);struct vulkan_##field *object=calloc(1,sizeof(*object));Type host;VkResult result; \
 if(!object)return VK_ERROR_OUT_OF_HOST_MEMORY;result=d->p_##name(d->host.device,info,NULL,&host);if(result){free(object);return result;} \
 vulkan_object_init(&object->obj,host);insert_object(d->physical_device->instance,&object->obj);*out=object->client.field;return VK_SUCCESS;}
#define DESTROY_WRAPPED(name,Type,field) \
static void VKAPI_CALL destroy_##field(VkDevice client,Type handle,const VkAllocationCallbacks *allocator) \
{if(!handle)return;struct vulkan_device *d=vulkan_device_from_handle(client);struct vulkan_##field *object=vulkan_##field##_from_handle(handle); \
 d->p_##name(d->host.device,object->host.field,NULL);remove_object(d->physical_device->instance,&object->obj);free(object);}
CREATE_WRAPPED(vkAllocateMemory,VkDeviceMemory,device_memory,VkMemoryAllocateInfo)
DESTROY_WRAPPED(vkFreeMemory,VkDeviceMemory,device_memory)
CREATE_WRAPPED(vkCreateFence,VkFence,fence,VkFenceCreateInfo)
DESTROY_WRAPPED(vkDestroyFence,VkFence,fence)
CREATE_WRAPPED(vkCreateSemaphore,VkSemaphore,semaphore,VkSemaphoreCreateInfo)
DESTROY_WRAPPED(vkDestroySemaphore,VkSemaphore,semaphore)
DESTROY_WRAPPED(vkDestroySwapchainKHR,VkSwapchainKHR,swapchain)
static VkResult VKAPI_CALL create_buffer(VkDevice client,const VkBufferCreateInfo *info,const VkAllocationCallbacks *allocator,VkBuffer *out)
{struct vulkan_device *d=vulkan_device_from_handle(client);return d->p_vkCreateBuffer(d->host.device,info,NULL,out);}
static VkResult VKAPI_CALL create_image(VkDevice client,const VkImageCreateInfo *info,const VkAllocationCallbacks *allocator,VkImage *out)
{struct vulkan_device *d=vulkan_device_from_handle(client);return d->p_vkCreateImage(d->host.device,info,NULL,out);}
static VkResult VKAPI_CALL map_memory(VkDevice client,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize size,VkMemoryMapFlags flags,void **out)
{struct vulkan_device *d=vulkan_device_from_handle(client);return d->p_vkMapMemory(d->host.device,vulkan_device_memory_from_handle(memory)->host.device_memory,offset,size,flags,out);}
static void VKAPI_CALL unmap_memory(VkDevice client,VkDeviceMemory memory)
{struct vulkan_device *d=vulkan_device_from_handle(client);d->p_vkUnmapMemory(d->host.device,vulkan_device_memory_from_handle(memory)->host.device_memory);}
static VkResult VKAPI_CALL queue_submit(VkQueue client,uint32_t count,const VkSubmitInfo *infos,VkFence fence)
{
    struct vulkan_queue *q=vulkan_queue_from_handle(client);
    for(uint32_t i=0;i<count;i++){
        VkSubmitInfo *info=(void *)(infos+i);
        for(uint32_t j=0;j<info->commandBufferCount;j++)((VkCommandBuffer*)info->pCommandBuffers)[j]=vulkan_command_buffer_from_handle(info->pCommandBuffers[j])->host.command_buffer;
        for(uint32_t j=0;j<info->waitSemaphoreCount;j++)((VkSemaphore*)info->pWaitSemaphores)[j]=vulkan_semaphore_from_handle(info->pWaitSemaphores[j])->host.semaphore;
        for(uint32_t j=0;j<info->signalSemaphoreCount;j++)((VkSemaphore*)info->pSignalSemaphores)[j]=vulkan_semaphore_from_handle(info->pSignalSemaphores[j])->host.semaphore;
    }
    return q->device->p_vkQueueSubmit(q->host.queue,count,infos,fence?vulkan_fence_from_handle(fence)->host.fence:0);
}
static VkResult VKAPI_CALL create_surface(VkInstance client,const VkWin32SurfaceCreateInfoKHR *info,const VkAllocationCallbacks *allocator,VkSurfaceKHR *out)
{
    struct vulkan_instance *i=vulkan_instance_from_handle(client);struct vulkan_surface *surface=calloc(1,sizeof(*surface));VkSurfaceKHR host;VkResult result;
    if(!surface)return VK_ERROR_OUT_OF_HOST_MEMORY;result=i->p_vkCreateWin32SurfaceKHR(i->host.instance,info,NULL,&host);if(result){free(surface);return result;}
    vulkan_object_init(&surface->obj,host);surface->instance=i;insert_object(i,&surface->obj);*out=surface->client.surface;return VK_SUCCESS;
}
static void VKAPI_CALL destroy_surface(VkInstance client,VkSurfaceKHR handle,const VkAllocationCallbacks *allocator)
{if(!handle)return;struct vulkan_instance *i=vulkan_instance_from_handle(client);struct vulkan_surface *s=vulkan_surface_from_handle(handle);i->p_vkDestroySurfaceKHR(i->host.instance,s->host.surface,NULL);remove_object(i,&s->obj);free(s);}
static VkBool32 VKAPI_CALL presentation_support(VkPhysicalDevice client,uint32_t family)
{struct vulkan_physical_device *p=vulkan_physical_device_from_handle(client);return p->instance->p_vkGetPhysicalDeviceWin32PresentationSupportKHR(p->host.physical_device,family);}
static VkResult VKAPI_CALL surface_capabilities(VkPhysicalDevice client,VkSurfaceKHR surface,VkSurfaceCapabilitiesKHR *out)
{struct vulkan_physical_device *p=vulkan_physical_device_from_handle(client);return p->instance->p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p->host.physical_device,vulkan_surface_from_handle(surface)->host.surface,out);}
static VkResult VKAPI_CALL surface_formats(VkPhysicalDevice client,VkSurfaceKHR surface,uint32_t *count,VkSurfaceFormatKHR *out)
{struct vulkan_physical_device *p=vulkan_physical_device_from_handle(client);return p->instance->p_vkGetPhysicalDeviceSurfaceFormatsKHR(p->host.physical_device,vulkan_surface_from_handle(surface)->host.surface,count,out);}
static void VKAPI_CALL physical_properties2(VkPhysicalDevice client,VkPhysicalDeviceProperties2 *out)
{
    struct vulkan_physical_device *p=vulkan_physical_device_from_handle(client);
    if(p->instance->p_vkGetPhysicalDeviceProperties2)p->instance->p_vkGetPhysicalDeviceProperties2(p->host.physical_device,out);
    else p->instance->p_vkGetPhysicalDeviceProperties2KHR(p->host.physical_device,out);
    out->properties.apiVersion=min(out->properties.apiVersion,VK_API_VERSION_1_0);
}
static VkResult VKAPI_CALL create_swapchain(VkDevice client,const VkSwapchainCreateInfoKHR *original,const VkAllocationCallbacks *allocator,VkSwapchainKHR *out)
{
    struct vulkan_device *d=vulkan_device_from_handle(client);struct vulkan_swapchain *s=calloc(1,sizeof(*s));VkSwapchainKHR host;VkSwapchainCreateInfoKHR info=*original;VkResult result;
    if(!s)return VK_ERROR_OUT_OF_HOST_MEMORY;info.surface=vulkan_surface_from_handle(info.surface)->host.surface;if(info.oldSwapchain)info.oldSwapchain=vulkan_swapchain_from_handle(info.oldSwapchain)->host.swapchain;
    result=d->p_vkCreateSwapchainKHR(d->host.device,&info,NULL,&host);if(result){fprintf(stderr,"native swapchain result=%d count=%u extent=%ux%u fmt=%u usage=%x mode=%u alpha=%x surface=%llx\n",result,info.minImageCount,info.imageExtent.width,info.imageExtent.height,info.imageFormat,info.imageUsage,info.presentMode,info.compositeAlpha,(unsigned long long)info.surface);free(s);return result;}vulkan_object_init(&s->obj,host);insert_object(d->physical_device->instance,&s->obj);*out=s->client.swapchain;return VK_SUCCESS;
}
static VkResult VKAPI_CALL acquire_next(VkDevice client,VkSwapchainKHR swapchain,uint64_t timeout,VkSemaphore semaphore,VkFence fence,uint32_t *index)
{struct vulkan_device *d=vulkan_device_from_handle(client);return d->p_vkAcquireNextImageKHR(d->host.device,vulkan_swapchain_from_handle(swapchain)->host.swapchain,timeout,semaphore?vulkan_semaphore_from_handle(semaphore)->host.semaphore:0,fence?vulkan_fence_from_handle(fence)->host.fence:0,index);}
static VkResult VKAPI_CALL queue_present(VkQueue client,const VkPresentInfoKHR *info)
{
    struct vulkan_queue *q=vulkan_queue_from_handle(client);
    for(uint32_t i=0;i<info->waitSemaphoreCount;i++)((VkSemaphore*)info->pWaitSemaphores)[i]=vulkan_semaphore_from_handle(info->pWaitSemaphores[i])->host.semaphore;
    for(uint32_t i=0;i<info->swapchainCount;i++)((VkSwapchainKHR*)info->pSwapchains)[i]=vulkan_swapchain_from_handle(info->pSwapchains[i])->host.swapchain;
    return q->device->p_vkQueuePresentKHR(q->host.queue,info);
}

NTSTATUS winedows_vulkan_call(unsigned int code,void *params)
{
    if(code>=unix_count)return STATUS_INVALID_PARAMETER;
    return __wine_unix_call_funcs[code](params);
}
const struct vulkan_funcs *winedows_vulkan_get_driver(UINT version)
{
    WCHAR path[512] = {0},actual[512] = {0};UINT len;DWORD actual_len;VkExtensionProperties *extensions;uint32_t count=0;PFN_vkEnumerateInstanceExtensionProperties enumerate;
    HMODULE shim;
    HMODULE (WINAPI *load_system_vulkan)(void);
    if(version!=WINE_VULKAN_DRIVER_VERSION)return NULL;
    if(system_loader)return &driver;
#ifdef _WIN64
    shim=GetModuleHandleW(L"winedows64.dll");
#else
    shim=GetModuleHandleW(L"winedows32.dll");
#endif
    if(shim)
    {
        /* The prefix runtime owns the authoritative host-file identity and
         * bypass scope. Never retry through the application's search path. */
        load_system_vulkan=(void *)GetProcAddress(shim,"WinedowsLoadSystemVulkan");
        if(!load_system_vulkan || !(system_loader=load_system_vulkan()))return NULL;
    }
    else
    {
#ifdef _WIN64
    len=GetSystemDirectoryW(path,ARRAY_SIZE(path)-1);
#else
    len=GetSystemWow64DirectoryW(path,ARRAY_SIZE(path)-1);if(!len)len=GetSystemDirectoryW(path,ARRAY_SIZE(path)-1);
#endif
    if(!len||len>=ARRAY_SIZE(path)-1||!path[0]||len+14>=ARRAY_SIZE(path))return NULL;lstrcatW(path,L"\\vulkan-1.dll");
    system_loader=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);if(!system_loader){fprintf(stderr,"native Vulkan load failed %lu path=%ls\n",GetLastError(),path);return NULL;}
    actual_len=GetModuleFileNameW(system_loader,actual,ARRAY_SIZE(actual)-1);
    if(!actual_len||actual_len>=ARRAY_SIZE(actual)-1||!same_file(path,actual)){fprintf(stderr,"native Vulkan identity mismatch expected=%ls actual=%ls\n",path,actual);goto failed;}
    }
    host_gipa=(void *)GetProcAddress(system_loader,"vkGetInstanceProcAddr");host_gdpa=(void *)GetProcAddress(system_loader,"vkGetDeviceProcAddr");host_properties=(void *)GetProcAddress(system_loader,"vkGetPhysicalDeviceProperties");
    if(!host_gipa||!host_gdpa||!host_properties){fprintf(stderr,"native Vulkan exports missing\n");goto failed;}
    enumerate=(void *)host_gipa(NULL,"vkEnumerateInstanceExtensionProperties");if(!enumerate||enumerate(NULL,&count,NULL)){fprintf(stderr,"native Vulkan extension query failed\n");goto failed;}
    if(!(extensions=calloc(count?count:1,sizeof(*extensions))))goto failed;
    if(enumerate(NULL,&count,extensions)){free(extensions);goto failed;}
    for(uint32_t i=0;i<count;i++){
        if(!strcmp(extensions[i].extensionName,VK_KHR_SURFACE_EXTENSION_NAME))driver.client_extensions.has_VK_KHR_surface=1;
        if(!strcmp(extensions[i].extensionName,VK_KHR_WIN32_SURFACE_EXTENSION_NAME))driver.client_extensions.has_VK_KHR_win32_surface=1;
        if(!strcmp(extensions[i].extensionName,VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))driver.client_extensions.has_VK_KHR_get_physical_device_properties2=1;
    }free(extensions);driver.host_extensions=driver.client_extensions;
    driver.p_vkGetInstanceProcAddr=get_instance_proc;driver.p_vkGetDeviceProcAddr=get_device_proc;driver.p_vkCreateInstance=create_instance;driver.p_vkDestroyInstance=destroy_instance;
    driver.p_vkCreateDevice=create_device;driver.p_vkDestroyDevice=destroy_device;driver.p_vkGetDeviceQueue=get_queue;
    driver.p_vkAllocateMemory=create_device_memory;driver.p_vkFreeMemory=destroy_device_memory;driver.p_vkMapMemory=map_memory;driver.p_vkUnmapMemory=unmap_memory;
    driver.p_vkCreateBuffer=create_buffer;driver.p_vkCreateImage=create_image;
    driver.p_vkCreateFence=create_fence;driver.p_vkDestroyFence=destroy_fence;driver.p_vkCreateSemaphore=create_semaphore;driver.p_vkDestroySemaphore=destroy_semaphore;
    driver.p_vkQueueSubmit=queue_submit;driver.p_vkCreateWin32SurfaceKHR=create_surface;driver.p_vkDestroySurfaceKHR=destroy_surface;
    driver.p_vkGetPhysicalDeviceWin32PresentationSupportKHR=presentation_support;driver.p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR=surface_capabilities;driver.p_vkGetPhysicalDeviceSurfaceFormatsKHR=surface_formats;
    driver.p_vkGetPhysicalDeviceProperties2=physical_properties2;driver.p_vkGetPhysicalDeviceProperties2KHR=physical_properties2;
    driver.p_vkCreateSwapchainKHR=create_swapchain;driver.p_vkDestroySwapchainKHR=destroy_swapchain;driver.p_vkAcquireNextImageKHR=acquire_next;driver.p_vkQueuePresentKHR=queue_present;
    return &driver;
failed:FreeLibrary(system_loader);system_loader=NULL;return NULL;
}
