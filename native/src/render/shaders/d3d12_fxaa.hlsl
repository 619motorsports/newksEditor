// Source-equivalent translation of the installed ksFXAA pixel algorithm.
// The static linear-repeat sampler is a labeled approximation of the
// recovered anisotropic-repeat runtime state.

Texture2D<float4> source_texture : register(t0);
SamplerState linear_repeat : register(s0);

cbuffer FxaaParameters : register(b0) {
    float2 inverse_dimensions;
    uint srgb_destination;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 texture_coordinates : TEXCOORD0;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    float2 position = float2((vertex_id << 1U) & 2U, vertex_id & 2U);
    output.texture_coordinates = position;
    output.position = float4(position * float2(2.0F, -2.0F) +
                                 float2(-1.0F, 1.0F),
                             0.0F, 1.0F);
    return output;
}

float luma(float3 color) {
    return color.r + color.g * 1.96321070F;
}

float decode_srgb_component(float encoded) {
    return encoded <= 0.04045F
               ? encoded / 12.92F
               : pow((encoded + 0.055F) / 1.055F, 2.4F);
}

float3 destination_color(float3 encoded) {
    if (srgb_destination == 0U) {
        return encoded;
    }
    return float3(decode_srgb_component(encoded.r),
                  decode_srgb_component(encoded.g),
                  decode_srgb_component(encoded.b));
}

