/*  VibeyMapTools Public API

    This header provides the public interface for embedding the light tool
    as a library in other applications.
*/

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

// Forward declarations
struct mbsp_t;
struct lightsurf_t;
namespace settings
{
class worldspawn_keys;
}

namespace vibey
{

/**
 * @brief Progress callback type
 * @param current Current progress value
 * @param total Total progress value
 * @param message Status message
 */
using ProgressCallback = std::function<void(int current, int total, const char *message)>;

/**
 * @brief Light tool configuration
 */
struct LightConfig
{
    std::string bspPath; ///< Path to input BSP file
    bool enableBounce = true; ///< Enable indirect lighting
    int bounces = 2; ///< Number of light bounces
    int extra = 1; ///< Extra sampling (1, 2, or 4)
    bool denoise = false; ///< Enable denoising
    bool stochastic = false; ///< Enable stochastic sampling
    bool gpu = false; ///< Enable GPU acceleration
    float minlight = 0.0f; ///< Minimum light level
    float scale = 1.0f; ///< Light scale factor
};

/**
 * @brief Result from lighting operation
 */
struct LightResult
{
    bool success = false;
    std::string errorMessage;
    int facesProcessed = 0;
    int raysTraced = 0;
    double elapsedSeconds = 0.0;
};

/**
 * @brief Initialize the lighting system
 *
 * Must be called before any other light functions.
 *
 * @return true on success, false on failure
 */
bool initialize();

/**
 * @brief Shutdown the lighting system
 *
 * Releases all resources. Call when done with lighting.
 */
void shutdown();

/**
 * @brief Load a BSP file for lighting
 *
 * @param path Path to the BSP file
 * @return Pointer to loaded BSP, or nullptr on failure
 */
std::unique_ptr<mbsp_t> loadBSP(const std::string &path);

/**
 * @brief Run the lighting process
 *
 * @param config Lighting configuration
 * @param progress Optional progress callback
 * @return Result of the lighting operation
 */
LightResult runLighting(const LightConfig &config, ProgressCallback progress = nullptr);

/**
 * @brief Save the lit BSP file
 *
 * @param bsp The BSP to save
 * @param path Output path
 * @return true on success
 */
bool saveBSP(const mbsp_t *bsp, const std::string &path);

/**
 * @brief Get version string
 */
const char *getVersion();

/**
 * @brief Check if GPU acceleration is available
 */
bool isGPUAvailable();

/**
 * @brief Check if OIDN denoising is available
 */
bool isOIDNAvailable();

} // namespace vibey
