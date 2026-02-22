#include "component.h"
#include <vector>

class Event {
public:
	virtual ~Event() = default;
};

// Specific event types
class CollisionEvent : public Event {
private:
	Entity* entity1;
	Entity* entity2;

public:
	CollisionEvent(Entity* e1, Entity* e2) : entity1(e1), entity2(e2) {}

	Entity* GetEntity1() const { return entity1; }
	Entity* GetEntity2() const { return entity2; }
};

// Event listener interface
class EventListener {
public:
	virtual ~EventListener() = default;
	virtual void OnEvent(const Event& event) = 0;
};

// Event system
class EventSystem {
private:
	std::vector<EventListener*> listeners;

public:
	void AddListener(EventListener* listener) {
		listeners.push_back(listener);
	}

	void RemoveListener(EventListener* listener) {
		auto it = std::find(listeners.begin(), listeners.end(), listener);
		if (it != listeners.end()) {
			listeners.erase(it);
		}
	}

	void DispatchEvent(const Event& event) {
		for (auto listener : listeners) {
			listener->OnEvent(event);
		}
	}
};

// Component that listens for events
// Handles physics-related behavior and responds to collision events through the event system
class PhysicsComponent : public Component, public EventListener {
public:
	void Initialize() override {
		// Register as event listener
		GetEventSystem().AddListener(this);
	}

	~PhysicsComponent() override {
		// Unregister as event listener
		GetEventSystem().RemoveListener(this);
	}

	void OnEvent(const Event& event) override {
		if (auto collisionEvent = dynamic_cast<const CollisionEvent*>(&event)) {
			// Handle collision event
		}
	}

private:
	EventSystem& GetEventSystem() {
		// Get event system from somewhere (e.g., service locator)
		static EventSystem eventSystem;
		return eventSystem;
	}
};