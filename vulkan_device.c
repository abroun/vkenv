#include "vulkan_device.h"
#include "logger.h"
#include "vulkan_utils.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const char LOG_TAG[] = "VulkanDevice";

bool createInstance(vkenv_InstanceConfig *config);
bool getPhysicalDevice(vkenv_Device device, vkenv_DeviceConfig *config);
bool createLogicalDevice(vkenv_Device device, vkenv_DeviceConfig *config);

static VkInstance vulkan_instance = VK_NULL_HANDLE;

bool vkenv_createInstance(vkenv_InstanceConfig *config_ptr)
{
  if (vulkan_instance == VK_NULL_HANDLE)
  {
    if (vkenv_loadVulkanInstanceCreationFuncs() && createInstance(config_ptr))
    {
      vkenv_loadVulkanAPI(vulkan_instance);
      return true;
    }
    else
    {
      logError(LOG_TAG, "vkenv_createInstance() failure");
      // Safely destroy Vulkan entities
      vkenv_destroyInstance();
      return false;
    }
  }
  else
  {
    logError(LOG_TAG, "vkenv_createInstance() failure: two Vulkan instances cannot exist at the same time.");
    return false;
  }
}

VkInstance vkenv_getInstance() { return vulkan_instance; }

void vkenv_destroyInstance()
{
  // Destroy instance
  VK_NULL_SAFE_DELETE(vulkan_instance, vkDestroyInstance(vulkan_instance, NULL));
  // Release Vulkan dynamic library
  vkenv_unloadVulkan();
  // Reset vulkan instance to NULL (allow vkenv_createInstance to be called again)
  vulkan_instance = VK_NULL_HANDLE;
}

bool vkenv_createDevice(vkenv_Device *device_ptr, vkenv_DeviceConfig *config_ptr)
{
  assert(vulkan_instance != NULL); // Vulkan instance must be valid
  assert(device_ptr != NULL);
  assert(config_ptr != NULL);

  assert(config_ptr->nb_general_queues > 0); // We must always have at least one usable queue

  // Allocate vkenv_Device
  *device_ptr = (vkenv_Device)malloc(sizeof(struct vkenv_Device_T));
  vkenv_Device device = *device_ptr;
  // Reset everything in the struct to 0 or NULL (important to only destroy allocated objects if there an issue)
  memset(device, 0, sizeof(struct vkenv_Device_T));

  // Reserve VkQueue arrays
  device->general_queues = (VkQueue *)malloc(sizeof(VkQueue) * config_ptr->nb_general_queues);
  device->async_compute_queues = (VkQueue *)malloc(sizeof(VkQueue) * config_ptr->nb_async_compute_queues);
  device->async_transfer_queues = (VkQueue *)malloc(sizeof(VkQueue) * config_ptr->nb_async_transfer_queues);

  if (getPhysicalDevice(device, config_ptr) && createLogicalDevice(device, config_ptr))
  {
    return true;
  }
  else
  {
    logError(LOG_TAG, "vkenv_Device creation failed");
    // Safely destroy Vulkan entities
    vkenv_destroyDevice(device_ptr);
    return false;
  }
}

void vkenv_destroyDevice(vkenv_Device *device_ptr)
{
  assert(vulkan_instance != NULL); // Vulkan instance must be valid
  assert(device_ptr != NULL);
  assert(*device_ptr != NULL); // destroyDevice shouldn't be called on NULL Device

  // Destroy logical device
  VK_NULL_SAFE_DELETE((*device_ptr)->device, vkDestroyDevice((*device_ptr)->device, NULL));

  // Free queue arrays
  free((*device_ptr)->general_queues);
  free((*device_ptr)->async_compute_queues);
  free((*device_ptr)->async_transfer_queues);

  // Releave vkenv_Device memory
  free(*device_ptr);
  *device_ptr = NULL;
}

