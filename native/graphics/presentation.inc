#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <atomic>
#include <cstring>
// 显示填充模式：fit=保持 16:9（左右黑边，默认改前行为）、crop=填满裁切、stretch=拉伸填满。
// debug.ot2.aspect=fit|crop|stretch 选择；首次显示时读取一次，改属性后重启生效。
enum DisplayAspect : int { display_fit = 0, display_crop = 1, display_stretch = 2 };
static std::atomic<int> display_aspect_override{-1}; // -1=未覆盖；0=fit 1=crop 2=stretch
extern "C" void vkhost_set_aspect_mode(int mode) {display_aspect_override.store(mode,std::memory_order_relaxed);}
static int display_aspect_mode() {
#ifdef __ANDROID__
    const int overridden = display_aspect_override.load(std::memory_order_relaxed);
    if (overridden >= 0) return overridden;
    static std::atomic<int> cached{-1};
    int value = cached.load(std::memory_order_relaxed);
    if (value < 0) {
        char property[PROP_VALUE_MAX]{};
        value = display_stretch;
        if (__system_property_get("debug.ot2.aspect", property) > 0) {
            if (!strcmp(property, "fit")) value = display_fit;
            else if (!strcmp(property, "stretch")) value = display_stretch;
            else if (!strcmp(property, "crop")) value = display_crop;
        }
        cached.store(value, std::memory_order_relaxed);
    }
    return value;
#else
    return display_fit;
#endif
}
// Display scan-out consumes encoded bytes, including sRGB guest swapchains.
// A Vulkan sRGB -> UNORM blit decodes them and darkens the displayed image.
// Sample a compatible UNORM view instead, as D2R's present_src does. This is
// one fullscreen draw, with no intermediate image or per-frame allocation.
// Presentation is serialized and finish_commands waits for its actual fence;
// the single descriptor set is therefore retired before its next update.
VkDescriptorSetLayout present_set_layout{};
VkPipelineLayout present_layout{};
VkDescriptorPool present_pool{};
VkDescriptorSet present_set{};
VkSampler present_sampler{};
std::map<VkFormat,VkPipeline> present_pipelines;
std::vector<VkImageView> swapchain_views;
VkFormat swapchain_format{};
void destroy_swapchain(){
    for(auto view:swapchain_views){legacy_forget_view(view);vkDestroyImageView(device,view,nullptr);}
    swapchain_views.clear();swapchain_images.clear();
    if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);
    swapchain=VK_NULL_HANDLE;
}
VkPipeline present_pipeline(VkFormat format){
    if(format!=VK_FORMAT_R8G8B8A8_UNORM&&format!=VK_FORMAT_B8G8R8A8_UNORM)ot2_host_die("Unsupported display format %u",format);
    if(auto found=present_pipelines.find(format);found!=present_pipelines.end())return found->second;
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,32};
    if(!present_layout){
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=1;li.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device,&li,nullptr,&present_set_layout),"present descriptor layout");
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=1;pi.pSetLayouts=&present_set_layout;pi.pushConstantRangeCount=1;pi.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device,&pi,nullptr,&present_layout),"present layout");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        check(vkCreateDescriptorPool(device,&pool,nullptr,&present_pool),"present descriptor pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=present_pool;ai.descriptorSetCount=1;ai.pSetLayouts=&present_set_layout;
        check(vkAllocateDescriptorSets(device,&ai,&present_set),"present descriptor set");
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(device,&si,nullptr,&present_sampler),"present sampler");
    }
    VkPipelineShaderStageCreateInfo stages[2]{};for(auto& stage:stages){stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.pName="main";}
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=clear_module(present_vert);stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=clear_module(present_frag);
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&format;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vs.viewportCount=vs.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&blend;
    VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=states;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.pNext=&rendering;ci.stageCount=2;ci.pStages=stages;ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vs;ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pDepthStencilState=&ds;ci.pColorBlendState=&cb;ci.pDynamicState=&dynamic;ci.layout=present_layout;
    auto pipeline=cached_pipeline(ci,{binding},{push});present_pipelines.emplace(format,pipeline);return pipeline;
}
void encode_present_pixels(VkCommandBuffer cmd,const tex_t& texture,Image& target,VkImageView target_view,VkExtent2D extent,const int* crop){
    if(batching)ot2_host_die("Presentation requires a retired synchronous descriptor set");
    auto& source=image_for(texture);
    if(source.is_3d||source.samples!=VK_SAMPLE_COUNT_1_BIT||source.aspects!=VK_IMAGE_ASPECT_COLOR_BIT)ot2_host_die("Invalid display source");
    int sx=0,sy=0,sw=texture.d.width,sh=texture.d.height;
    if(crop&&(crop[2]||crop[3])){sx=crop[0];sy=crop[1];sw=crop[2];sh=crop[3];}
    if(sx<0||sy<0||sw<=0||sh<=0||uint64_t(sx)+sw>texture.d.width||uint64_t(sy)+sh>texture.d.height)ot2_host_die("Invalid display crop");
    const int aspect=display_aspect_mode();
    const double scale=aspect==display_crop?std::max(double(extent.width)/sw,double(extent.height)/sh):std::min(double(extent.width)/sw,double(extent.height)/sh);
    int dw=std::max(1,int(sw*scale)),dh=std::max(1,int(sh*scale)),dx=(extent.width-dw)/2,dy=(extent.height-dh)/2;
    int rsx=sx,rsy=sy,rsw=sw,rsh=sh;
    if(aspect==display_stretch){dw=extent.width;dh=extent.height;dx=dy=0;}
    else if(aspect==display_crop){
        const double target=double(extent.width)/extent.height,source=double(sw)/sh;
        if(source>target){const int nw=std::max(1,int(sh*target));rsx=sx+(sw-nw)/2;rsw=nw;}
        else{const int nh=std::max(1,int(sw/target));rsy=sy+(sh-nh)/2;rsh=nh;}
        dw=extent.width;dh=extent.height;dx=dy=0;
    }
    ot2::touch_display_size(sw,sh);
    view_t raw{};raw.set_format=raw.set_levels=raw.set_layers=raw.set_target=raw.set_swizzle=1;raw.format=texture.d.format;raw.nlevels=raw.nlayers=1;raw.target=NVN_TEXTURE_TARGET_2D;raw.swizzle=2|(3<<3)|(4<<6)|(5<<9);
    switch(source.format){case VK_FORMAT_R8G8B8A8_SRGB:raw.format=NVN_FORMAT_RGBA8;break;case VK_FORMAT_B8G8R8A8_SRGB:raw.format=NVN_FORMAT_BGRA8;break;default:break;}
    const auto pipeline=present_pipeline(target.format);
    VkDescriptorImageInfo image_info{present_sampler,make_view(texture,&raw),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=present_set;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image_info;
    vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    barrier(cmd,source,source.layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);barrier(cmd,target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkPipelineRenderingCreateInfo formats{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};formats.colorAttachmentCount=1;formats.pColorAttachmentFormats=&target.format;
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};attachment.imageView=target_view;attachment.imageLayout=target.layout;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};render.renderArea={{0,0},extent};render.layerCount=1;render.colorAttachmentCount=1;render.pColorAttachments=&attachment;
    begin_rendering_compatible(cmd,render,formats,VK_SAMPLE_COUNT_1_BIT);
    bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,present_layout,present_set);
    VkViewport viewport{float(dx),float(dy),float(dw),float(dh),0,1};bind_viewport(cmd,viewport);bind_scissor(cmd,{{dx,dy},{uint32_t(dw),uint32_t(dh)}});
    const float w=texture.d.width,h=texture.d.height;
    const float constants[]{rsx/w,rsy/h,rsw/w,rsh/h,(rsx+.5f)/w,(rsy+.5f)/h,(rsx+rsw-.5f)/w,(rsy+rsh-.5f)/h};
    vkCmdPushConstants(cmd,present_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(constants),constants);vkCmdDraw(cmd,3,1,0,0);end_rendering_compatible(cmd);
}
