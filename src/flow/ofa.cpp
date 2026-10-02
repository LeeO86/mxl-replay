#include "flow/cuda_flow.hpp"

#include <cstdint>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace replay
{
namespace
{
using PFN_vkEnumerateInstanceExtensionProperties = int (*)(char const*, std::uint32_t*, void*);

struct VkExtensionProperties
{
    char name[256];
    std::uint32_t spec;
};
} // namespace

bool ofaProbe()
{
    void* library = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (library == nullptr)
    {
        return false;
    }
    auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(dlsym(library, "vkEnumerateInstanceExtensionProperties"));
    if (enumerate == nullptr)
    {
        dlclose(library);
        return false;
    }
    std::uint32_t count = 0;
    if (enumerate(nullptr, &count, nullptr) != 0 || count == 0)
    {
        dlclose(library);
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (enumerate(nullptr, &count, extensions.data()) != 0)
    {
        dlclose(library);
        return false;
    }
    bool found = false;
    for (auto const& extension : extensions)
    {
        if (std::string(extension.name) == "VK_NV_optical_flow")
        {
            found = true;
        }
    }
    dlclose(library);
    return found;
}
} // namespace replay
