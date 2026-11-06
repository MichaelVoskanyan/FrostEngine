#pragma once

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Frost {

    // An entity is nothing but an id; what it is made of is the set of components attached to it.
    // In a level, entities are the level's objects and use their (stable, saved) ObjectIds.
    using EntityId = std::uint64_t;

    // Anything can be a component as long as it is a plain (non-const, non-reference) class type
    // that can be moved around: components are stored by value, packed together per type.
    template <typename T>
    concept Component = std::is_class_v<T> && std::same_as<T, std::remove_cvref_t<T>> && std::movable<T>;

    namespace Detail {

        // A small, dense number per component type, handed out the first time a type is used; it
        // indexes the registry's pool table.
        [[nodiscard]]
        inline std::size_t NextComponentTypeIndex() noexcept {
            static std::atomic<std::size_t> next{0};
            return next.fetch_add(1, std::memory_order_relaxed);
        }

        template <Component T>
        [[nodiscard]]
        std::size_t ComponentTypeIndex() noexcept {
            static const std::size_t index = NextComponentTypeIndex();
            return index;
        }

    }  // namespace Detail

    // The type-independent part of a ComponentPool, so the registry can act on an entity's
    // components without knowing their types (removing all of them, copying all of them).
    class ComponentPoolBase {
    public:
        virtual ~ComponentPoolBase() = default;

        [[nodiscard]]
        virtual bool Contains(EntityId entity) const noexcept = 0;

        // Does nothing if the entity has no such component.
        virtual void Remove(EntityId entity) = 0;

        // Gives `to` a copy of `from`'s component (replacing one it already has). Does nothing if
        // `from` has none, or if the component type cannot be copied.
        virtual void Copy(EntityId from, EntityId to) = 0;

        [[nodiscard]]
        virtual std::size_t Size() const noexcept = 0;

        // The entities that have this component, in storage order.
        [[nodiscard]]
        virtual std::span<const EntityId> GetEntities() const noexcept = 0;

    protected:
        ComponentPoolBase() = default;
        ComponentPoolBase(const ComponentPoolBase&) = default;
        ComponentPoolBase& operator=(const ComponentPoolBase&) = default;
    };

    // Every component of one type, packed contiguously (so a system walking them touches memory in
    // order), with a lookup from entity to slot. Removal moves the last component into the freed
    // slot, so storage never has holes.
    //
    // References and pointers to components stay valid until a component of the same type is added
    // or removed (either can move the storage); look components up again after that.
    template <Component T>
    class ComponentPool final : public ComponentPoolBase {
    public:
        // Constructs the entity's component from `args` (replacing one it already has).
        template <typename... Args>
        T& Emplace(const EntityId entity, Args&&... args) {
            if (const auto found = m_Indices.find(entity); found != m_Indices.end()) {
                T& existing = m_Components[found->second];
                existing = Construct(std::forward<Args>(args)...);
                return existing;
            }

            m_Components.push_back(Construct(std::forward<Args>(args)...));
            m_Entities.push_back(entity);
            m_Indices.emplace(entity, m_Components.size() - 1);
            return m_Components.back();
        }

        [[nodiscard]]
        T* TryGet(const EntityId entity) noexcept {
            const auto found = m_Indices.find(entity);
            return found != m_Indices.end() ? &m_Components[found->second] : nullptr;
        }

        [[nodiscard]]
        const T* TryGet(const EntityId entity) const noexcept {
            const auto found = m_Indices.find(entity);
            return found != m_Indices.end() ? &m_Components[found->second] : nullptr;
        }

        [[nodiscard]]
        bool Contains(const EntityId entity) const noexcept override {
            return m_Indices.contains(entity);
        }

        void Remove(const EntityId entity) override {
            const auto found = m_Indices.find(entity);
            if (found == m_Indices.end()) { return; }

            const std::size_t index = found->second;
            const std::size_t last = m_Components.size() - 1;
            m_Indices.erase(found);

            if (index != last) {
                m_Components[index] = std::move(m_Components[last]);
                m_Entities[index] = m_Entities[last];
                m_Indices[m_Entities[index]] = index;
            }

            m_Components.pop_back();
            m_Entities.pop_back();
        }

        void Copy(const EntityId from, const EntityId to) override {
            if constexpr (std::is_copy_constructible_v<T>) {
                const T* const source = TryGet(from);
                if (source == nullptr || from == to) { return; }

                // Copied out first: adding to the pool may reallocate the storage `source` is in.
                T copy(*source);
                Emplace(to, std::move(copy));
            }
        }

        [[nodiscard]]
        std::size_t Size() const noexcept override {
            return m_Components.size();
        }

        [[nodiscard]]
        std::span<const EntityId> GetEntities() const noexcept override {
            return m_Entities;
        }

        // The components themselves, in the same order as GetEntities().
        [[nodiscard]]
        std::span<T> GetComponents() noexcept {
            return m_Components;
        }

        [[nodiscard]]
        std::span<const T> GetComponents() const noexcept {
            return m_Components;
        }

    private:
        // Parentheses when T has a matching constructor (including copy/move), braces otherwise,
        // so aggregates can be built from their members too.
        template <typename... Args>
        [[nodiscard]]
        static T Construct(Args&&... args) {
            if constexpr (std::is_constructible_v<T, Args...>) { return T(std::forward<Args>(args)...); }
            else { return T{std::forward<Args>(args)...}; }
        }

        std::vector<T> m_Components;
        std::vector<EntityId> m_Entities;  // m_Entities[i] owns m_Components[i].
        std::unordered_map<EntityId, std::size_t> m_Indices;
    };

    // The component storage of an entity component system: which components each entity has, and
    // their data. Any Component type can be attached without registering it first; its pool is
    // created the first time it is used.
    //
    // Systems work on the entities that have a given set of components with Each<A, B, ...>(), e.g.
    //   registry.Each<TransformComponent, RigidBodyComponent>(
    //           [](EntityId entity, TransformComponent& transform, RigidBodyComponent& body) { ... });
    //
    // Entity ids are chosen by the owner (the Level); the registry only records components.
    class Registry final {
    public:
        Registry() = default;

        Registry(const Registry&) = delete;
        Registry& operator=(const Registry&) = delete;

        Registry(Registry&&) = default;
        Registry& operator=(Registry&&) = default;

        // Attaches a component built from `args` (replacing one of the same type the entity already
        // has) and returns it. See ComponentPool for how long the reference stays valid.
        template <Component T, typename... Args>
        T& Add(const EntityId entity, Args&&... args) {
            return GetOrCreatePool<T>().Emplace(entity, std::forward<Args>(args)...);
        }

        // Does nothing if the entity has no such component.
        template <Component T>
        void Remove(const EntityId entity) {
            if (ComponentPool<T>* const pool = FindPool<T>(); pool != nullptr) { pool->Remove(entity); }
        }

        template <Component T>
        [[nodiscard]]
        bool Has(const EntityId entity) const noexcept {
            const ComponentPool<T>* const pool = FindPool<T>();
            return pool != nullptr && pool->Contains(entity);
        }

        // The entity's component, or null if it has none.
        template <Component T>
        [[nodiscard]]
        T* TryGet(const EntityId entity) noexcept {
            ComponentPool<T>* const pool = FindPool<T>();
            return pool != nullptr ? pool->TryGet(entity) : nullptr;
        }

        template <Component T>
        [[nodiscard]]
        const T* TryGet(const EntityId entity) const noexcept {
            const ComponentPool<T>* const pool = FindPool<T>();
            return pool != nullptr ? pool->TryGet(entity) : nullptr;
        }

        // The entity's component; throws std::out_of_range if it has none.
        template <Component T>
        [[nodiscard]]
        T& Get(const EntityId entity) {
            T* const component = TryGet<T>(entity);
            if (component == nullptr) { throw std::out_of_range("Registry::Get: the entity has no such component."); }
            return *component;
        }

        template <Component T>
        [[nodiscard]]
        const T& Get(const EntityId entity) const {
            const T* const component = TryGet<T>(entity);
            if (component == nullptr) { throw std::out_of_range("Registry::Get: the entity has no such component."); }
            return *component;
        }

        // Detaches every component the entity has (of any type).
        void RemoveAll(const EntityId entity) {
            for (const std::unique_ptr<ComponentPoolBase>& pool : m_Pools) {
                if (pool != nullptr) { pool->Remove(entity); }
            }
        }

        // Gives `to` a copy of each (copyable) component `from` has, replacing any of the same type.
        void CopyAll(const EntityId from, const EntityId to) {
            for (const std::unique_ptr<ComponentPoolBase>& pool : m_Pools) {
                if (pool != nullptr) { pool->Copy(from, to); }
            }
        }

        // The pool of one component type, or null if no component of that type was ever added.
        template <Component T>
        [[nodiscard]]
        ComponentPool<T>* FindPool() noexcept {
            const std::size_t index = Detail::ComponentTypeIndex<T>();
            return index < m_Pools.size() ? static_cast<ComponentPool<T>*>(m_Pools[index].get()) : nullptr;
        }

        template <Component T>
        [[nodiscard]]
        const ComponentPool<T>* FindPool() const noexcept {
            const std::size_t index = Detail::ComponentTypeIndex<T>();
            return index < m_Pools.size() ? static_cast<const ComponentPool<T>*>(m_Pools[index].get()) : nullptr;
        }

        // Calls `function(entity, a, b, ...)` for every entity that has all of the components
        // A, B, ... (walking whichever of their pools is smallest).
        //
        // `function` may modify the components it is given, but must not add or remove components
        // of the types being iterated (that would move them while they are being walked).
        template <Component... Ts, typename Function>
            requires(sizeof...(Ts) > 0) && std::invocable<Function&, EntityId, Ts&...>
        void Each(Function&& function) {
            EachImpl<Registry, Ts...>(*this, function);
        }

        template <Component... Ts, typename Function>
            requires(sizeof...(Ts) > 0) && std::invocable<Function&, EntityId, const Ts&...>
        void Each(Function&& function) const {
            EachImpl<const Registry, Ts...>(*this, function);
        }

    private:
        template <Component T>
        ComponentPool<T>& GetOrCreatePool() {
            const std::size_t index = Detail::ComponentTypeIndex<T>();
            if (index >= m_Pools.size()) { m_Pools.resize(index + 1); }
            if (m_Pools[index] == nullptr) { m_Pools[index] = std::make_unique<ComponentPool<T>>(); }
            return static_cast<ComponentPool<T>&>(*m_Pools[index]);
        }

        template <typename Self, Component... Ts, typename Function>
        static void EachImpl(Self& self, Function& function) {
            const std::array<const ComponentPoolBase*, sizeof...(Ts)> pools{self.template FindPool<Ts>()...};

            const ComponentPoolBase* smallest = nullptr;
            for (const ComponentPoolBase* const pool : pools) {
                if (pool == nullptr) { return; }  // No entity has this component at all.
                if (smallest == nullptr || pool->Size() < smallest->Size()) { smallest = pool; }
            }

            for (const EntityId entity : smallest->GetEntities()) {
                const auto components = std::make_tuple(self.template TryGet<Ts>(entity)...);
                const bool hasAll = std::apply([](const auto*... component) { return ((component != nullptr) && ...); },
                                               components);
                if (!hasAll) { continue; }

                std::apply([&](auto*... component) { function(entity, *component...); }, components);
            }
        }

        // Indexed by Detail::ComponentTypeIndex; null where this registry never saw the type.
        std::vector<std::unique_ptr<ComponentPoolBase>> m_Pools;
    };

}  // namespace Frost
