/*
// Copyright (c) 2016 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "drmbuffer.h"

#include <drm_fourcc.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <hwcdefs.h>
#include <nativebufferhandler.h>

#include "hwctrace.h"

namespace hwcomposer {

DrmBuffer::~DrmBuffer() {
  ReleaseFrameBuffer();

  if (buffer_handler_ && handle_) {
    buffer_handler_->ReleaseBuffer(handle_);
    buffer_handler_->DestroyHandle(handle_);
  }
}

void DrmBuffer::Initialize(const HwcBuffer& bo) {
  width_ = bo.width;
  height_ = bo.height;
  for (uint32_t i = 0; i < 4; i++) {
    pitches_[i] = bo.pitches[i];
    offsets_[i] = bo.offsets[i];
    gem_handles_[i] = bo.gem_handles[i];
  }

  format_ = bo.format;
  prime_fd_ = bo.prime_fd;
  usage_ = bo.usage;
  if (usage_ & hwcomposer::kLayerCursor) {
    // We support DRM_FORMAT_ARGB8888 for cursor.
    frame_buffer_format_ = DRM_FORMAT_ARGB8888;
  } else {
    frame_buffer_format_ = format_;
  }

  switch (format_) {
    case DRM_FORMAT_YVU420:
    case DRM_FORMAT_UYVY:
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_YUV420:
      is_yuv_ = true;
      break;
    default:
      is_yuv_ = false;
  }
}

void DrmBuffer::InitializeFromNativeHandle(
    HWCNativeHandle handle, NativeBufferHandler* buffer_handler) {
  struct HwcBuffer bo;
  buffer_handler->CopyHandle(handle, &handle_);
  if (!buffer_handler->ImportBuffer(handle_, &bo)) {
    ETRACE("Failed to Import buffer.");
    return;
  }

  buffer_handler_ = buffer_handler;
  Initialize(bo);
}

struct vk_import DrmBuffer::ImportImage(VkInstance inst, VkPhysicalDevice phys_dev, VkDevice dev, VkImageUsageFlags usage) {
  struct vk_import import;

  PFN_vkGetPhysicalDeviceImageFormatProperties2KHR vkGetPhysicalDeviceImageFormatProperties2KHR =
    (PFN_vkGetPhysicalDeviceImageFormatProperties2KHR)vkGetInstanceProcAddr(
      inst, "vkGetPhysicalDeviceImageFormatProperties2KHR");
  if (vkGetPhysicalDeviceImageFormatProperties2KHR == NULL) {
    ETRACE("vkGetInstanceProcAddr(\"vkGetPhysicalDeviceImageFormatProperties2KHR\") failed\n");
    import.res = VK_ERROR_INITIALIZATION_FAILED;
    return import;
  }

  VkFormat vk_format = GbmToVkFormat(format_);
  if (vk_format == VK_FORMAT_UNDEFINED) {
    ETRACE("Failed DRM -> Vulkan format conversion\n");
    import.res = VK_ERROR_FORMAT_NOT_SUPPORTED;
    return import;
  }

  VkPhysicalDeviceExternalImageFormatInfoKHR phys_ext_image_format = {};
  phys_ext_image_format.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR;
  phys_ext_image_format.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;

  VkPhysicalDeviceImageFormatInfo2KHR phys_image_format = {};
  phys_image_format.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR;
  phys_image_format.pNext = &phys_ext_image_format;
  phys_image_format.format = vk_format;
  phys_image_format.type = VK_IMAGE_TYPE_2D;
  phys_image_format.tiling = VK_IMAGE_TILING_OPTIMAL;
  phys_image_format.usage = usage;

  VkExternalImageFormatPropertiesKHR ext_image_format_props = {};
  ext_image_format_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES_KHR;

  VkImageFormatProperties2KHR image_format_props = {};
  image_format_props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2_KHR;
  image_format_props.pNext = &ext_image_format_props;

  import.res = vkGetPhysicalDeviceImageFormatProperties2KHR(phys_dev, &phys_image_format, &image_format_props);
  if (import.res != VK_SUCCESS) {
    ETRACE("vkGetPhysicalDeviceImageFormatProperties2KHR failed\n");
    return import;
  }

  if (!(ext_image_format_props.externalMemoryProperties.externalMemoryFeatures &
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR)) {
    ETRACE("Image format not supported for import to Vulkan\n");
    import.res = VK_ERROR_FORMAT_NOT_SUPPORTED;
    return import;
  }

  VkExtent3D image_extent = {};
  image_extent.width = width_;
  image_extent.height = height_;
  image_extent.depth = 1;

  uint32_t queue_index = 0;

  VkExternalMemoryImageCreateInfoKHR ext_mem_img_create = {};
  ext_mem_img_create.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_KHR;
  ext_mem_img_create.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;

  VkImageCreateInfo image_create = {};
  image_create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create.pNext = &ext_mem_img_create;
  image_create.imageType = VK_IMAGE_TYPE_2D;
  image_create.format = vk_format;
  image_create.extent = image_extent;
  image_create.mipLevels = 1;
  image_create.arrayLayers = 1;
  image_create.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create.usage = usage;
  image_create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create.queueFamilyIndexCount = 1;
  image_create.pQueueFamilyIndices = &queue_index;
  image_create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  import.res = vkCreateImage(dev, &image_create, NULL, &import.image);
  if (import.res != VK_SUCCESS) {
    ETRACE("vkCreateImage failed\n");
    return import;
  }

  VkMemoryRequirements mem_reqs;
  vkGetImageMemoryRequirements(dev, import.image, &mem_reqs);

  VkImportMemoryFdInfoKHR import_mem_info = {};
  import_mem_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  import_mem_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
  import_mem_info.fd = static_cast<int>(prime_fd_);

  uint32_t mem_type_index = ffs(mem_reqs.memoryTypeBits) - 1;

  VkMemoryAllocateInfo alloc_info = {};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.pNext = &import_mem_info;
  alloc_info.allocationSize = mem_reqs.size;
  alloc_info.memoryTypeIndex = mem_type_index;

  import.res = vkAllocateMemory(dev, &alloc_info, NULL, &import.memory);
  if (import.res != VK_SUCCESS) {
    ETRACE("vkAllocateMemory failed\n");
    return import;
  }

  if (!(mem_reqs.memoryTypeBits & (1 << mem_type_index))) {
    ETRACE("VkImage and dma_buf have incompatible VkMemoryTypes\n");
    import.res = VK_ERROR_FORMAT_NOT_SUPPORTED;
    return import;
  }

  import.res = vkBindImageMemory(dev, import.image, import.memory, 0);
  if (import.res != VK_SUCCESS) {
    ETRACE("vkBindImageMemory failed\n");
  }

  return import;
}

EGLImageKHR DrmBuffer::ImportImage(EGLDisplay egl_display) {
  EGLImageKHR image = EGL_NO_IMAGE_KHR;
  // Note: If eglCreateImageKHR is successful for a EGL_LINUX_DMA_BUF_EXT
  // target, the EGL will take a reference to the dma_buf.
  if (is_yuv_) {
    if (format_ == DRM_FORMAT_NV12) {
      const EGLint attr_list_nv12[] = {
          EGL_WIDTH,                     static_cast<EGLint>(width_),
          EGL_HEIGHT,                    static_cast<EGLint>(height_),
          EGL_LINUX_DRM_FOURCC_EXT,      static_cast<EGLint>(format_),
          EGL_DMA_BUF_PLANE0_FD_EXT,     static_cast<EGLint>(prime_fd_),
          EGL_DMA_BUF_PLANE0_PITCH_EXT,  static_cast<EGLint>(pitches_[0]),
          EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(offsets_[0]),
          EGL_DMA_BUF_PLANE1_FD_EXT,     static_cast<EGLint>(prime_fd_),
          EGL_DMA_BUF_PLANE1_PITCH_EXT,  static_cast<EGLint>(pitches_[1]),
          EGL_DMA_BUF_PLANE1_OFFSET_EXT, static_cast<EGLint>(offsets_[1]),
          EGL_NONE,                      0};
      image = eglCreateImageKHR(
          egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
          static_cast<EGLClientBuffer>(nullptr), attr_list_nv12);
    } else {
      const EGLint attr_list_yv12[] = {
          EGL_WIDTH,                     static_cast<EGLint>(width_),
          EGL_HEIGHT,                    static_cast<EGLint>(height_),
          EGL_LINUX_DRM_FOURCC_EXT,      static_cast<EGLint>(format_),
          EGL_DMA_BUF_PLANE0_FD_EXT,     static_cast<EGLint>(prime_fd_),
          EGL_DMA_BUF_PLANE0_PITCH_EXT,  static_cast<EGLint>(pitches_[0]),
          EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(offsets_[0]),
          EGL_DMA_BUF_PLANE1_FD_EXT,     static_cast<EGLint>(prime_fd_),
          EGL_DMA_BUF_PLANE1_PITCH_EXT,  static_cast<EGLint>(pitches_[1]),
          EGL_DMA_BUF_PLANE1_OFFSET_EXT, static_cast<EGLint>(offsets_[1]),
          EGL_DMA_BUF_PLANE2_FD_EXT,     static_cast<EGLint>(prime_fd_),
          EGL_DMA_BUF_PLANE2_PITCH_EXT,  static_cast<EGLint>(pitches_[2]),
          EGL_DMA_BUF_PLANE2_OFFSET_EXT, static_cast<EGLint>(offsets_[2]),
          EGL_NONE,                      0};
      image = eglCreateImageKHR(
          egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
          static_cast<EGLClientBuffer>(nullptr), attr_list_yv12);
    }
  } else {
    const EGLint attr_list[] = {
        EGL_WIDTH,                     static_cast<EGLint>(width_),
        EGL_HEIGHT,                    static_cast<EGLint>(height_),
        EGL_LINUX_DRM_FOURCC_EXT,      static_cast<EGLint>(format_),
        EGL_DMA_BUF_PLANE0_FD_EXT,     static_cast<EGLint>(prime_fd_),
        EGL_DMA_BUF_PLANE0_PITCH_EXT,  static_cast<EGLint>(pitches_[0]),
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_NONE,                      0};
    image =
        eglCreateImageKHR(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                          static_cast<EGLClientBuffer>(nullptr), attr_list);
  }

  return image;
}

void DrmBuffer::SetRecommendedFormat(uint32_t format) {
  frame_buffer_format_ = format;
}

bool DrmBuffer::CreateFrameBuffer(uint32_t gpu_fd) {
  ReleaseFrameBuffer();
  int ret = drmModeAddFB2(gpu_fd, width_, height_, frame_buffer_format_,
                          gem_handles_, pitches_, offsets_, &fb_id_, 0);

  if (ret) {
    ETRACE("drmModeAddFB2 error (%dx%d, %c%c%c%c, handle %d pitch %d) (%s)",
           width_, height_, frame_buffer_format_, frame_buffer_format_ >> 8,
           frame_buffer_format_ >> 16, frame_buffer_format_ >> 24,
           gem_handles_[0], pitches_[0], strerror(-ret));

    fb_id_ = 0;
    return false;
  }

  gpu_fd_ = gpu_fd;
  return true;
}

void DrmBuffer::ReleaseFrameBuffer() {
  if (fb_id_ && gpu_fd_ && drmModeRmFB(gpu_fd_, fb_id_))
    ETRACE("Failed to remove fb %s", PRINTERROR());

  fb_id_ = 0;
}

void DrmBuffer::Dump() {
  DUMPTRACE("DrmBuffer Information Starts. -------------");
  if (usage_ & kLayerNormal)
    DUMPTRACE("BufferUsage: kLayerNormal.");
  if (usage_ & kLayerCursor)
    DUMPTRACE("BufferUsage: kLayerCursor.");
  if (usage_ & kLayerProtected)
    DUMPTRACE("BufferUsage: kLayerProtected.");
  if (usage_ & kLayerVideo)
    DUMPTRACE("BufferUsage: kLayerVideo.");
  DUMPTRACE("Width: %d", width_);
  DUMPTRACE("Height: %d", height_);
  DUMPTRACE("Fb: %d", fb_id_);
  DUMPTRACE("Prime Handle: %d", prime_fd_);
  DUMPTRACE("Format: %4.4s", (char*)&format_);
  for (uint32_t i = 0; i < 4; i++) {
    DUMPTRACE("Pitch:%d value:%d", i, pitches_[i]);
    DUMPTRACE("Offset:%d value:%d", i, offsets_[i]);
    DUMPTRACE("Gem Handles:%d value:%d", i, gem_handles_[i]);
  }
  DUMPTRACE("DrmBuffer Information Ends. -------------");
}

OverlayBuffer* OverlayBuffer::CreateOverlayBuffer() {
  return new DrmBuffer();
}

}  // namespace hwcomposer
