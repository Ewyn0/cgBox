#include <string>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <functional>
#include <vector>
#include <unordered_map>
#include "component.h"
#include "resource_custom.h"

class CullingSystem {
private:
	Camera* camera;
	std::vector<Entity*> visibleEntities;

public:
	explicit CullingSystem(Camera* cam) : camera(cam) {}

	void SetCamera(Camera* cam) {
		camera = cam;
	}

	void CullScene(const std::vector<Entity*>& allEntities) {
		visibleEntities.clear();

		if (!camera) return;

		// Get camera frustum
		Frustum rustum = camera->GetFrustum();

		// Check each entity against the frustum
		for (auto entity : allEntities) {
			if (!entity->IsActive()) continue;

			auto meshComponent = entity->GetComponent<MeshComponent>();
			if (!meshComponent) continue;

			auto transformComponent = entity->GetComponent<TransformComponent>();
			if (!transformComponent) continue;

			// Get bounding box of the mesh
			BoundingBox boundingBox = meshComponent->GetBoundingBox();

			// Transform bounding box by entity transform
			boundingBox.Transform(transformComponent->GetTransformMatrix());

			// Check if bounding box is visible
			if (frustum.Intersects(boundingBox)) {
				visibleEntities.push_back(entity);
			}
		}
	}

	const std::vector<Entity*>& GetVisibleEntities() const {
		return visibleEntities;
	}
};

// A comprehensive rendergraph implementation for automated dependency management
class Rendergraph {
private:
	// Resource description and management structure
	// Represents Image resource used during rendering (textures)
	struct ImageResource {
		std::string name;                     // Human-readable identifier for debugging and referencing
		vk::Format format;                    // Pixel format (RGBA8, Depth24Stencil8, etc.)
		vk::Extent2D extent;                  // Dimensions in pixels for 2D resources
		vk::ImageUsageFlags usage;            // How this resource will be used (color attachment, texture, etc.)
		vk::ImageLayout initialLayout;        // Expected layout when the frame begins
		vk::ImageLayout finalLayout;          // Required layout when the frame end

		// Actual GPU resources - populated during compilation
		vk::raii::Image image = nullptr;      // The GPU image object
		vk::raii::DeviceMemory memory = nullptr;  // Backing memory allocation
		vk::raii::ImageView view = nullptr;   // Shader-accessible view of the image 
	};

	// Render pass representation within the graph structure
	// Each pass represents a distict rendering operation with defined inputs and outputs
	struct Pass {
		std::string name;                     // Descriptive name for debugging and profiling
		std::vector<std::string> inputs;      // Resource this pass reads from (dependencies)
		std::vector<std::string> outputs;     // Resources this pass writes to (products)
		std::function<void(vk::raii::CommandBuffer&)> executeFunc;  // The actual rendering code
	};

	// Core data storage for the rendergraph system
	std::unordered_map<std::string, Resource> resources; // All resources referenced in the graph
	std::vector<Pass> passes;                            // All rendering passes in definition order
	std::vector<size_t> executionOrder;                  // Computed optimal execution sequence

	// Automatic synchronization management
	// These objects ensure correct GPU execution order without manual barriers
	std::vector<vk::raii:Semaphore> semaphores;          // GPU synchronization primitives
	std::vector<std::pair<size_t, size_t>> semaphoreSignalWaitPairs; // (signaling pass, waiting pass)

	vk::raii:Device & device;  // Vulkan device for resource creation

public:
	explicit Rendergraph(vk::raii::Device& dev) : device(dev) {}

	// Resource registration interface for declaring all resources used during rendering
	// This method establishes resource metadata without creating actual GPU resources
	void AddResource(const std::string& name, vk::Format format, vk::Extent2D extent,
		vk::ImageUsageFlags usage, vk::ImageLayout initialLayout,
		vk::ImageLayout finalLayout) {

		Resource resource;
		resource.name = name;                // Store human-readable identifier
		resource.format = format;            // Define pixel format and bit depth
		resource.extent = extent;            // Set resource dimensions
		resource.usage = usage;              // Specify intended usage patterns
		resource.initialLayout = initialLayout; // Define starting layout state
		resource.finalLayout = finalLayout;  // Define required ending state

		resources[name] = resource;          // Register in the global resource map
	}

	// Pass registration interface for defining rendering operations and their dependencies
	// This method establishes the logical structure of rendering without immediate execution
	void AddPass(const std::string& name,
		const std::vector<std::string>& inputs,
		const std::vector<std::string>& outputs,
		std::function<void(vk::raii::CommandBuffer&)> executeFunc) {

		Pass pass;
		pass.name = name;                    // Assign descriptive identifier
		pass.inputs = inputs;                // List all resources this pass reads
		pass.outputs = outputs;              // List all resources this pass writes
		pass.executeFunc = executeFunc;      // Store the actual rendering implementation

		passes.push_back(pass);              // Add to the ordered pass list
	}

