#include <string>
#include <unordered_map>
#include <typeindex>

// Create resource manager
ResourceManager resourceManager;

// **** EXAMPLE USAGE OF RESOURCE MANAGER ****
// // Load resources
// auto texture = resourceManager.Load<Texture>("brick");
// auto mesh = resourceManager.Load<Mesh>("cube");
// auto vertexShader = resourceManager.Load<Shader>("basic", vk::ShaderStageFlagBits::eVertex);
// auto fragmentShader = resourceManager.Load<Shader>("basic", vk::ShaderStageFlagBits::eFragment);
// 
// // Use resources
// if (texture && mesh && vertexShader && fragmentShader) {
// 	// Create material using shaders
// 	Material material(vertexShader, fragmentShader);
// 
// 	// Set texture in material
// 	material.SetTexture("diffuse", texture);
// 
// 	// Create entity with mesh and material
// 	Entity entity("MyEntity");
// 	auto meshComponent = entity.AddComponent<MeshComponent>(mesh.Get(), &material);
// }
// 
// // Resources will be automatically released when handles go out of scope
// // or you can explicitly release them
// resourceManager.Release(texture.GetId());
//
// TODO: implement async resource manager as shown in 04_resource_management
// TODO: implement resource streaming for hot reloading 04_resource_management



class ResourceManager;
// Resource handle
template<typename T>
class ResourceHandle {
private:
	std::stringbuf resourceId;
	ResourceManager* resourceManager;

public:
	ResourceHandle() : resourceManager(nullptr) {}

	ResourceHandle(const std::string& id, ResourceManager* manager)
		: resourceId(id), resourceManager(manager) {}

	T* Get() const {
		if (!resourceManager) return nullptr;
		return resourceManager->GetResource<T>(resourceId);
	}

	bool IsValid() const {
		return resourceManager && resourceManager->HasResource<T>(resourceId);
	}

	const std::string& GetId() const {
		return resourceId;
	}

	// Convenience operators
	T* operator->() const {
		return Get();
	}

	T& operator*() const {
		return *Get();
	}

	operator bool() const {
		return IsValid();
	}
};

// Resource base class
class Resource {
private:
	std::string resourceId;
	bool loaded = false;

public:
	explicit Resource(const std::string& id) : resourceId(id) {}
	virtual ~Resource() = default;

	// Core resource identity and state access methods
	const std::string& GetId() const { return resourceId; }
	bool IsLoaded() const { return loaded; }

	// Virtual interface for resource-specific loading and unloading behavior
	bool Load() {
		loaded = doLoad();
		return loaded;
	}

	void Unload() {
		doUnload();
		loaded = false;
	}

protected:
	virtual bool doLoad() = 0;
	virtual void doUnload() = 0;
};

// Resource manager
class ResourceManager {
private:
	// Two-level storage system: organize by type first, then by unique identifier
	// This approach enables type-safe resource access while maintaining efficient lookup
	std::unordered_map<std::type_index,
		std::unordered_map<std::string, std::shared_ptr<Resource>>> resources;

	// Two-level reference counting system for automatic resource lifecycle management
	// First level maps resource type, second level maps resource IDs to their data
	struct ResourceData {
		std::shared_ptr<Resource> resource; // The actual resource
		int refCount;                       // Reference count for this resource
	};
	std::unordered_map<std::type_index,
		std::unordered_map<std::string, ResourceData>> refCounts;

public:
	template<typename T>
	ResourceHandle<T> Load(const std::string& resourceId) {
		static_assert(std::is_base_of<Resource, T>::value, "T must derive from Resource");

	// Step 3a: Check existing resource cache to avoid redundatn loading
		auto& typeResources = resources[std::type_index(typeid(T))];
		auto it = typeResources.find(resourceId);

		if (it != typeResources.end()) {
			// Resource exists in cache - increment reference count and return handle
			refCounts[std::type_index(typeid(T))][resourceId].refCount++;
			return ResourceHandle<T>(resourceId, this);
		}

		// Step 3b: Create new resource instance and attempt loading
		auto resource = std::make_shared<T>(resourceId);
		if (!resource->Load()) {
			// Loading failed - return invalid handle rather than corrupting cache
			return ResourceHandle<T>();
		}

		// Step 3c: Cache successful resource and initialize reference tracking
		typeResources[resourceId] = resource;
		refCounts[resourceId] = 1;

		return ResourceHandle<T>(resourceId, this);
	}

	template<typename T>
	T* GetResource(const std::string& resourceId) {
		// Access type-specific resource container using compile-time type information
		auto& typeResources = resources[std::type_index(typeid(T))];
		auto it = typeResources.find(resourceId);

		if (it != typeResources.end()) {
			// Resource found - perform safe downcast and return typed pointer
			return static_cast<T*>(it->second.get());
		}

		// Resource not found - return null for safe handling by caller
		return nullptr;
	}

	template<typename T>
	bool HasResource(const std::string& resourceId) {
		// Efficient existence check without resource access overhead
		auto resourceIt = resources.find(std::type_index(typeid(T)));
		return resourceIt != resources.end();
	}

	template<typename T>
	void Release(const std::string& resourceId) {
		// Locate reference count entry for this resource
		auto& typeRefCounts = refCounts[std::type_index(typeid(T))];
		auto it = typeRefCounts.find(resourceId);
		if (it != typeRefCounts.end()) {
			it->second.refCount--;

			// Check if resource has no remaining references
			if (it->second <= 0) {
				// Step 5a: Locate and unload the unreferenced resource across all type containers
				for (auto& [type, typeResources] : resources) {
					auto resourceIt = typeResources.find(resourceId);
					if (resourceIt != typeResources.end()) {
						resourceIt->second->Unload();
						typeResources.erase(resourceIt);
						break;
					}
				}

				// Step 5b: Clean up reference counting entry
				typeRefCounts.erase(it);
			}
		}
	}

	void UnloadAll() {
		// Emergency cleanup method for system shutdown or major state changes
		for (auto& [type, typeResources] : resources) {
			for (auto& [id, resource] : typeResources) {
				resource->Unload(); // Ensure all resources clean up properly
			}
			typeResources.clear();  // Clear type-specific containers
		}
		refCounts.clear();          // Reset all reference counts
	}
};

