#include <windows.h>
extern "C" {
#include "wine/vulkan.h"
}
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <stdexcept>
#define GLOBAL(F) auto F=(PFN_##F)gipa(nullptr,#F)
#define INSTANCE(F) auto F=(PFN_##F)gipa(instance,#F);if(!F)throw std::runtime_error("missing " #F)
#define DEVICE(F) auto F=(PFN_##F)gdpa(device,#F);if(!F)throw std::runtime_error("missing " #F)
static void check(VkResult r,const char*where){if(r!=VK_SUCCESS){std::fprintf(stderr,"%s VkResult=%d\n",where,r);throw std::runtime_error(where);}}
#define VK(call) check((call),#call)
static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties &p,uint32_t bits,VkMemoryPropertyFlags flags){for(uint32_t i=0;i<p.memoryTypeCount;i++)if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags)return i;throw std::runtime_error("memory type");}
#include "draw.h"
#include "wsi.h"
static int run_probe(int argc,wchar_t**argv){try{
 if(argc<2)return 2;bool wine=false;uint32_t vendor=0;
 for(int arg=2;arg<argc;arg++){if(!wcscmp(argv[arg],L"--wine"))wine=true;else if(!wcscmp(argv[arg],L"--vendor")&&arg+1<argc)vendor=wcstoul(argv[++arg],nullptr,0);else throw std::runtime_error("unknown argument");}
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 HMODULE module;
#ifdef WINEDOWS_VULKAN_STATIC_PROBE
 auto gipa=vkGetInstanceProcAddr;
 if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,(const WCHAR *)gipa,&module))throw std::runtime_error("static Vulkan module");
#else
 module=wcschr(argv[1],L'\\')||wcschr(argv[1],L'/')?LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32):LoadLibraryW(argv[1]);
 if(!module){std::fprintf(stderr,"load error=%lu\n",GetLastError());return 3;}
 auto gipa=(PFN_vkGetInstanceProcAddr)GetProcAddress(module,"vkGetInstanceProcAddr");if(!gipa)throw std::runtime_error("GIPA");