	// Rendergraph compilation - transforms declarative descriptions into executable pipeline
	// This method performs dependency analysis, resource allocation, and execution planning
	void Compile() {
		// Dependency Graph Construction
		// Build bidirectional dependency relationships between passes
		std::vector<std::vector<size_t>> dependencies(passes.size());   // What each pass depends on
		std::vector<std::vector<size_t>> dependents(passes.size());     // What depends on each pass

		// Track which pass produces each resource (write-after-write dependencies)
		std::unordered_map<std::string, size_t> resourceWriters;

		// Dependency Discovery Through Resource Usage Analysis
		// Analyze each pass to determine data flow relationships
		for (size_t i = 0; i < passes.size(); ++i) {
			const auto& pass = passes[i];

			// Process input dependencies - this pass must wait for producers
			for (const auto& input : pass.inputs) {
				auto it = resourceWriters.find(input);
				if (it != resourceWriters.end()) {
					// Found the pass that produces this input - create dependency link
					dependencies[i].push_back(it->second);     // This pass depends on the producer
					dependents[it->second].push_back(i);       // Producer has this as dependent
				}
			}

			// Register output production - subsequent passes may depend on these
			for (const auto& output : pass.outputs) {
				resourceWriters[output] = i;                   // Record this pass as producer
			}
		}

		// Topological Sort for Optimal Execution Order
		// Use depth-first search to compute valid execution sequence while detecting cycles
		std::vector<bool> visited(passes.size(), false);       // Track completed nodes
		std::vector<bool> inStack(passes.size(), false);       // Track current recursion path

		std::function<void(size_t)> visit = [&](size_t node) {
			if (inStack[node]) {
				// Cycle detection - circular dependency found
				throw std::runtime_error("Cycle detected in rendergraph");
			}

			if (visited[node]) {
				return; // Already processed this node and its dependencies
			}

			inStack[node] = true;   // Mark as currently being processed

			// Recursively process all dependent passes first (post-order traversal)
			for (auto dependent : dependents[node]) {
				visit(dependent);
			}

			inStack[node] = false;  // Remove from current path
			visited[node] = true;   // Mark as completely processed
			executionOrder.push_back(node); // Add to execution sequence
		};

		// Process all uncisited nodes to handle disconnected graph components
		for (size_t i = 0; i < passes.size(); ++i) {
			if (!visited[i]) {
				visit(i);
			}
		}

		// Automatic Synchronization Object Creation
		// Generate semaphores for all dependencies identified during analysis
		for (size_t i = 0; i < passes.size(); ++i) {
			for (auto dep : dependencies[i]) {
				// Create a GPU semaphore for this dependency relationship
				// The dependent pass will wait on this semaphore before executing
				semaphores.emplace_back(device.createSemaphore({}));
				semaphoreSignalWaitPairs.emplace_back(dep, i); // (producer, consumer) pair
			}
		}

		// Physical Resource Allocation and Creation
		// Transform resource descriptions into actual GPU objects
		for (auto& [name, resource] : resources) {
			// Configure image creation parameters based on resource description
			vk::ImageCreateInfo imageInfo;
			imageInfo.setImageType(vk::ImageType::e2D)            // 2D texture/render target
				.setFormat(resource.format)                       // Pixel format from description
				.setExtent({resource.extent.width, resource.extent.height, 1 }) // Dimensions
				.setMipLevels(1)                                  // Single mip level for simplicity
				.setArrayLayers(1)                                // Single layer (not aray texture)
				.setSamples(vk::SampleCountFlagBits::e1)          // No multisampling
				.setTiling(vk::ImageTiling::eOptimal)             // GPU-optimal memory layout
				.setUsage(resource.usage)                         // Usage flags from registration
				.setSharingMode(vk::SharingMode::eExclusive)      // Single queue family access
				.setInitialLayout(vk::ImageLayout::eUndefined);   // Initial layout (will be transitioned)

			resource.image = device.createImage(imageInfo);       // Create the GPU image object

			// Allocate backing memory for the image
			vk::MemoryRequirements memRequirements = resource.image.getMemoryRequirements();

			vk::MemoryAllocateInfo allocInfo;
			allocInfo.setAllocationSize(memRequirements.size)     // Required memory size
				.setMemoryTypeIndex(FindMemoryType(memRequirements.memoryTypeBits,
					vk::MemoryPropertyFlagBits::eDeviceLocal));   // GPU-local memory

			resource.memory = device.allocateMemory(allocInfo);   // Allocate GPU memory
			resource.image.bindMemory(*resource.memory, 0);       // Bind memory to image

			// Create image view for shader access
			vk::ImageViewCreateInfo viewInfo;
			viewInfo.setImage(*resource.image)                    // Reference the created image
				.setViewType(vk::ImageViewType::e2D)              // 2D view type
				.setFormat(resource.format)                       // Match image format
				.setSubresourceRange({ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });   // Full image access

			resource.view = device.createImageView(viewInfo);     // Create shader-accessible view
		}
	}
	
	// Resource access interface for retrieving compiled resources
	Resource* GetResource(const std::string& name) {
		auto it = resources.find(name);
		return (it != resources.end()) ? &it->second : nullptr;
	}
};


