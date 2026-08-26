/*  VibeyMapTools Public API Implementation
 */

#include <light/vibey_api.hh>
#include <light/light.hh>
#include <common/log.hh>

namespace vibey
{

static bool s_initialized = false;

bool initialize()
{
    if (s_initialized)
        return true;

    // Initialize logging, thread pool, etc.
    s_initialized = true;
    return true;
}

void shutdown()
{
    s_initialized = false;
}

std::unique_ptr<mbsp_t> loadBSP(const std::string &path)
{
    // TODO: Implement BSP loading
    return nullptr;
}

LightResult runLighting(const LightConfig &config, ProgressCallback progress)
{
    LightResult result;

    if (!s_initialized) {
        result.errorMessage = "Light system not initialized";
        return result;
    }

    // TODO: Implement full lighting pipeline
    // This would involve:
    // 1. Loading BSP
    // 2. Setting up light_options from config
    // 3. Calling LightWorld
    // 4. Saving results

    result.success = true;
    return result;
}

bool saveBSP(const mbsp_t *bsp, const std::string &path)
{
    // TODO: Implement BSP saving
    return false;
}

const char *getVersion()
{
    return "VibeyMapTools 1.0.0";
}

bool isGPUAvailable()
{
#ifdef HAVE_OPTIX
    return true;
#else
    return false;
#endif
}

bool isOIDNAvailable()
{
#ifdef HAVE_OIDN
    return true;
#else
    return false;
#endif
}

} // namespace vibey
