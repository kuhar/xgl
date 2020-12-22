#include "vulkan/vulkan.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

void printHelp() {
  std::cerr << "mk-empty-cache -- Vulkan application to create an empty Vulkan Pipeline Cache file\n\n"
            << "USAGE:\t mk-empty-cache output-cache-file.bin\n";
}

int main(int argc, char **argv) {
  if (argc != 2) {
    printHelp();
    return 3;
  }

  std::string firstArg(argv[1]);
  if (firstArg.empty() || firstArg.front() == '-' || firstArg.front() == '?' || firstArg.find("help") == 0) {
    printHelp();
    return 3;
  }

  int cacheFD = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, mode_t(0666));
  if (cacheFD == -1) {
    perror("Error opening output cache file for write");
    return 3;
  }

#define CHECKED(VK_CALL...)                                                                                            \
  do {                                                                                                                 \
    auto vkCallRes = (VK_CALL);                                                                                        \
    if (vkCallRes != VK_SUCCESS) {                                                                                     \
      std::cerr << #VK_CALL << " failed, value = " << static_cast<int64_t>(vkCallRes) << " (" << __FILE__ << ":"       \
                << __LINE__ << ")\n";                                                                                  \
      return 4;                                                                                                        \
    }                                                                                                                  \
  } while (false)

  VkApplicationInfo appInfo = {};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "mk-empty-cache";
  appInfo.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
  appInfo.pEngineName = "No Engine (headless)";
  appInfo.engineVersion = VK_MAKE_VERSION(0, 0, 1);
  appInfo.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  VkInstance instance = nullptr;
  CHECKED(vkCreateInstance(&createInfo, nullptr, &instance));

  uint32_t gpuCount = 0;
  CHECKED(vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr));
  std::vector<VkPhysicalDevice> physicalDevices(gpuCount);
  CHECKED(vkEnumeratePhysicalDevices(instance, &gpuCount, physicalDevices.data()));
  VkPhysicalDevice physicalDevice = physicalDevices[0];

  VkDeviceCreateInfo deviceCreateInfo = {};
  deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceCreateInfo.queueCreateInfoCount = 0;
  VkDevice device = nullptr;
  CHECKED(vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device));

  VkPipelineCacheCreateInfo pipelineCacheCreateInfo = {};
  pipelineCacheCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  pipelineCacheCreateInfo.initialDataSize = 0;
  pipelineCacheCreateInfo.pInitialData = nullptr;
  VkPipelineCache pipelineCache = nullptr;
  CHECKED(vkCreatePipelineCache(device, &pipelineCacheCreateInfo, nullptr, &pipelineCache));

  size_t pipelineCacheSize = 0;
  CHECKED(vkGetPipelineCacheData(device, pipelineCache, &pipelineCacheSize, nullptr));
  if (pipelineCacheSize == 0) {
    std::cerr << "Queried pipeline cache data is, unexpectedly, empty\n";
    return 4;
  }

  if (ftruncate(cacheFD, pipelineCacheSize) != 0) {
    perror("Faield to resize the output cache file");
    return 4;
  }

  void *outBuffer = mmap(nullptr, pipelineCacheSize, PROT_READ | PROT_WRITE, MAP_SHARED, cacheFD, 0);
  if (outBuffer == MAP_FAILED) {
    perror("Failed to mmap the output file");
    return 4;
  }

  CHECKED(vkGetPipelineCacheData(device, pipelineCache, &pipelineCacheSize, outBuffer));
  std::cout << "Pipeline cache data successfully written to " << firstArg << std::endl;

  munmap(outBuffer, pipelineCacheSize);
  close(cacheFD);

  vkDestroyPipelineCache(device, pipelineCache, nullptr);
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);

  return 0;
#undef CHECKED
}