#version 450

// Source-equivalent translation of the installed ksFXAA pixel algorithm.
// The backend supplies a linear-repeat sampler as a labeled approximation of
// the recovered anisotropic-repeat runtime state.

layout(set = 0, binding = 0) uniform sampler2D source_texture;

layout(push_constant) uniform FxaaParameters {
    vec2 inverse_dimensions;
    uint srgb_destination;
} parameters;

layout(location = 0) in vec2 texture_coordinates;
layout(location = 0) out vec4 output_color;

float luma(vec3 color) {
    return color.r + color.g * 1.96321070;
}

float decode_srgb_component(float encoded) {
    return encoded <= 0.04045
               ? encoded / 12.92
               : pow((encoded + 0.055) / 1.055, 2.4);
}

vec3 destination_color(vec3 encoded) {
    if (parameters.srgb_destination == 0U) {
        return encoded;
    }
    return vec3(decode_srgb_component(encoded.r),
                decode_srgb_component(encoded.g),
                decode_srgb_component(encoded.b));
}

#define SAMPLE_OFFSET(x, y) \
    textureLodOffset(source_texture, texture_coordinates, 0.0, ivec2(x, y))

void main() {
    vec4 north = SAMPLE_OFFSET(0, -1);
    vec4 west = SAMPLE_OFFSET(-1, 0);
    vec4 center = SAMPLE_OFFSET(0, 0);
    vec4 east = SAMPLE_OFFSET(1, 0);
    vec4 south = SAMPLE_OFFSET(0, 1);

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
    float threshold = max(maximum_luma * 0.125, 0.0416666679);
    if (luma_range < threshold) {
        output_color = vec4(destination_color(center.rgb), 1.0);
        return;
    }

    vec3 neighborhood_sum = north.rgb + west.rgb + center.rgb + east.rgb +
                            south.rgb;
    float subpixel = abs((luma_north + luma_west + luma_east + luma_south) *
                         0.25 - luma_center) / luma_range;
    subpixel = min(max(subpixel - 0.25, 0.0) * 1.33333337, 0.75);

    vec4 northwest = SAMPLE_OFFSET(-1, -1);
    vec4 northeast = SAMPLE_OFFSET(1, -1);
    vec4 southwest = SAMPLE_OFFSET(-1, 1);
    vec4 southeast = SAMPLE_OFFSET(1, 1);
    neighborhood_sum += northwest.rgb + northeast.rgb + southwest.rgb +
                        southeast.rgb;

    float luma_northwest = luma(northwest.rgb);
    float luma_northeast = luma(northeast.rgb);
    float luma_southwest = luma(southwest.rgb);
    float luma_southeast = luma(southeast.rgb);
    float vertical_edge =
        abs(luma_northwest * 0.25 + luma_northeast * 0.25 -
            luma_north * 0.5) +
        abs(luma_west * 0.5 + luma_east * 0.5 - luma_center) +
        abs(luma_southwest * 0.25 + luma_southeast * 0.25 -
            luma_south * 0.5);
    float horizontal_edge =
        abs(luma_northwest * 0.25 + luma_southwest * 0.25 -
            luma_west * 0.5) +
        abs(luma_north * 0.5 + luma_south * 0.5 - luma_center) +
        abs(luma_northeast * 0.25 + luma_southeast * 0.25 -
            luma_east * 0.5);
    bool horizontal = horizontal_edge >= vertical_edge;

    float step_length = horizontal ? -parameters.inverse_dimensions.y
                                   : -parameters.inverse_dimensions.x;
    float first_luma = horizontal ? luma_north : luma_west;
    float second_luma = horizontal ? luma_south : luma_east;
    float first_gradient = first_luma - luma_center;
    float second_gradient = second_luma - luma_center;
    float first_average = (first_luma + luma_center) * 0.5;
    float second_average = (second_luma + luma_center) * 0.5;
    bool first_is_steepest = abs(first_gradient) >= abs(second_gradient);
    float edge_luma = first_is_steepest ? first_average : second_average;
    float gradient = max(abs(first_gradient), abs(second_gradient));
    if (!first_is_steepest) {
        step_length = -step_length;
    }

    vec2 edge_position = texture_coordinates;
    if (horizontal) {
        edge_position.y += step_length * 0.5;
    } else {
        edge_position.x += step_length * 0.5;
    }
    vec2 search_step = horizontal
        ? vec2(parameters.inverse_dimensions.x, 0.0)
        : vec2(0.0, parameters.inverse_dimensions.y);
    vec2 negative_position = edge_position - search_step;
    vec2 positive_position = edge_position + search_step;
    float gradient_threshold = gradient * 0.25;
    float negative_luma = edge_luma;
    float positive_luma = edge_luma;
    bool negative_done = false;
    bool positive_done = false;
    for (int index = 0; index < 32; ++index) {
        if (!negative_done) {
            negative_luma = luma(textureLod(source_texture,
                                            negative_position, 0.0).rgb);
            negative_done = abs(negative_luma - edge_luma) >=
                            gradient_threshold;
        }
        if (!positive_done) {
            positive_luma = luma(textureLod(source_texture,
                                            positive_position, 0.0).rgb);
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
        ? texture_coordinates.x - negative_position.x
        : texture_coordinates.y - negative_position.y;
    float positive_distance = horizontal
        ? positive_position.x - texture_coordinates.x
        : positive_position.y - texture_coordinates.y;
    bool negative_is_nearest = negative_distance < positive_distance;
    float nearest_luma = negative_is_nearest ? negative_luma : positive_luma;
    bool center_is_lower = (luma_center - edge_luma) < 0.0;
    bool nearest_is_lower = (nearest_luma - edge_luma) < 0.0;
    float edge_offset = 0.0;
    if (center_is_lower != nearest_is_lower) {
        float total_distance = negative_distance + positive_distance;
        float nearest_distance = min(negative_distance, positive_distance);
        edge_offset = step_length * (0.5 - nearest_distance / total_distance);
    }

    vec2 filtered_coordinates = texture_coordinates;
    if (horizontal) {
        filtered_coordinates.y += edge_offset;
    } else {
        filtered_coordinates.x += edge_offset;
    }
    vec3 edge_color = textureLod(source_texture, filtered_coordinates, 0.0).rgb;
    vec3 filtered = mix(edge_color, neighborhood_sum * (1.0 / 9.0), subpixel);
    output_color = vec4(destination_color(filtered), 1.0);
}
