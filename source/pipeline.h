#include <string>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <functional>
#include <vector>
#include <unordered_map>
#include "component.h"
#include "resource_custom.h"

class CullingSystem
{
private:
	Camera *camera;
	std::vector<Entity *> visibleEntities;

public:
	explicit CullingSystem(Camera *cam) : camera(cam) {}

	void SetCamera(Camera *cam)
	{
		camera = cam;
	}

	void CullScene(const std::vector<Entity *> &allEntities)
	{
		visibleEntities.clear();

		if (!camera)
			return;

		// Get camera frustum
		Frustum rustum = camera->GetFrustum();

		// Check each entity against the frustum
		for (auto entity : allEntities)
		{
			if (!entity->IsActive())
				continue;

			auto meshComponent = entity->GetComponent<MeshComponent>();
			if (!meshComponent)
				continue;

			auto transformComponent = entity->GetComponent<TransformComponent>();
			if (!transformComponent)
				continue;

			// Get bounding box of the mesh
			BoundingBox boundingBox = meshComponent->GetBoundingBox();

			// Transform bounding box by entity transform
			boundingBox.Transform(transformComponent->GetTransformMatrix());

			// Check if bounding box is visible
			if (frustum.Intersects(boundingBox))
			{
				visibleEntities.push_back(entity);
			}
		}
	}

	const std::vector<Entity *> &GetVisibleEntities() const
	{
		return visibleEntities;
	}
};

// A comprehensive rendergraph implementation for automated dependency management
class Rendergraph
{
private:
	// Resource description and management structure
	// Represents Image resource used during rendering (textures)
	struct ImageResource
	{
		std::string name;			   // Human-readable identifier for debugging and referencing
		vk::Format format;			   // Pixel format (RGBA8, Depth24Stencil8, etc.)
		vk::Extent2D extent;		   // Dimensions in pixels for 2D resources
		vk::ImageUsageFlags usage;	   // How this resource will be used (color attachment, texture, etc.)
		vk::ImageLayout initialLayout; // Expected layout when the frame begins
		vk::ImageLayout finalLayout;   // Required layout when the frame end

		// Actual GPU resources - populated during compilation
		vk::raii::Image image = nullptr;		 // The GPU image object
		vk::raii::DeviceMemory memory = nullptr; // Backing memory allocation
		vk::raii::ImageView view = nullptr;		 // Shader-accessible view of the image
	};

	// Render pass representation within the graph structure
	// Each pass represents a distict rendering operation with defined inputs and outputs
	struct Pass
	{
		std::string name;											// Descriptive name for debugging and profiling
		std::vector<std::string> inputs;							// Resource this pass reads from (dependencies)
		std::vector<std::string> outputs;							// Resources this pass writes to (products)
		std::function<void(vk::raii::CommandBuffer &)> executeFunc; // The actual rendering code
	};

	// Core data storage for the rendergraph system
	std::unordered_map<std::string, Resource> resources; // All resources referenced in the graph
	std::vector<Pass> passes;							 // All rendering passes in definition order
	std::vector<size_t> executionOrder;					 // Computed optimal execution sequence

	// Automatic synchronization management
	// These objects ensure correct GPU execution order without manual barriers
	std::vector<vk::raii : Semaphore> semaphores;					 // GPU synchronization primitives
	std::vector<std::pair<size_t, size_t>> semaphoreSignalWaitPairs; // (signaling pass, waiting pass)

	vk::raii : Device &device; // Vulkan device for resource creation

public:
	explicit Rendergraph(vk::raii::Device &dev) : device(dev) {}

	// Resource registration interface for declaring all resources used during rendering
	// This method establishes resource metadata without creating actual GPU resources
	void AddResource(const std::string &name, vk::Format format, vk::Extent2D extent,
					 vk::ImageUsageFlags usage, vk::ImageLayout initialLayout,
					 vk::ImageLayout finalLayout)
	{

		Resource resource;
		resource.name = name;					// Store human-readable identifier
		resource.format = format;				// Define pixel format and bit depth
		resource.extent = extent;				// Set resource dimensions
		resource.usage = usage;					// Specify intended usage patterns
		resource.initialLayout = initialLayout; // Define starting layout state
		resource.finalLayout = finalLayout;		// Define required ending state

		resources[name] = resource; // Register in the global resource map
	}

