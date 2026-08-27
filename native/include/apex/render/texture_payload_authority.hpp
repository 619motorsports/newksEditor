#pragma once

#include <cstdint>

namespace apex::render {

enum class TexturePayloadAuthority : std::uint8_t {
    // The frame supplies non-owning tables in final model texture order.
    caller_tables,
    // Preparation decodes payloads copied into the model and owns the created
    // texture resources plus one linear/repeat sampler.
    owned_model_payloads,
    // Compatibility spelling retained for existing KN5 callers.
    embedded_kn5 = owned_model_payloads,
};

// Keep the public static-scene spelling while callers migrate to the shared
// model-payload term.
using StaticSceneTextureAuthority = TexturePayloadAuthority;

}  // namespace apex::render