#endif
 GLOBAL(vkCreateInstance);GLOBAL(vkEnumerateInstanceExtensionProperties);
 uint32_t extension_count=0;VK(vkEnumerateInstanceExtensionProperties(nullptr,&extension_count,nullptr));std::printf("extensions=%u\n",extension_count);std::fflush(stdout);
 if(extension_count<2)throw std::runtime_error("missing surface extensions");std::vector<VkExtensionProperties>enumerated(extension_count);uint32_t full_count=extension_count;VK(vkEnumerateInstanceExtensionProperties(nullptr,&full_count,enumerated.data()));for(uint32_t i=0;i<full_count;i++)for(uint32_t j=0;j<i;j++)if(!std::strcmp(enumerated[i].extensionName,enumerated[j].extensionName))throw std::runtime_error("duplicate extension");
 struct Bounded {VkExtensionProperties one;uint64_t canary;} bounded{};bounded.canary=0x13579bdf2468ace0ull;uint32_t capacity=1;if(vkEnumerateInstanceExtensionProperties(nullptr,&capacity,&bounded.one)!=VK_INCOMPLETE||capacity!=1||bounded.canary!=0x13579bdf2468ace0ull)throw std::runtime_error("extension enumeration bounds");
 const char*extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Winedows real Vulkan probe";app.apiVersion=VK_API_VERSION_1_0;
 VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};create.pApplicationInfo=&app;create.enabledExtensionCount=2;create.ppEnabledExtensionNames=extensions;VkInstance instance;
 const char*missing_extension="VK_WINEDOWS_missing_fixture";VkInstanceCreateInfo rejected=create;rejected.enabledExtensionCount=1;rejected.ppEnabledExtensionNames=&missing_extension;VkInstance invalid=nullptr;if(vkCreateInstance(&rejected,nullptr,&invalid)!=VK_ERROR_EXTENSION_NOT_PRESENT)throw std::runtime_error("unknown instance extension accepted");
 if(wine){app.apiVersion=VK_API_VERSION_1_1;if(vkCreateInstance(&create,nullptr,&invalid)!=VK_ERROR_INCOMPATIBLE_DRIVER)throw std::runtime_error("unimplemented API version accepted");app.apiVersion=VK_API_VERSION_1_0;}
 VK(vkCreateInstance(&create,nullptr,&instance));std::printf("instance=%p first-word=%llx\n",(void*)instance,*(unsigned long long*)instance);std::fflush(stdout);
 if(wine&&*(uint64_t*)instance!=0x01cdc0de)throw std::runtime_error("Wine object identity missing");if(!wine&&*(uint64_t*)instance==0x01cdc0de)throw std::runtime_error("native provider selected Wine objects");if(gipa(instance,"vkWinedowsMissingFixture"))throw std::runtime_error("unknown function exposed");if(wine&&gipa(instance,"vkGetMemoryWin32HandleKHR"))throw std::runtime_error("unimplemented backend function exposed");
 INSTANCE(vkEnumeratePhysicalDevices);INSTANCE(vkGetPhysicalDeviceProperties);INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);INSTANCE(vkGetPhysicalDeviceMemoryProperties);INSTANCE(vkCreateDevice);INSTANCE(vkGetDeviceProcAddr);INSTANCE(vkDestroyInstance);auto gdpa=vkGetDeviceProcAddr;
 INSTANCE(vkCreateWin32SurfaceKHR);INSTANCE(vkDestroySurfaceKHR);INSTANCE(vkGetPhysicalDeviceSurfaceSupportKHR);INSTANCE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);INSTANCE(vkGetPhysicalDeviceSurfaceFormatsKHR);
 uint32_t count=0;VK(vkEnumeratePhysicalDevices(instance,&count,nullptr));if(!count)throw std::runtime_error("no GPU");std::vector<VkPhysicalDevice> physical(count);VK(vkEnumeratePhysicalDevices(instance,&count,physical.data()));auto gpu=physical[0];
 if(vendor){gpu=nullptr;for(auto candidate:physical){VkPhysicalDeviceProperties candidate_properties{};vkGetPhysicalDeviceProperties(candidate,&candidate_properties);if(candidate_properties.vendorID==vendor){gpu=candidate;break;}}if(!gpu)throw std::runtime_error("requested GPU vendor absent");}
 VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(gpu,&props);std::printf("GPU=%s vendor=%04x device=%04x API=%x\n",props.deviceName,props.vendorID,props.deviceID,props.apiVersion);std::fflush(stdout);
 VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(gpu,&memory);
 vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,families.data());uint32_t family=0;while(family<count&&!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))family++;if(family==count)throw std::runtime_error("no graphics queue");
 float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
 const char*device_extensions[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;di.enabledExtensionCount=1;di.ppEnabledExtensionNames=device_extensions;VkDevice device;VK(vkCreateDevice(gpu,&di,nullptr,&device));
 DEVICE(vkGetDeviceQueue);DEVICE(vkCreateImage);DEVICE(vkGetImageMemoryRequirements);DEVICE(vkAllocateMemory);DEVICE(vkBindImageMemory);DEVICE(vkCreateBuffer);DEVICE(vkGetBufferMemoryRequirements);DEVICE(vkBindBufferMemory);DEVICE(vkCreateCommandPool);DEVICE(vkAllocateCommandBuffers);DEVICE(vkBeginCommandBuffer);DEVICE(vkEndCommandBuffer);DEVICE(vkCmdPipelineBarrier);DEVICE(vkCmdClearColorImage);DEVICE(vkCmdCopyImageToBuffer);DEVICE(vkCreateFence);DEVICE(vkResetFences);DEVICE(vkWaitForFences);DEVICE(vkQueueSubmit);DEVICE(vkMapMemory);DEVICE(vkUnmapMemory);DEVICE(vkDestroyCommandPool);DEVICE(vkDestroyFence);DEVICE(vkDestroyBuffer);DEVICE(vkDestroyImage);DEVICE(vkFreeMemory);DEVICE(vkDestroyDevice);DEVICE(vkDeviceWaitIdle);DEVICE(vkCreateSwapchainKHR);DEVICE(vkGetSwapchainImagesKHR);DEVICE(vkAcquireNextImageKHR);DEVICE(vkQueuePresentKHR);DEVICE(vkDestroySwapchainKHR);DEVICE(vkResetCommandPool);
 VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_R8G8B8A8_UNORM;ii.extent={8,8,1};ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;ii.sharingMode=VK_SHARING_MODE_EXCLUSIVE;VkImage img;VK(vkCreateImage(device,&ii,nullptr,&img));
 VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,img,&req);VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;alloc.memoryTypeIndex=memory_type(memory,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VkDeviceMemory image_mem;VK(vkAllocateMemory(device,&alloc,nullptr,&image_mem));VK(vkBindImageMemory(device,img,image_mem,0));
 VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=8*8*4;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;VkBuffer buf;VK(vkCreateBuffer(device,&bi,nullptr,&buf));vkGetBufferMemoryRequirements(device,buf,&req);alloc.allocationSize=req.size;alloc.memoryTypeIndex=memory_type(memory,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);VkDeviceMemory buf_mem;VK(vkAllocateMemory(device,&alloc,nullptr,&buf_mem));VK(vkBindBufferMemory(device,buf,buf_mem,0));
 VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=family;VkCommandPool pool;VK(vkCreateCommandPool(device,&pci,nullptr,&pool));VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;VkCommandBuffer cb;VK(vkAllocateCommandBuffers(device,&cai,&cb));
 VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK(vkBeginCommandBuffer(cb,&begin));VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=img;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
 VkClearColorValue color{{0.25f,0.5f,0.75f,1.0f}};vkCmdClearColorImage(cb,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);barrier.oldLayout=barrier.newLayout;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
 VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={8,8,1};vkCmdCopyImageToBuffer(cb,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buf,1,&copy);VK(vkEndCommandBuffer(cb));
 VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence;VK(vkCreateFence(device,&fi,nullptr,&fence));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cb;VK(vkQueueSubmit(queue,1,&submit,fence));VK(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull));
 void*mapped;VK(vkMapMemory(device,buf_mem,0,VK_WHOLE_SIZE,0,&mapped));const int expected[]={64,128,191,255};for(int pixel=0;pixel<64;pixel++)for(int channel=0;channel<4;channel++)if(std::abs(int(((unsigned char*)mapped)[pixel*4+channel])-expected[channel])>1)throw std::runtime_error("GPU pixel mismatch");vkUnmapMemory(device,buf_mem);std::puts("PASS 64 GPU image pixels through command/memory/fence dispatch");std::fflush(stdout);
 draw_and_read(gdpa,device,queue,memory,img,buf,buf_mem,pool,cb,fence);
 wsi_lifecycle(gipa,gdpa,instance,gpu,device,queue,family,pool,cb,fence);
 vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,pool,nullptr);vkDestroyBuffer(device,buf,nullptr);vkFreeMemory(device,buf_mem,nullptr);vkDestroyImage(device,img,nullptr);vkFreeMemory(device,image_mem,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);FreeLibrary(module);return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
int wmain(int argc,wchar_t**argv){for(int cycle=0;cycle<3;cycle++){std::printf("Lifecycle cycle=%d\n",cycle);int result=run_probe(argc,argv);if(result)return result;}std::puts("PASS all3 complete GPU lifecycle cycles");return 0;}