	// Pass registration interface for defining rendering operations and their dependencies
	// This method establishes the logical structure of rendering without immediate execution
	void AddPass(const std::string &name,
				 const std::vector<std::string> &inputs,
				 const std::vector<std::string> &outputs,
				 std::function<void(vk::raii::CommandBuffer &)> executeFunc)
	{

		Pass pass;
		pass.name = name;				// Assign descriptive identifier
		pass.inputs = inputs;			// List all resources this pass reads
		pass.outputs = outputs;			// List all resources this pass writes
		pass.executeFunc = executeFunc; // Store the actual rendering implementation

		passes.push_back(pass); // Add to the ordered pass list
	}

	// Rendergraph compilation - transforms declarative descriptions into executable pipeline
	// This method performs dependency analysis, resource allocation, and execution planning
	void Compile()
	{
		// Dependency Graph Construction
		// Build bidirectional dependency relationships between passes
		std::vector<std::vector<size_t>> dependencies(passes.size()); // What each pass depends on
		std::vector<std::vector<size_t>> dependents(passes.size());	  // What depends on each pass

		// Track which pass produces each resource (write-after-write dependencies)
		std::unordered_map<std::string, size_t> resourceWriters;

		// Dependency Discovery Through Resource Usage Analysis
		// Analyze each pass to determine data flow relationships
		for (size_t i = 0; i < passes.size(); ++i)
		{
			const auto &pass = passes[i];

			// Process input dependencies - this pass must wait for producers
			for (const auto &input : pass.inputs)
			{
				auto it = resourceWriters.find(input);
				if (it != resourceWriters.end())
				{
					// Found the pass that produces this input - create dependency link
					dependencies[i].push_back(it->second); // This pass depends on the producer
					dependents[it->second].push_back(i);   // Producer has this as dependent
				}
			}

			// Register output production - subsequent passes may depend on these
			for (const auto &output : pass.outputs)
			{
				resourceWriters[output] = i; // Record this pass as producer
			}
		}

		// Topological Sort for Optimal Execution Order
		// Use depth-first search to compute valid execution sequence while detecting cycles
		std::vector<bool> visited(passes.size(), false); // Track completed nodes
		std::vector<bool> inStack(passes.size(), false); // Track current recursion path

		std::function<void(size_t)> visit = [&](size_t node)
		{
			if (inStack[node])
			{
				// Cycle detection - circular dependency found
				throw std::runtime_error("Cycle detected in rendergraph");
			}

			if (visited[node])
			{
				return; // Already processed this node and its dependencies
			}

			inStack[node] = true; // Mark as currently being processed

			// Recursively process all dependent passes first (post-order traversal)
			for (auto dependent : dependents[node])
			{
				visit(dependent);
			}

			inStack[node] = false;			// Remove from current path
			visited[node] = true;			// Mark as completely processed
			executionOrder.push_back(node); // Add to execution sequence
		};

		// Process all uncisited nodes to handle disconnected graph components
		for (size_t i = 0; i < passes.size(); ++i)
		{
			if (!visited[i])
			{
				visit(i);
			}
		}

		// Automatic Synchronization Object Creation
		// Generate semaphores for all dependencies identified during analysis
		for (size_t i = 0; i < passes.size(); ++i)
		{
			for (auto dep : dependencies[i])
			{
				// Create a GPU semaphore for this dependency relationship
				// The dependent pass will wait on this semaphore before executing
				semaphores.emplace_back(device.createSemaphore({}));
				semaphoreSignalWaitPairs.emplace_back(dep, i); // (producer, consumer) pair
			}
		}

		// Physical Resource Allocation and Creation
		// Transform resource descriptions into actual GPU objects
		for (auto &[name, resource] : resources)
		{
			// Configure image creation parameters based on resource description
			vk::ImageCreateInfo imageInfo;
			imageInfo.setImageType(vk::ImageType::e2D)						   // 2D texture/render target
				.setFormat(resource.format)									   // Pixel format from description
				.setExtent({resource.extent.width, resource.extent.height, 1}) // Dimensions
				.setMipLevels(1)											   // Single mip level for simplicity
				.setArrayLayers(1)											   // Single layer (not aray texture)
				.setSamples(vk::SampleCountFlagBits::e1)					   // No multisampling
				.setTiling(vk::ImageTiling::eOptimal)						   // GPU-optimal memory layout
				.setUsage(resource.usage)									   // Usage flags from registration
				.setSharingMode(vk::SharingMode::eExclusive)				   // Single queue family access
				.setInitialLayout(vk::ImageLayout::eUndefined);				   // Initial layout (will be transitioned)

			resource.image = device.createImage(imageInfo); // Create the GPU image object

			// Allocate backing memory for the image
			vk::MemoryRequirements memRequirements = resource.image.getMemoryRequirements();

			vk::MemoryAllocateInfo allocInfo;
			allocInfo.setAllocationSize(memRequirements.size) // Required memory size
				.setMemoryTypeIndex(FindMemoryType(memRequirements.memoryTypeBits,
												   vk::MemoryPropertyFlagBits::eDeviceLocal)); // GPU-local memory

			resource.memory = device.allocateMemory(allocInfo); // Allocate GPU memory
			resource.image.bindMemory(*resource.memory, 0);		// Bind memory to image

			// Create image view for shader access
			vk::ImageViewCreateInfo viewInfo;
			viewInfo.setImage(*resource.image)										 // Reference the created image
				.setViewType(vk::ImageViewType::e2D)								 // 2D view type
				.setFormat(resource.format)											 // Match image format
				.setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}); // Full image access

