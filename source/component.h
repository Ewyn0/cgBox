#include <memory>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "resource.h"
#include <glm/gtc/quaternion.hpp>

class Entity;

// Component type ID system
class ComponentTypeIDSystem {
private:
	static size_t nextTypeID;

public:
	template<typename T>
	static size_t GetTypeID() {
		static size_t typeID = nextTypeID++;
		return typeID;
	}
};

size_t ComponentTypeIDSystem::nextTypeID = 0;

class Component {
public:
	enum class State {
		Uninitialized,
		Initializing,
		Active,
		Destroying,
		Destroyed
	};
protected:
	Entity* owner = nullptr;


private:
	State state = State::Uninitialized;
	
public:
	virtual ~Component() {
		if (state != State::Destroyed) {
			OnDestroy();
			state = State::Destroyed;
		}
	}

	virtual void Initialize() {
		if (state == State::Uninitialized) {
			state = State::Initializing;
			OnInitialize();
			state = State::Active;
		}
	}

	void Destroy() {
		if (state == State::Active) {
			state = State::Destroying;
			OnDestroy();
			state = State::Destroyed;
		}
	}

	bool IsActive() const { return state == State::Active; }


	void SetOwner(Entity* entity) { owner = entity; }
	Entity* GetOwner() const { return owner; }

	template<typename T>
	static size_t GetTypeID() {
		return ComponentTypeIDSystem::GetTypeID<T>();
	}

protected:
	virtual void OnInitialize() {}
	virtual void OnDestroy() {}
	virtual void Update(float deltaTime) {}
	virtual void Render() {}

	friend class Entity; // Allow Entity to call protected methods
};

class Entity {
private:
	std::string name;
	bool active = true;
	std::vector<std::unique_ptr<Component>> components;
	std::unordered_map<size_t, Component*> componentMap;

public:
	explicit Entity(const std::string& entityName) : name(entityName) {}

	const std::string& GetName() const { return name; }
	bool IsActive() const { return active; }
	void SetActive(bool isActive) { active = isActive; }

	void Initialize() {
		for (auto& component : components) {
			component->Initialize();
		}
	}

	void Update(float deltaTime) {
		if (!active) return;

		for (auto& component : components) {
			component->Update(deltaTime);
		}
	}

	void Render() {
		if (!active) return;

		for (auto& component : components) {
			component->Render();
		}
	}

	template<typename T, typename... Args>
	T* AddComponent(Args&&... args) {
		static_assert(std::is_base_of<Component, T>::value, "T must derive from Component");

		size_t typeID = Component::GetTypeID<T>();

		// Check if component of this type already exists
		auto it = componentMap.find(typeID);
		if (it != componentMap.end()) {
			return static_cast<T*>(it->second);
		}

		// Create new component
		auto component = std::make_unique<T>(std::forward<Args>(args)...);
		T* componentPtr = component.get();
		componentMap[typeID] = componentPtr;
		components.push_back(std::move(component));
		return componentPtr;
	}

	template<typename T>
	T* GetComponent() {
		size_t typeID = Component::GetTypeID<T>();
		auto it = componentMap.find(typeID);
		if (it != componentMap.end()) {
			return static_cast<T*>(it->second);
		}
		return nullptr;
	}

	template<typename T>
	bool RemoveComponent() {
		size_t typeID = Component::GetTypeID<T>();
		auto it = componentMap.find(typeID);
		if (it != componentMap.end()) {
			Component* componentPtr = it->second;
			componentMap.erase(it);

			for (auto compIt = components.begin(); compIt != components.end(); ++compIt) {
				if (compIt->get() == componentPtr) {
					components.erase(compIt);
					return true;
				}
			}
		}
		return false;
	}
};

class TransformComponent : public Component {
private:
	glm::vec3 position = glm::vec3(0.0f);
	glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	glm::vec3 scale = glm::vec3(1.0f);

	// Cached transformation matrix
	mutable glm::mat4 transformMatrix = glm::mat4(1.0f);
	mutable bool transformDirty = true;

public:
	void SetPosition(const glm::vec3& pos) {
		position = pos;
		transformDirty = true;
	}

	void SetRotation(const glm::quat& rot) {
		rotation = rot;
		transformDirty = true;
	}

	void SetScale(const glm::vec3& s) {
		scale = s;
		transformDirty = true;
	}

	const glm::vec3& GetPosition() const { return position; }
	const glm::quat& GetRotation() const { return rotation; }
	const glm::vec3& GetScale() const { return scale; }

	glm::mat4 GetTransformMatrix() const {
		if (transformDirty) {
			// Calculate transformation matrix
			glm::mat4 translationMatrix = glm::translate(glm::mat4(1.0f), position);
			glm::mat4 rotationMatrix = glm::mat4_cast(rotation);
			glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.0f), scale);

			transformMatrix = translationMatrix * rotationMatrix * scaleMatrix;
			transformDirty = false;
		}
		return transformMatrix;
	}
};

// Mesh component
// Manages the visual represenation of an entity by handling its 3D mesh and material
class MeshComponent : public Component {
private:
	Mesh* mesh = nullptr;
	Material* material = nullptr;

public:
	MeshComponent(Mesh* m, Material* mat) : mesh(m), material(mat) {}

	void SetMesh(Mesh* m) { mesh = m; }
	void SetMaterial(Material* mat) { material = mat; }

	Mesh* GetMesh() const { return mesh; }
	Material* GetMaterial() const { return material; }

	void Render() override {
		if (!mesh || !material) return;

		// Get transform component
		auto transform = GetOwner()->GetComponent<TransformComponent>();
		if (!transform) return;

		// Render mesh with material and transform
		material->Bind();
		material->SetuUniform("modelMatrix", transform->GetTransformMatrix());
		mesh->Render();
	}
};

// Camera component
// Defines a viewpoint for rendering the scene by managing view and projection matrices
class CameraComponent : public Component {
private:
	float fieldOfView = 45.0f;
	float aspectRatio = 16.0f / 9.0f;
	float nearPlane = 0.1f;
	float farPlane = 1000.0f;

	glm::mat4 viewMatrix = glm::mat4(1.0f);
	mutable glm::mat4 projectionMatrix = glm::mat4(1.0f);
	mutable bool projectionDirty = true;

public:
	void SetPerspective(float fov, float aspect, float nearP, float farP) {
		fieldOfView = fov;
		aspectRatio = aspect;
		nearPlane = nearP;
		farPlane = farP;
		projectionDirty = true;
	}

	glm::mat4 GetViewMatrix() const {
		// Get transform component
		auto transform = GetOwner()->GetComponent<TransformComponent>();
		if (transform) {
			// Calculate view matrix from transform
			glm::vec3 position = transform->GetPosition();
			glm::quat rotation = transform->GetRotation();

			// Forward vector (local -Z)
			glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
			// Up vector (local +Y)
			glm::vec3 up = rotation * glm::vec3(0.0f, 1.0f, 0.0f);

			return glm::lookAt(position, position + forward, up);
		}
		return glm::mat4(1.0f);
	}

	glm::mat4 GetProjectionMatrix() const {
		if (projectionDirty) {
			projectionMatrix = glm::perspective(
				glm::radians(fieldOfView),
				aspectRatio,
				nearPlane,
				farPlane
			);
			projectionDirty = false;
		}
		return projectionMatrix;
	}
};