float4 pixel_main(VertexOutput input) : SV_Target0 {
    const float2 uv = input.texture_coordinates;
    float4 north = source_texture.SampleLevel(linear_repeat, uv, 0.0F,
                                               int2(0, -1));
    float4 west = source_texture.SampleLevel(linear_repeat, uv, 0.0F,
                                              int2(-1, 0));
    float4 center = source_texture.SampleLevel(linear_repeat, uv, 0.0F,
                                                int2(0, 0));
    float4 east = source_texture.SampleLevel(linear_repeat, uv, 0.0F,
                                              int2(1, 0));
    float4 south = source_texture.SampleLevel(linear_repeat, uv, 0.0F,
                                               int2(0, 1));

    float luma_north = luma(north.rgb);
    float luma_west = luma(west.rgb);
    float luma_center = luma(center.rgb);
    float luma_east = luma(east.rgb);
    float luma_south = luma(south.rgb);
    float minimum_luma = min(luma_center,
        min(min(luma_north, luma_west), min(luma_east, luma_south)));
    float maximum_luma = max(luma_center,
        max(max(luma_north, luma_west), max(luma_east, luma_south)));
    float luma_range = maximum_luma - minimum_luma;
    float threshold = max(maximum_luma * 0.125F, 0.0416666679F);
    if (luma_range < threshold) {
        return float4(destination_color(center.rgb), 1.0F);
    }

    float3 neighborhood_sum = north.rgb + west.rgb + center.rgb + east.rgb +
                              south.rgb;
    float subpixel = abs((luma_north + luma_west + luma_east + luma_south) *
                         0.25F - luma_center) / luma_range;
    subpixel = min(max(subpixel - 0.25F, 0.0F) * 1.33333337F, 0.75F);

    float4 northwest = source_texture.SampleLevel(linear_repeat, uv, 0.0F,
                                                   int2(-1, -1));
    float4 northeast = source_texture.SampleLevel(linear_repeat, uv, 0.0F,
                                                   int2(1, -1));
    float4 southwest = source_texture.SampleLevel(linear_repeat, uv, 0.0F,
                                                   int2(-1, 1));
    float4 southeast = source_texture.SampleLevel(linear_repeat, uv, 0.0F,
                                                   int2(1, 1));
    neighborhood_sum += northwest.rgb + northeast.rgb + southwest.rgb +
                        southeast.rgb;

    float luma_northwest = luma(northwest.rgb);
    float luma_northeast = luma(northeast.rgb);
    float luma_southwest = luma(southwest.rgb);
    float luma_southeast = luma(southeast.rgb);
    float vertical_edge =
        abs(luma_northwest * 0.25F + luma_northeast * 0.25F -
            luma_north * 0.5F) +
        abs(luma_west * 0.5F + luma_east * 0.5F - luma_center) +
        abs(luma_southwest * 0.25F + luma_southeast * 0.25F -
            luma_south * 0.5F);
    float horizontal_edge =
        abs(luma_northwest * 0.25F + luma_southwest * 0.25F -
            luma_west * 0.5F) +
        abs(luma_north * 0.5F + luma_south * 0.5F - luma_center) +
        abs(luma_northeast * 0.25F + luma_southeast * 0.25F -
            luma_east * 0.5F);
    bool horizontal = horizontal_edge >= vertical_edge;

    float step_length = horizontal ? -inverse_dimensions.y
                                   : -inverse_dimensions.x;
    float first_luma = horizontal ? luma_north : luma_west;
    float second_luma = horizontal ? luma_south : luma_east;
    float first_gradient = first_luma - luma_center;
    float second_gradient = second_luma - luma_center;
    float first_average = (first_luma + luma_center) * 0.5F;
    float second_average = (second_luma + luma_center) * 0.5F;
    bool first_is_steepest = abs(first_gradient) >= abs(second_gradient);
    float edge_luma = first_is_steepest ? first_average : second_average;
    float gradient = max(abs(first_gradient), abs(second_gradient));
    if (!first_is_steepest) {
        step_length = -step_length;
    }

    float2 edge_position = uv;
    if (horizontal) {
        edge_position.y += step_length * 0.5F;
    } else {
        edge_position.x += step_length * 0.5F;
    }
    float2 search_step = horizontal
        ? float2(inverse_dimensions.x, 0.0F)
        : float2(0.0F, inverse_dimensions.y);
    float2 negative_position = edge_position - search_step;
    float2 positive_position = edge_position + search_step;
    float gradient_threshold = gradient * 0.25F;
    float negative_luma = edge_luma;
    float positive_luma = edge_luma;
    bool negative_done = false;
    bool positive_done = false;
    [loop]
    for (int index = 0; index < 32; ++index) {
        if (!negative_done) {
            negative_luma = luma(source_texture.SampleLevel(
                linear_repeat, negative_position, 0.0F).rgb);
            negative_done = abs(negative_luma - edge_luma) >=
                            gradient_threshold;
        }
        if (!positive_done) {
            positive_luma = luma(source_texture.SampleLevel(
                linear_repeat, positive_position, 0.0F).rgb);
            positive_done = abs(positive_luma - edge_luma) >=
                            gradient_threshold;
        }
        if (negative_done && positive_done) {
            break;
        }
        if (!negative_done) {
            negative_position -= search_step;
        }
        if (!positive_done) {
            positive_position += search_step;
        }
    }

    float negative_distance = horizontal
        ? uv.x - negative_position.x
        : uv.y - negative_position.y;
    float positive_distance = horizontal
        ? positive_position.x - uv.x
        : positive_position.y - uv.y;
    bool negative_is_nearest = negative_distance < positive_distance;
    float nearest_luma = negative_is_nearest ? negative_luma : positive_luma;
    bool center_is_lower = (luma_center - edge_luma) < 0.0F;
    bool nearest_is_lower = (nearest_luma - edge_luma) < 0.0F;
    float edge_offset = 0.0F;
    if (center_is_lower != nearest_is_lower) {
        float total_distance = negative_distance + positive_distance;
        float nearest_distance = min(negative_distance, positive_distance);
        edge_offset = step_length * (0.5F - nearest_distance / total_distance);
    }

    float2 filtered_coordinates = uv;
    if (horizontal) {
        filtered_coordinates.y += edge_offset;
    } else {
        filtered_coordinates.x += edge_offset;
    }
    float3 edge_color = source_texture.SampleLevel(
        linear_repeat, filtered_coordinates, 0.0F).rgb;
    float3 filtered = lerp(edge_color, neighborhood_sum * (1.0F / 9.0F),
                           subpixel);
    return float4(destination_color(filtered), 1.0F);
}
