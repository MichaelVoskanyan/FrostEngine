#pragma once

#include <Frost/Scene/Components/AnimationTriggerComponent.hpp>
#include <Frost/Scene/Components/AttachmentComponent.hpp>
#include <Frost/Scene/Components/BoxColliderComponent.hpp>
#include <Frost/Scene/Components/CameraComponent.hpp>
#include <Frost/Scene/Components/CapsuleColliderComponent.hpp>
#include <Frost/Scene/Components/CharacterControllerComponent.hpp>
#include <Frost/Scene/Components/FlyControllerComponent.hpp>
#include <Frost/Scene/Components/LightComponent.hpp>
#include <Frost/Scene/Components/MeshComponent.hpp>
#include <Frost/Scene/Components/PhysicsQueryComponent.hpp>
#include <Frost/Scene/Components/RigidBodyComponent.hpp>
#include <Frost/Scene/Components/SkeletalMeshComponent.hpp>
#include <Frost/Scene/Components/TransformComponent.hpp>
#include <Frost/Scene/Registry.hpp>

#include <cstddef>
#include <utility>

namespace Frost {

    // A compile-time list of component types, for code that has to handle each of them in turn:
    //   BuiltinComponents::ForEach([&]<typename T>() { ... T ... });
    template <Component... Ts>
    struct ComponentList {
        static constexpr std::size_t kCount = sizeof...(Ts);

        // Calls `function.template operator()<T>()` for each type, in order.
        template <typename Function>
        static void ForEach(Function&& function) {
            (function.template operator()<Ts>(), ...);
        }
    };

    // The components an object in a level can have. Saved with levels (LevelSerializer) and
    // editable in the editor's Inspector, in this order.
    //
    // To add a component type:
    //   1. Define it (a struct with a `static constexpr const char* kDisplayName`), in its own
    //      header under Frost/Scene/Components/, and include it above.
    //   2. Append it to this list.
    //   3. Specialize ComponentSerializer<T> in LevelSerializer.cpp (how it is saved and loaded;
    //      the build fails until this exists).
    //   4. Give the editor's InspectorPanel a DrawComponent overload for it (likewise required).
    // A system that works on it iterates it with Registry::Each (see PhysicsSystem).
    using BuiltinComponents =
            ComponentList<TransformComponent, AttachmentComponent, MeshComponent, SkeletalMeshComponent,
                          AnimationTriggerComponent, LightComponent, CameraComponent, FlyControllerComponent,
                          CharacterControllerComponent, BoxColliderComponent, CapsuleColliderComponent,
                          RigidBodyComponent, PhysicsQueryComponent>;

}  // namespace Frost
