#pragma once

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Core.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
// clang-format on

namespace ox::SceneEvents {
struct OnContactAddedEvent {
  const JPH::Body& body1;
  const JPH::Body& body2;
  const JPH::ContactManifold& manifold;
  const JPH::ContactSettings& settings;
  OnContactAddedEvent(const JPH::Body& body1_,
                      const JPH::Body& body2_,
                      const JPH::ContactManifold& manifold_,
                      const JPH::ContactSettings& settings_)
      : body1(body1_),
        body2(body2_),
        manifold(manifold_),
        settings(settings_) {}
};

struct OnContactPersistedEvent {
  const JPH::Body& body1;
  const JPH::Body& body2;
  const JPH::ContactManifold& manifold;
  const JPH::ContactSettings& settings;
  OnContactPersistedEvent(const JPH::Body& body1_,
                          const JPH::Body& body2_,
                          const JPH::ContactManifold& manifold_,
                          const JPH::ContactSettings& settings_)
      : body1(body1_),
        body2(body2_),
        manifold(manifold_),
        settings(settings_) {}
};

struct OnContactRemovedEvent {
  const JPH::SubShapeIDPair& sub_shape_pair;
  OnContactRemovedEvent(const JPH::SubShapeIDPair& sub_shape_pair_) : sub_shape_pair(sub_shape_pair_) {}
};

struct OnBodyActivatedEvent {
  const JPH::BodyID& body_id;
  JPH::uint64 body_user_data;
  OnBodyActivatedEvent(const JPH::BodyID& body_id_, JPH::uint64 body_user_data_)
      : body_id(body_id_),
        body_user_data(body_user_data_) {}
};

struct OnBodyDeactivatedEvent {
  const JPH::BodyID& body_id;
  JPH::uint64 body_user_data;
  OnBodyDeactivatedEvent(const JPH::BodyID& body_id_, JPH::uint64 body_user_data_)
      : body_id(body_id_),
        body_user_data(body_user_data_) {}
};
} // namespace ox::SceneEvents