// Helper function to get a list of properties for the available physical devices
void vkenv_getPhysicalDevicesProperties(uint32_t *nb_devices, VkPhysicalDeviceProperties *devices_properties)
{
  assert(vulkan_instance != NULL); // Vulkan instance must be valid
  assert(nb_devices != NULL);

  // If devices is NULL, this only fills the nb_devices, otherwise data is copied to devices
  if (devices_properties == NULL)
  {
    vkEnumeratePhysicalDevices(vulkan_instance, nb_devices, NULL);
  }
  else
  {
    VkPhysicalDevice *devices = (VkPhysicalDevice *)malloc(sizeof(VkPhysicalDevice) * (*nb_devices));
    vkEnumeratePhysicalDevices(vulkan_instance, nb_devices, devices);
    if (devices != NULL)
    {
      for (uint32_t i = 0; i < (*nb_devices); i++)
      {
        vkGetPhysicalDeviceProperties(devices[i], &devices_properties[i]);
      }
    }
    free(devices);
  }
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

bool getRequestedExtensionsSupported(uint32_t platform_extension_count, VkExtensionProperties *platform_extensions, uint32_t requested_extension_count,
                                     const char **requested_extensions, bool *mask)
{
  // Check if the extension names in requested_extensions are found the platform extensions
  // When an requested extension is found, the mask value at the same index is set to true
  memset(mask, 0, sizeof(bool) * requested_extension_count);
  uint32_t nb_found = 0;
  for (uint32_t platform_idx = 0; platform_idx < platform_extension_count; platform_idx++)
  {
    for (uint32_t req_idx = 0; req_idx < requested_extension_count; req_idx++)
    {
      if (strcmp(platform_extensions[platform_idx].extensionName, requested_extensions[req_idx]) == 0)
      {
        mask[req_idx] = true;
        nb_found++;
      }
    }
  }
  return nb_found >= requested_extension_count;
}

bool createInstance(vkenv_InstanceConfig *config)
{
  // Check that requested instance extensions are supported
  uint32_t available_extension_cnt = 0;
  vkEnumerateInstanceExtensionProperties(NULL, &available_extension_cnt, NULL);
  VkExtensionProperties *available_extensions = (VkExtensionProperties *)malloc(sizeof(VkExtensionProperties) * available_extension_cnt);
  bool *requested_extensions_mask = (bool *)malloc(sizeof(bool) * config->instance_extension_count);
  vkEnumerateInstanceExtensionProperties(NULL, &available_extension_cnt, available_extensions);
  bool requested_extensions_supported = getRequestedExtensionsSupported(available_extension_cnt, available_extensions, config->instance_extension_count,
                                                                        config->instance_extensions, requested_extensions_mask);
  if (requested_extensions_supported == false)
  {
    logError(LOG_TAG, "Could not create VulkanInstance. The following required extensions are not supported: ");
    for (uint32_t i = 0; i < config->instance_extension_count; i++)
    {
      if (requested_extensions_mask[i] == false)
      {
        logError(LOG_TAG, "\t - %s", config->instance_extensions[i]);
      }
    }
  }
  free(requested_extensions_mask);
  free(available_extensions);

  if (requested_extensions_supported == false)
  {
    return false;
  }

  // Create VkInstance
  VkApplicationInfo app_info = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .pNext = NULL,
                                .pApplicationName = config->application_name,
                                .applicationVersion = config->application_version,
                                .pEngineName = config->engine_name,
                                .engineVersion = config->engine_version,
                                .apiVersion = config->vulkan_api_version};

  VkInstanceCreateInfo instance_create_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                               .pNext = NULL,
                                               .flags = 0,
                                               .pApplicationInfo = &app_info,
                                               .enabledLayerCount = config->validation_layer_count,
                                               .ppEnabledLayerNames = config->validation_layers,
                                               .enabledExtensionCount = config->instance_extension_count,
                                               .ppEnabledExtensionNames = config->instance_extensions};

  VkResult create_instance_res = vkCreateInstance(&instance_create_info, NULL, &vulkan_instance);
  if (create_instance_res != VK_SUCCESS)
  {
    logError(LOG_TAG, "Vulkan instance creation failed (vkCreateInstance: %s)", vkenv_getVkResultString(create_instance_res));
    return false;
  }
  return true;
}