			resource.view = device.createImageView(viewInfo); // Create shader-accessible view
		}
	}

	// Resource access interface for retrieving compiled resources
	Resource *GetResource(const std::string &name)
	{
		auto it = resources.find(name);
		return (it != resources.end()) ? &it->second : nullptr;
	}

	// Rendergraph execution engine - coordinates pass execution with automatic synchronization
	// This method transforms the compiled rendergraph into actual GPU work
	void Execute(vk::raii::CommandBuffer &commandBuffer, vk::Queue queue)
	{
		// Execution state management for dynamic synchronization
		std::vector<vk::CommandBuffer> cmdBuffers;		// Command buffer storage
		std::vector<vk::Semaphore> waitSemaphores;		// Synchronization dependencies for current pass
		std::vector<vk::PipelineStageFlags> waitStages; // Pipeline stages to wait on
		std::vector<vk::Semaphore> signalSemaphores;	// Semaphores to signal after current pass

		// Ordered Pass Execution with Automatic Dependency Management
		// Execute each pass in the computed dependency-safe order
		for (auto passIdx : executionOrder)
		{
			const auto &pass = passes[passIdx];

			// Synchronization Setup - Collect Dependencies for Current Pass
			// Determine what this pass must wait for before executing
			waitSemaphores.clear();
			waitStages.clear();

			for (size_t i = 0; i < semaphoreSignalWaitPairs.size(); ++i)
			{
				if (semaphoreSignalWaitPairs[i].second == passIdx)
				{
					// This pass depends on the completion of another pass
					waitSemaphores.push_back(*semaphores[i]);								 // wait for dependency completion
					waitStages.push_back(vk::PipelineStageFlagBits::eColorAttachmentOutput); // Wait at output stage
				}
			}

			// Collect semaphores that this pass will signal for dependent passes
			signalSemaphores.clear();
			for (size_t i = 0; i < semaphoreSignalWaitPairs.size(); ++i)
			{
				if (semaphoreSignalWaitPairs[i].first == passIdx)
				{
					// Other passes depend on this pass's completion
					signalSemaphores.push_back(*semaphores[i]); // Signal completion for dependents
				}
			}

			// Command Buffer Preparation and Resource Layout Transisitons
			// Set up command recording and transition resources to appropriate layouts
			commandBuffer.begin({}); // Begin command recording

			// Transition input resources to shader-readable layouts
			for (const auto &input : pass.inputs)
			{
				auto &resource = resources[input];

				vk::ImageMemoryBarrier barrier;
				barrier.setOldLayout(resource.initialLayout)			   // Current resource layout
					.setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal) // Taget layout for reading
					.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)	   // No queue family transfer
					.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
					.setImage(*resource.image)
					.setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}) // Full image range
					.setSrcAccessMask(vk::AccessFlagBits::eMemoryWrite)					// Previous write access
					.setDstAccessMask(vk::AccessFlagBits::eShaderRead);					// Required read access

				// Insert pipeline barrier for safe layout transition
				commandBuffer.pipelineBarrier(
					vk::PipelineStageFlagBits::eAllCommands,	// Wait for all previous work
					vk::PipelineStageFlagBits::eFragmentShader, // Enable frament shader access
					vk::DependencyFlagBits::eByRegion,			// Region-local dependency
					0, nullptr, 0, nullptr, 1, &barrier			// Image barrier only
				);
			}

			// Transition output resources to render target layouts
			for (const auto &output : pass.outputs)
			{
				auto &resource = resources[output];

				vk::ImageMemoryBarrier barrier;
				barrier.setOldLayout(resource.initialLayout)				// Current layout state
					.setNewLayout(vk::ImageLayout::eColorAttachmentOptimal) // Optimal for color output
					.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
					.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
					.setImage(*resource.image)
					.setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1})
					.setSrcAccessMask(vk::AccessFlagBits::eMemoryRead)			  // Previous read access
					.setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite); // Required write access

				// Insert barrier for safe transition to writable state
				commandBufer.pipelineBarrier(
					vk::PipelineStageFlagBits::eAllCommands,
					vk::PipelineStageFlagBits::eColorAttachmentOutput, // Enable color attachment writes
					vk::DependencyFlagBits::eByRegion,
					0, nullptr, 0, nullptr, 1, &barrier);
			}

			// Pass Execition - Execute the Actual Rendering Logic
			// Call the user-provided rendering function with prepared command buffer
			pass.executeFunc(commandBuffer); // Execute pass-specific rendering

			// Final Layout Transitions - Prepare Resources for Subsequent Use
			// Transition output resources to their final required layouts
			for (const auto &output : pass.outputs)
			{
				auto &resource = resources[output];

				vk::ImageMemoryBarrier barrier;
				barrier.setOldLayout(vk::ImageLayout::eColorAttachmentOptimal) // Current writable layout
					.setNewLayout(resource.finalLayout)
					.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
					.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
					.setImage(*resource.image)
					.setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1})
					.setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite) // Previous write operations
					.setDstAccessMask(vk::AccessFlagBits::eMemoryRead);			 // Enable subsequent reads

				// Insert final barrier for layout transition
				commandBuffer.pipelineBarrier(
					vk::PipelineStageFlagBits::eColorAttachmentOutput, // After color writes complete
					vk::PipelineStageFlagBits::eAllCommands,		   // Before any subsequent work
					vk::DependencyFlagBits::eByRegion,
					0, nullptr, 0, nullptr, 1, &barrier);
			}

			// Command Submission with Synchronization
			// Submit command buffer with appropriate dependency and signaling semaphores
			commandBuffer.end(); // Finalize command recording

			vk::SubmitInfo submitInfo;
			submitInfo.setWaitSemaphoreCount(static_cast<uint32_t>(waitSemaphores.size())) // Dependencies to wait for
				.setPWaitSemaphores(waitSemaphores.data())								   // Dependency semaphores
				.setPWaitDstStageMask(waitStages.data())								   // Pipeline stages to wait at
				.setCommandBufferCount(1)												   // Single command buffer
				.setPCommandBuffers(&*commandBuffer)									   // Command buffer to execute
				.setSignalSemaphoreCount(static_cast<uint32_t>(signalSemaphores.size()))   // Semaphores to signal
				.setPSignalSemaphores(signalSemaphores.data());							   // Signal semaphores

			queue.sugmit(1, &submitInfo, nullptr); // submit to GPU queue
		}
	}

