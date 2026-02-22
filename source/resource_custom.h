#include "resource.h"
#include <vulkan/vulkan.hpp>

// Texture resource
class Texture : public Resource {
private:
	// Core Vulkan GPU resources for texture representation 
	vk::Image image;         // GPU image object containing pixel data
	vk::DeviceMemory memory; // GPU memory allocation backing the image
	vk::DeviceSize offset;   // Offset within the memory allocation for this texture
	vk::ImageView imageView; // Shader-accessible view into the image
	vk::Sampler sampler;     // Sampling configuration (filtering, wrapping, etc.)

	// Texture metadata for validation and debugging
	int width = 0;           // Image width in pixels
	int height = 0;          // Image height in pixels
	int channels = 0;        // Number of color channels (RGB=3, RGBA=4, etc.)

public:
	explicit Texture(const std::string& id) : Resource(id) {}

	~Texture() override {
		Unload();            // Ensure proper cleanup when object is destroyed
	}

	bool Load() override {
		// Step 2a: Construct file path using resource ID and expected format
		std::string filePath = "texture/" + GetId() + ".ktx";

		// Step 2b: Load raw image data from disk with format detetion
		unsigned char* data = LoadImageData(filePath, &width, &height, &channels);
		if (!data) {
			return false;    // Failed to load - return failure without partial state
		}

		// Step 2c: Transform raw pixel data into Vulkan GPU resources
		CreatevulkanImage(data, width, height, channels);

		// Step 2d: Clean up temporary CPU memory to prevent leaks
		FreeImageData(data);

		return Resource::Load(); // Mark resource as successfully loaded
	}
};