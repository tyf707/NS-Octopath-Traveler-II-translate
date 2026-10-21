// Host-owned draw clears preserve masked channels, stencil bits, scissors and
// individual layers. Transfer clears cannot express those NVN semantics.
#include "clear_shaders.h"
VkPipelineLayout clear_layout{};
VkShaderModule clear_vertex{},clear_fragments[3]{};
template<size_t N> VkShaderModule clear_module(const uint32_t (&words)[N]) {
    return shared_shader_module(words,N);
}
void draw_clear(const tex_t& texture,const view_t* view,const NVNcopyRegion& region,const void* rgba,int color_mask,int color_type,const p_clear_ds* depth_clear) {
    auto& img=image_for(texture);auto range=subresources(img,view);range.levelCount=1;
    const uint32_t width=std::max(1U,texture.d.width>>range.baseMipLevel),height=std::max(1U,texture.d.height>>range.baseMipLevel);
    const int x=std::max(0,region.xoffset),y=std::max(0,region.yoffset);
    const int right=std::min(int(width),region.xoffset+region.width),bottom=std::min(int(height),region.yoffset+region.height);
    if(right<=x||bottom<=y||region.depth<=0)return;
    // 深度+stencil 图：游戏只清 depth 时，vkCmdClearDepthStencilImage 会连 stencil 一起清。
    // 用「LOAD/STORE 的 depth-stencil attachment + 只清 depth 的 vkCmdClearAttachments」保留 stencil。
    {
        const bool full=x==0&&y==0&&right==int(width)&&bottom==int(height);
        const bool single_layer=region.depth==1&&img.layers==1&&range.layerCount==1;
        if(full&&single_layer&&img.samples==VK_SAMPLE_COUNT_1_BIT&&depth_clear&&depth_clear->depth_mask!=0&&
           (img.aspects&VK_IMAGE_ASPECT_STENCIL_BIT)&&depth_clear->stencil_mask!=0xFF){
            auto cmd=begin_commands();
            barrier(cmd,img,img.layout,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            view_t layer_view=view?*view:view_t{};layer_view.set_levels=layer_view.set_layers=1;
            layer_view.base_level=range.baseMipLevel;layer_view.nlevels=1;
            layer_view.min_layer=(view&&view->set_layers?view->min_layer:0)+region.zoffset;layer_view.nlayers=1;
            VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            attachment.imageView=make_view(texture,&layer_view,true);attachment.imageLayout=img.layout;
            attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};
            render.renderArea={{0,0},{width,height}};render.layerCount=1;
            render.pDepthAttachment=&attachment;render.pStencilAttachment=&attachment;
            VkPipelineRenderingCreateInfo formats{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            formats.depthAttachmentFormat=img.format;formats.stencilAttachmentFormat=img.format;
            begin_rendering_compatible(cmd,render,formats,img.samples);
            VkClearAttachment clear{};clear.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;clear.clearValue.depthStencil={depth_clear->depth,0};
            VkClearRect rect{{{0,0},{width,height}},0,1};
            vkCmdClearAttachments(cmd,1,&clear,1,&rect);
            end_rendering_compatible(cmd);
            barrier(cmd,img,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
            finish_commands(cmd);
            return;
        }
    }
    // 快速路径：整图、单层、单采样、全通道的纯色/深度清除，用 GPU 原生 clear，
    // 比“全屏三角形 + LOAD/STORE”快很多（4096x4096 的清除实测占 7ms/帧）。
    // 只覆盖语义完全等价的简单情形；mask/scissor/多层/压缩格式仍走原画刷路径。
    {
        const bool full=x==0&&y==0&&right==int(width)&&bottom==int(height);
        const bool single_layer=region.depth==1&&img.layers==1&&range.layerCount==1;
        const bool simple_samples=img.samples==VK_SAMPLE_COUNT_1_BIT;
        const bool plain_color=!depth_clear&&rgba&&color_mask==15&&color_type==0&&!img.bc;
        const bool plain_depth=depth_clear&&depth_clear->depth_mask!=0&&((!(img.aspects&VK_IMAGE_ASPECT_STENCIL_BIT))||depth_clear->stencil_mask==0xFF);
        const bool clearable_format=img.format==VK_FORMAT_R8G8B8A8_UNORM||img.format==VK_FORMAT_R8G8B8A8_SRGB||img.format==VK_FORMAT_B8G8R8A8_UNORM||img.format==VK_FORMAT_B8G8R8A8_SRGB;
        if(full&&single_layer&&simple_samples&&(plain_depth||(plain_color&&clearable_format))){
            auto cmd=begin_commands();
            barrier(cmd,img,img.layout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageSubresourceRange fast_range{img.aspects,range.baseMipLevel,1,0,1};
            if(plain_depth){
                VkClearDepthStencilValue value{depth_clear->depth,uint32_t(depth_clear->stencil)};
                vkCmdClearDepthStencilImage(cmd,img.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&fast_range);
            }else{
                VkClearColorValue value{};memcpy(value.float32,rgba,16);
                vkCmdClearColorImage(cmd,img.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&fast_range);
            }
            barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
            finish_commands(cmd);
            return;
        }
    }
    if(!clear_layout){
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,20};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device,&layout,nullptr,&clear_layout),"clear layout");
        clear_vertex=clear_module(clear_vert);clear_fragments[0]=clear_module(clear_frag);clear_fragments[1]=clear_module(clear_uint_frag);clear_fragments[2]=clear_module(clear_int_frag);
    }
    const bool depth=depth_clear!=nullptr,stencil=(img.aspects&VK_IMAGE_ASPECT_STENCIL_BIT)!=0;
    VkPipelineShaderStageCreateInfo stages[2]{};for(auto& stage:stages){stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.pName="main";}
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=clear_vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=clear_fragments[color_type];
    const VkFormat attachment_format=view&&view->set_format?format(view->format):img.format;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    if(depth){rendering.depthAttachmentFormat=attachment_format;if(stencil)rendering.stencilAttachmentFormat=attachment_format;}
    else{rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&attachment_format;}
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vs.viewportCount=vs.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=img.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if(depth){ds.depthTestEnable=ds.depthWriteEnable=depth_clear->depth_mask!=0;ds.depthCompareOp=VK_COMPARE_OP_ALWAYS;
        ds.stencilTestEnable=stencil&&depth_clear->stencil_mask;ds.front={VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_REPLACE,VK_COMPARE_OP_ALWAYS,255,uint32_t(depth_clear->stencil_mask),uint32_t(depth_clear->stencil)};ds.back=ds.front;}
    VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=color_mask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};if(!depth){cb.attachmentCount=1;cb.pAttachments=&blend;}
    VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=dynamic_states;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.pNext=&rendering;ci.stageCount=depth?1:2;ci.pStages=stages;ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vs;ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pDepthStencilState=&ds;ci.pColorBlendState=&cb;ci.pDynamicState=&dynamic;ci.layout=clear_layout;
    auto pipeline=cached_pipeline(ci,{},{{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,20}});
    auto cmd=begin_commands();barrier(cmd,img,img.layout,depth?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    struct {uint32_t color[4];float depth;} push{};if(rgba)memcpy(push.color,rgba,16);push.depth=depth?depth_clear->depth:0;
    for(int layer=0;layer<region.depth;layer++){
        view_t layer_view=view?*view:view_t{};layer_view.set_levels=layer_view.set_layers=1;layer_view.base_level=range.baseMipLevel;layer_view.nlevels=1;layer_view.min_layer=(view&&view->set_layers?view->min_layer:0)+region.zoffset+layer;layer_view.nlayers=1;
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};attachment.imageView=make_view(texture,&layer_view,true);attachment.imageLayout=img.layout;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};render.renderArea={{0,0},{width,height}};render.layerCount=1;
        if(depth){render.pDepthAttachment=&attachment;if(stencil)render.pStencilAttachment=&attachment;}else{render.colorAttachmentCount=1;render.pColorAttachments=&attachment;}
        if(gpu_profile_enabled)gpu_profile_region_begin(cmd,"clear-draw:"+std::to_string(width)+"x"+std::to_string(height));
        begin_rendering_compatible(cmd,render,rendering,img.samples);VkViewport viewport{0,0,float(width),float(height),0,1};VkRect2D scissor{{x,y},{uint32_t(right-x),uint32_t(bottom-y)}};
        bind_viewport(cmd,viewport);bind_scissor(cmd,scissor);bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        vkCmdPushConstants(cmd,clear_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);vkCmdDraw(cmd,3,1,0,0);end_rendering_compatible(cmd);gpu_profile_region_end(cmd);
    }
    finish_commands(cmd);
}