private:
	uint32_t FindMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties)
	{
		// Implementation to find suitable memory type
		// ...
		return 0; // Placeholder
	}

	// Comprehensive image layout transisiton implementation
	// This function demonstrates proper synchronization and layout management in Vulkan
	void TransitionImageLayout(vk::raii::CommandBuffer &commandBuffer,
							   vk::Image image,
							   vk::Format format,
							   vk::ImageLayout oldLayout,
							   vk::ImageLayout newLayout)
	{
		// Configure the basic image memory berrier structure
		// This barrier will coordinate memory access and layout transitions
		vk::ImageMemoryBarrier barrier;
		barrier.setOldLayout(oldLayout)											 // Current image layout state
			.setNewLayout(newLayout)											 // Target layout after transition
			.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)					 // No queue family ownership transfer
			.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)					 // Same queue family throughout
			.setImage(image)													 // Target image for the transition
			.setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}); // Full color image range

		// Initialize pipeline stage traching for synchronization timing
		// These stages define when operations must complete and when new operations can begin
		vk::PipelineStageFlags sourceStage;		// When previous operations must finish
		vk::PipelineStageFlags destinationStage // When subsequent operations can start

			// Configure synchronization for undefined-to-transfer layout transitions
			// This pattern is common when preparing images for data uploads
			if (oldLayout == vk::ImageLayout::eUndefined &&
				newLayout == vk::ImageLayout::eTransferDstOptimal)
		{

			// Configure memory access permissions for upload preparation
			barrier.setSrcAccessMask(vk::AccessFlagBits::eNone)		   // No previous access to synchronize
				.setDstAccessMask(vk::AccessFlagBits::eTransferWrite); // Enable transfer write operations

			// Set pipeline stage synchronization points for upload workflow
			sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;		   // Transfer operations must complete
			destinationStage = vk::PipelineStageFlagBits::eFragmentShader; // Fragment shaders can access
		}
		else
		{
			// Handle unsupported transition combinations
			// Production code would include additional common transition patterns
			throw std::invalid_argument("Unsupported layout transition!");
		}

		// Execute the pipeline barrier with configured synchronization
		// This commits the layout transition and memory synchronization to the command buffer
		commandBuffer.pipelineBarrier(
			sourceStage,					   // Wait for these operations to complete
			destinationStage,				   // Before allowing these operations to begin
			vk::DependencyFlagBits::eByRegion, // Enable region-local optimizations
			0, nullptr,						   // No memory barriers needed
			0, nullptr,						   // No buffer barriers needed
			1, &barrier						   // Apply out image memory barrier
		);
	}

	// Comprehensive frame rendering with proper synchronization
	// This function demonstrates the complete cycle of frame rendering coordination
	void RenderFrame(vk::raii::Device &device, vk::Queue graphicsQueue, vk::Queue presentQueue)
	{
		// Synchronize with previous frame completion
		// Prevent CPU from submitting work faster than GPU can process it
		vk::Result result = device.waitForFences(1, &*inFlightFence, VK_TRUE, UINT64_MAX);

		// Reset fence for this frame's completion tracking
		// Prepare the fence to signal when this frame's GPU work completes
		device.resetFences(1, &*inFlightFence);

		// Acquire next available image from the swapchain
		// This operation coordinates with the presentation engine and display system
		uint32_t imageIndex;
		result = device.acquireNextImageKHR(*swapchain,				  // Target swapchain for acquisition
											UINT64_MAX,				  // Wait indefinitely for image availability
											*imageAvailableSemaphore, // Semaphore signaled when image is available
											nullptr,				  // No fence needed for this operation
											&imageIndex);			  // Receives index of acquired image

		// Record command buffer for this frame's rendering
		// Command buffer recording happens here with acquired image as render target
		// ... (command recording implementation would go here)

		// Configure GPU work submission with comprehensive synchronization
		// This submission coordinates image availability, rendering and presentation readiness
		vk::SubmitInfo submitInfo;
		vk::PipelineStageFlags waitStages[] = {vk::PipelineStageFlagBits::eColorAttachmentOutput};

		submitInfo.setWaitSemaphoreCount(1)					  // Wait for one semaphore before execution
			.setPWaitSemaphores(&*imageAvailableSemaphore)	  // Don't start until image is available
			.setPWaitDstStageMask(waitStages)				  // Specifically wait before color output
			.setCommandBufferCount(1)						  // Submit one command buffer
			.setPCommandBuffers(&*commandBuffer)			  // The recorded rendering commands
			.setSignalSemaphoreCount(1)						  // Signal one semaphore when complete
			.setPSignalSemaphores(&*renderFinishedSemaphore); // Notify when rendering is finished

		// Submit work to GPU with fence-based completion tracking
		// The fence allows CPU to know when this frame's GPU work has completed
		graphicsQueue.submit(1, &submitInfo, *inFlightFence);

		// Present the rendered image to the display
		// This operation transfers the completed frame from rendering to display system
		vk::PresentInfoKHR presentInfo;
		presentInfo.setWaitSemaphoreCount(1)			   // Wait for rendering completion
			.setPWaitSemaphores(&*renderFinishedSemaphore) // Don't present until rendering finishes
			.setSwapchainCount(1)						   // Present to one swapchain
			.setPSwapchains(&*swapchain)				   // Target swapchain for presentation
			.setPImageIndices(&imageIndex);				   // Present the image we rendered to

		// Submit presentation request to the presentation engine
		result = presentQueue.presentKHR(&presentInfo);
	}
};