bool findQueueFamilyIndex(VkPhysicalDevice physical_device, uint32_t *queue_family_idx, uint32_t present_flagbits, uint32_t absent_flagbits,
                          const uint32_t req_queue_cnt)
{
  // If no queue will be used there's no need to continue
  if (req_queue_cnt == 0)
  {
    return false;
  }

  bool queue_idx_found = false;
  // Request queue families info
  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, NULL);
  VkQueueFamilyProperties *queue_families = (VkQueueFamilyProperties *)malloc(queue_family_count * sizeof(VkQueueFamilyProperties));
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families);
  *queue_family_idx = 0;
  for (uint32_t i = 0; i < queue_family_count; i++)
  {
    // Check every bit/flag of the queue family flags and verify that bits set in present_flagbits are also set in the queue family flags
    // and that bits set in absent_flagbits are not set in the queue family flags.
    bool conditions_met = true;
    for (int bit_idx = 0; bit_idx < 32; bit_idx++)
    {
      uint32_t bit = 1 << bit_idx;
      uint32_t present_bit = present_flagbits & bit;
      uint32_t absent_bit = absent_flagbits & bit;
      uint32_t queue_family_flags_bit = queue_families[i].queueFlags & bit;
      if ((present_bit && !(present_flagbits & queue_family_flags_bit)) || (absent_bit && absent_bit & queue_family_flags_bit))
      {
        // If bit is required and not found in the queue family flags or if bit must not be set but is found in the queue family flags
        // stop checking this queue
        conditions_met = false;
        break;
      }
    }
    if (conditions_met && queue_families[i].queueCount >= req_queue_cnt) // also check that the desired number of queue can be used with this queue family
    {
      *queue_family_idx = i;
      queue_idx_found = true;
      break;
    }
  }
  free(queue_families);
  return queue_idx_found;
}

