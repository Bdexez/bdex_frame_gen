/* Lists the Vulkan instance layers the loader finds ("layer <name>" per
 * line) and creates an instance through them (tools/ci/flatpak-check.sh:
 * vulkaninfo is not in the Flatpak SDK). */
#include <stdio.h>
#include <vulkan/vulkan.h>

int main(void) {
  uint32_t n = 0;
  VkLayerProperties p[64];
  vkEnumerateInstanceLayerProperties(&n, NULL);
  if (n > 64) n = 64;
  vkEnumerateInstanceLayerProperties(&n, p);
  for (uint32_t i = 0; i < n; i++) printf("layer %s\n", p[i].layerName);
  VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  VkInstance inst;
  VkResult r = vkCreateInstance(&ci, NULL, &inst);
  printf("vkCreateInstance %d\n", (int)r);
  if (r == VK_SUCCESS) vkDestroyInstance(inst, NULL);
  return r != VK_SUCCESS;
}
