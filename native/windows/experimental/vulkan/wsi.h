/* Native Win32 WSI lifecycle. Empty capabilities are observed, never repaired
 * into an extent which the native surface does not support. */
static void wsi_lifecycle(PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
        VkInstance instance, VkPhysicalDevice gpu, VkDevice device, VkQueue queue,
        uint32_t family, VkCommandPool pool, VkCommandBuffer cb, VkFence fence)
{
    INSTANCE(vkCreateWin32SurfaceKHR); INSTANCE(vkDestroySurfaceKHR);
    INSTANCE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); INSTANCE(vkGetPhysicalDeviceSurfaceFormatsKHR);
    INSTANCE(vkGetPhysicalDeviceSurfaceSupportKHR);
    DEVICE(vkCreateSwapchainKHR); DEVICE(vkDestroySwapchainKHR); DEVICE(vkGetSwapchainImagesKHR);
    DEVICE(vkAcquireNextImageKHR); DEVICE(vkQueuePresentKHR); DEVICE(vkDeviceWaitIdle);
    DEVICE(vkResetFences); DEVICE(vkWaitForFences); DEVICE(vkResetCommandPool);
    DEVICE(vkBeginCommandBuffer); DEVICE(vkEndCommandBuffer); DEVICE(vkCmdPipelineBarrier);
    DEVICE(vkCmdClearColorImage); DEVICE(vkQueueSubmit);
    HWND window=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Winedows Vulkan WSI",
            WS_POPUP|WS_SYSMENU|WS_MINIMIZEBOX,10,10,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window)throw std::runtime_error("WSI window");
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance=GetModuleHandleW(nullptr); sci.hwnd=window;
    VkSurfaceKHR surface; VK(vkCreateWin32SurfaceKHR(instance,&sci,nullptr,&surface));
    VkBool32 supported; VK(vkGetPhysicalDeviceSurfaceSupportKHR(gpu,family,surface,&supported));
    if(!supported)throw std::runtime_error("WSI queue support");
    uint32_t count=0; VK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu,surface,&count,nullptr));
    std::vector<VkSurfaceFormatKHR> formats(count);
    VK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu,surface,&count,formats.data()));
    if(!count)throw std::runtime_error("WSI formats");
    VkSwapchainKHR swapchain=0;
    unsigned presented=0,deferred=0;
    auto present=[&](const char *phase)
    {
        VK(vkResetFences(device,1,&fence));
        uint32_t index=0;
        VkResult acquired=vkAcquireNextImageKHR(device,swapchain,5000000000ull,0,fence,&index);
        std::printf("WSI %s acquire=%d\n",phase,acquired);
        if(acquired==VK_ERROR_OUT_OF_DATE_KHR)return;
        if(acquired!=VK_SUCCESS && acquired!=VK_SUBOPTIMAL_KHR)check(acquired,"WSI acquire");
        VK(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull));
        VK(vkGetSwapchainImagesKHR(device,swapchain,&count,nullptr));
        std::vector<VkImage> images(count);
        VK(vkGetSwapchainImagesKHR(device,swapchain,&count,images.data()));
        if(index>=count)throw std::runtime_error("WSI image index");
        VK(vkResetFences(device,1,&fence)); VK(vkResetCommandPool(device,pool,0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK(vkBeginCommandBuffer(cb,&begin));
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image=images[index]; barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,0,nullptr,0,nullptr,1,&barrier);
        VkClearColorValue color{{0.25f,0.5f,0.75f,1.f}};
        vkCmdClearColorImage(cb,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
        barrier.oldLayout=barrier.newLayout; barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=0;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0,0,nullptr,0,nullptr,1,&barrier);
        VK(vkEndCommandBuffer(cb));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cb;
        VK(vkQueueSubmit(queue,1,&submit,fence));
        VK(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull));
        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; info.swapchainCount=1;
        info.pSwapchains=&swapchain; info.pImageIndices=&index;
        VkResult result=vkQueuePresentKHR(queue,&info);
        std::printf("WSI %s present=%d\n",phase,result);
        if(result!=VK_SUCCESS && result!=VK_SUBOPTIMAL_KHR && result!=VK_ERROR_OUT_OF_DATE_KHR)
            check(result,"WSI present");
        VK(vkDeviceWaitIdle(device));
        if(result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR)++presented;
    };
    auto resize=[&](const char *phase,int width,int height,bool minimized=false)
    {
        if(!SetWindowPos(window,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE))
            throw std::runtime_error("WSI resize");
        if(minimized)ShowWindow(window,SW_SHOWMINNOACTIVE);
        MSG message; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE))DispatchMessageW(&message);
        RECT before,after; GetClientRect(window,&before);
        VkSurfaceCapabilitiesKHR caps{};
        VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu,surface,&caps));
        GetClientRect(window,&after);
        if(!EqualRect(&before,&after))throw std::runtime_error("WSI query changed application window");
        std::printf("WSI %s client=%ldx%ld current=%ux%u min=%ux%u max=%ux%u iconic=%d\n",phase,
                before.right,before.bottom,caps.currentExtent.width,caps.currentExtent.height,
                caps.minImageExtent.width,caps.minImageExtent.height,caps.maxImageExtent.width,
                caps.maxImageExtent.height,IsIconic(window)); std::fflush(stdout);
        if(!caps.currentExtent.width || !caps.currentExtent.height)
        {
            ++deferred; std::puts("PASS empty surface defers swapchain creation"); return;
        }
        if(minimized)return; /* A driver may retain the restored extent. */
        if(swapchain)present("old extent after resize");
        VkSwapchainCreateInfoKHR sw{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sw.surface=surface; sw.minImageCount=caps.minImageCount;
        sw.imageFormat=formats[0].format; sw.imageColorSpace=formats[0].colorSpace;
        sw.imageExtent=caps.currentExtent; sw.imageArrayLayers=1;
        sw.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT; sw.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
        sw.preTransform=caps.currentTransform; sw.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sw.presentMode=VK_PRESENT_MODE_FIFO_KHR; sw.clipped=VK_TRUE; sw.oldSwapchain=swapchain;
        VkSwapchainKHR next; VK(vkCreateSwapchainKHR(device,&sw,nullptr,&next));
        if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);
        swapchain=next; present(phase);
    };
    resize("hidden internal-sized",1,1);
    resize("empty height",120,0);
    resize("empty both",0,0);
    resize("restore from empty",96,80);
    resize("resize",128,72);
    resize("minimize",128,72,true);
    ShowWindow(window,SW_RESTORE); ShowWindow(window,SW_HIDE);
    resize("restore from minimized",80,96);
    if(presented<4 || deferred<2)throw std::runtime_error("WSI lifecycle coverage");
    VK(vkDeviceWaitIdle(device));
    vkDestroySwapchainKHR(device,swapchain,nullptr); vkDestroySurfaceKHR(instance,surface,nullptr);
    DestroyWindow(window);
    std::printf("PASS WSI lifecycle presentations=%u empty-deferred=%u\n",presented,deferred);
}