float getPhysicalDeviceCapabilityScore(VkPhysicalDevice device, uint32_t required_device_extension_count, const char **required_device_extensions,
                                       const uint32_t req_general_queue_cnt, const uint32_t req_compute_queue_cnt, const uint32_t req_transfer_queue_cnt,
                                       bool allow_cpu)
{
  // Need present if GPU_DEBUG and support
  // GPU score = queue_types_multiplier * heap_size_multiplier
  // queue_types_multiplier = number of different queue types available (general purpose, async compute, async transfer)
  // heap_size_multiplier = available GPU memory size in Gb
  // If allow_cpu is false and the GPU is not a DISCRETE_GPU or an INTEGRATED_GPU, the returned score will be 0.f
  // If the requested device extensions are not support by the GPU, the returned score will be 0.f

  float valid_type_multiplier = 0.f;
  float queue_types_multiplier = 0.f;
  float extensions_supported_multiplier = 0.f;
  float heap_size_multiplier = 0.f;

  // Check GPU type (only accept INTEGRATED or DEDICATED GPUs if allow_cpu is false)
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(device, &props);
  if (!allow_cpu && props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
  {
    logInfo(LOG_TAG, "\t\t -> Invalid GPU type");
    valid_type_multiplier = 0.f;
  }
  else
  {
    logInfo(LOG_TAG, "\t\t -> Valid GPU type");
    valid_type_multiplier = 1.f;
  }

  // Check queue family types availability
  uint32_t general_queue_family_idx, tmp_queue_idx = 0;
  bool general_queue_available =
      findQueueFamilyIndex(device, &general_queue_family_idx, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0, req_general_queue_cnt);
  bool compute_queue_available = findQueueFamilyIndex(device, &tmp_queue_idx, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT, req_compute_queue_cnt);
  bool transfer_queue_available =
      findQueueFamilyIndex(device, &tmp_queue_idx, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, req_transfer_queue_cnt);
  // For the multiplier, the general queue is mandatory, all other async queue add 1.0 to the multiplier
  if (general_queue_available)
  {
    queue_types_multiplier = 1.f + (compute_queue_available ? 1.f : 0.f) + (transfer_queue_available ? 1.f : 0.f);
    logInfo(LOG_TAG, "\t\t -> General-purpose queue family available", (int)queue_types_multiplier);
    if (compute_queue_available)
    {
      logInfo(LOG_TAG, "\t\t -> Support requirements on async-compute queues");
    }
    if (transfer_queue_available)
    {
      logInfo(LOG_TAG, "\t\t -> Support requirements on async-transfer queues");
    }
  }
  else
  {
    logInfo(LOG_TAG, "\t\t -> No general purpose queue family available or queue count requirement not met");
    queue_types_multiplier = 0.f;
  }

  // Check that device extensions are supported
  uint32_t available_device_extension_count = 0u;
  vkEnumerateDeviceExtensionProperties(device, NULL, &available_device_extension_count, NULL);
  VkExtensionProperties *available_extensions = (VkExtensionProperties *)malloc(sizeof(VkExtensionProperties) * available_device_extension_count);
  bool *required_extensions_mask = (bool *)malloc(sizeof(bool) * required_device_extension_count);
  vkEnumerateDeviceExtensionProperties(device, NULL, &available_device_extension_count, available_extensions);
  bool requested_extensions_supported = getRequestedExtensionsSupported(
      available_device_extension_count, available_extensions, required_device_extension_count, required_device_extensions, required_extensions_mask);

  if (requested_extensions_supported == false)
  {
    logInfo(LOG_TAG, "\t\t -> Missing required device extension(s):");
    for (uint32_t i = 0; i < required_device_extension_count; i++)
    {
      if (required_extensions_mask[i] == false)
      {
        logInfo(LOG_TAG, "\t\t\t\"%s\"", required_device_extensions[i]);
      }
    }
    extensions_supported_multiplier = 0.f;
  }
  else
  {
    logInfo(LOG_TAG, "\t\t -> Required device extensions supported");
    extensions_supported_multiplier = 1.f;
  }
  free(required_extensions_mask);
  free(available_extensions);

  // Compute total heap size
  VkPhysicalDeviceMemoryProperties memory_props;
  vkGetPhysicalDeviceMemoryProperties(device, &memory_props);
  uint64_t available_memory_sum = 0u;
  for (uint32_t i = 0; i < memory_props.memoryHeapCount; i++)
  {
    if (memory_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
    {
      available_memory_sum += memory_props.memoryHeaps[i].size;
    }
  }
  heap_size_multiplier = ((float)available_memory_sum) / 1000000000;
  logInfo(LOG_TAG, "\t\t -> Device local memory size %f Gbytes", heap_size_multiplier);
  return valid_type_multiplier * extensions_supported_multiplier * queue_types_multiplier * heap_size_multiplier;
}

bool getPhysicalDevice(vkenv_Device device, vkenv_DeviceConfig *config)
{
  // Retrieve a list of Vulkan-enabled GPUs
  uint32_t nb_devices = 0u;
  vkEnumeratePhysicalDevices(vulkan_instance, &nb_devices, NULL);
  if (nb_devices == 0)
  {
    logError(LOG_TAG, "No GPU with Vulkan support found.");
    return false;
  }

  VkPhysicalDevice *devices = (VkPhysicalDevice *)malloc(nb_devices * sizeof(VkPhysicalDevice));
  vkEnumeratePhysicalDevices(vulkan_instance, &nb_devices, devices);
  bool valid_gpu_found = false;

  if (config->target_device_idx < 0)
  {
    // If not device has been selected by the user, we need to evaluate physical devices candidates and pick the best one
    logInfo(LOG_TAG, "Looking for GPU candidates:");
    // Select best GPU available with a score function getPhysicalDeviceCapabilityScore
    float best_gpu_score = 0.f;
    for (uint32_t i = 0; i < nb_devices; i++)
    {
      VkPhysicalDeviceProperties candidate_props;
      vkGetPhysicalDeviceProperties(devices[i], &candidate_props);
      logInfo(LOG_TAG, "\t Device %d (name: %s, device ID: %d,vendor ID: %d)", i, candidate_props.deviceName, candidate_props.deviceID,
              candidate_props.vendorID);
      float score = getPhysicalDeviceCapabilityScore(devices[i], config->device_extension_count, config->device_extensions, config->nb_general_queues,
                                                     config->nb_async_compute_queues, config->nb_async_transfer_queues, config->allow_cpu);
      if (score > best_gpu_score)
      {
        best_gpu_score = score;
        device->physical_device = devices[i];
      }
      logInfo(LOG_TAG, "\t\t -> Device score: %f (%s)", score, (score == 0.f) ? "Invalid" : "Valid");
    }
    // If no valid device found
    if (best_gpu_score > 0.f)
    {
      valid_gpu_found = true;
    }
    else
    {
      logError(LOG_TAG, "No valid GPU found.");
    }
  }
  else
  {
    // GPU selected by the user, pick the one corresponding to config->target_device_idx
    if ((uint32_t)config->target_device_idx >= nb_devices)
    {
      logError(LOG_TAG, "Provided target_device_idx(%d) in vkenv_Context_Config is invalid (out of available GPU range)", config->target_device_idx);
    }
    else
    {
      device->physical_device = devices[config->target_device_idx];
      // Mandatory device extensions check
      uint32_t available_device_extension_count = 0u;
      vkEnumerateDeviceExtensionProperties(device->physical_device, NULL, &available_device_extension_count, NULL);
      VkExtensionProperties *available_extensions = (VkExtensionProperties *)malloc(sizeof(VkExtensionProperties) * available_device_extension_count);
      bool *required_extensions_mask = (bool *)malloc(sizeof(bool) * config->device_extension_count);
      vkEnumerateDeviceExtensionProperties(device->physical_device, NULL, &available_device_extension_count, available_extensions);
      bool requested_extensions_supported = getRequestedExtensionsSupported(
          available_device_extension_count, available_extensions, config->device_extension_count, config->device_extensions, required_extensions_mask);

      if (requested_extensions_supported == false)
      {
        logError(LOG_TAG, "GPU selection is invalid. Missing required device extension(s):");
        for (uint32_t i = 0; i < config->device_extension_count; i++)
        {
          if (required_extensions_mask[i] == false)
          {
            logError(LOG_TAG, "\t -> %s", config->device_extensions[i]);
          }
        }
      }
      else if (!findQueueFamilyIndex(device->physical_device, &device->general_queues_family_idx, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0,
                                     config->nb_general_queues))
      {
        logError(LOG_TAG, "GPU selection is invalid. No general purpose queue family available or queue count requirement not met.");
      }
      else
      {
        valid_gpu_found = true;
      }
      free(required_extensions_mask);
      free(available_extensions);
    }
  }
  free(devices);

  if (valid_gpu_found)
  {
    // If we have a valid physical device, retrieve and store its main properties
    // Retrieve physical device relevant info (properties, select queues)
    vkGetPhysicalDeviceProperties(device->physical_device, &(device->physical_device_props));
    vkGetPhysicalDeviceMemoryProperties(device->physical_device, &(device->physical_device_memory_props));
    // Find the device queue family indices (we're looking for general purpose, async compute and async transfer queues)
    findQueueFamilyIndex(device->physical_device, &device->general_queues_family_idx, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0,
                         config->nb_general_queues);
    device->async_compute_available = findQueueFamilyIndex(device->physical_device, &device->async_compute_queues_family_idx, VK_QUEUE_COMPUTE_BIT,
                                                           VK_QUEUE_GRAPHICS_BIT, config->nb_async_compute_queues);
    device->async_transfer_available = findQueueFamilyIndex(device->physical_device, &device->async_transfer_queues_family_idx, VK_QUEUE_TRANSFER_BIT,
                                                            VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, config->nb_async_transfer_queues);

    // Print out debug informations about selected device
    logInfo(LOG_TAG, "Selected GPU: %s [device ID=%d][vendor ID=%d]", device->physical_device_props.deviceName, device->physical_device_props.deviceID,
            device->physical_device_props.vendorID);
    if (config->nb_async_compute_queues > 0)
    {
      logInfo(LOG_TAG, "GPU async compute support: %d", device->async_compute_available);
    }
    if (config->nb_async_transfer_queues > 0)
    {
      logInfo(LOG_TAG, "GPU async transfer support: %d", device->async_transfer_available);
    }
  }

  return valid_gpu_found;
}

bool createLogicalDevice(vkenv_Device device, vkenv_DeviceConfig *config)
{
  bool creation_success = true;

  // Update queue count in the device structure
  device->general_queue_cnt = config->nb_general_queues;
  device->async_compute_queue_cnt = (device->async_compute_available ? config->nb_async_compute_queues : 0);
  device->async_transfer_queue_cnt = (device->async_transfer_available ? config->nb_async_transfer_queues : 0);

  // Create a Vulkan device giving access to all available queue families found
  uint32_t queue_create_info_count = 1 + (device->async_compute_available ? 1 : 0) + (device->async_transfer_available ? 1 : 0);
  VkDeviceQueueCreateInfo *queue_create_infos = (VkDeviceQueueCreateInfo *)malloc(sizeof(VkDeviceQueueCreateInfo) * queue_create_info_count);
  float *queue_priorities = (float *)malloc(sizeof(float) * queue_create_info_count);
  for (uint32_t i = 0; i < queue_create_info_count; i++)
  {
    queue_priorities[i] = 1.f;
  }

  queue_create_infos[0] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                    .pNext = NULL,
                                                    .flags = 0,
                                                    .queueFamilyIndex = device->general_queues_family_idx,
                                                    .queueCount = device->general_queue_cnt,
                                                    .pQueuePriorities = queue_priorities};
  int cnt = 1;
  if (device->async_compute_available)
  {
    queue_create_infos[cnt] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                        .pNext = NULL,
                                                        .flags = 0,
                                                        .queueFamilyIndex = device->async_compute_queues_family_idx,
                                                        .queueCount = device->async_compute_queue_cnt,
                                                        .pQueuePriorities = queue_priorities};
    cnt++;
  }
  if (device->async_transfer_available)
  {
    queue_create_infos[cnt] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                        .pNext = NULL,
                                                        .flags = 0,
                                                        .queueFamilyIndex = device->async_transfer_queues_family_idx,
                                                        .queueCount = device->async_transfer_queue_cnt,
                                                        .pQueuePriorities = queue_priorities};
    cnt++;
  }

  VkDeviceCreateInfo device_create_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                           .pNext = NULL,
                                           .flags = 0,
                                           .queueCreateInfoCount = queue_create_info_count,
                                           .pQueueCreateInfos = queue_create_infos,
                                           .enabledLayerCount = 0, // device layers are deprecated
                                           .ppEnabledLayerNames = NULL,
                                           .enabledExtensionCount = config->device_extension_count,
                                           .ppEnabledExtensionNames = config->device_extensions,
                                           .pEnabledFeatures = NULL};

  VkResult create_device_res = vkCreateDevice(device->physical_device, &device_create_info, NULL, &device->device);
  if (create_device_res != VK_SUCCESS)
  {
    logError(LOG_TAG, "Failed to create logical device (vkCreateDevice: %s)", vkenv_getVkResultString(create_device_res));
    creation_success = false;
  }
  else
  {
    // Retrieve general queues
    for (uint32_t i = 0; i < device->general_queue_cnt; i++)
    {
      vkGetDeviceQueue(device->device, device->general_queues_family_idx, i, &device->general_queues[i]);
    }

    // Retrieve async compute queues
    if (device->async_compute_available)
    {
      for (uint32_t i = 0; i < device->async_compute_queue_cnt; i++)
      {
        vkGetDeviceQueue(device->device, device->async_compute_queues_family_idx, i, &device->async_compute_queues[i]);
      }
    }

    // Retrieve async transfer queues
    if (device->async_transfer_available)
    {
      for (uint32_t i = 0; i < device->async_transfer_queue_cnt; i++)
      {
        vkGetDeviceQueue(device->device, device->async_transfer_queues_family_idx, i, &device->async_transfer_queues[i]);
      }
    }
  }
  free(queue_create_infos);
  free(queue_priorities);

  return creation_success;
}