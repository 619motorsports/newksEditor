#include "apex/formats/fbx.hpp"

#include "apex/core/deflate.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace apex::formats {
namespace {

constexpr std::string_view kBinaryMagic = "Kaydara FBX Binary  ";

[[noreturn]] void fail(FbxStage stage, std::string_view code, std::string_view message, std::size_t offset) {
    throw FbxError({stage, std::string(code), std::string(message), offset});
}

std::size_t addSize(std::size_t left, std::size_t right, std::size_t offset, FbxStage stage) {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        fail(stage, "size_overflow", "FBX size arithmetic overflow", offset);
    return left + right;
}

std::size_t mulSize(std::size_t left, std::size_t right, std::size_t offset, FbxStage stage) {
    if (left != 0u && right > std::numeric_limits<std::size_t>::max() / left)
        fail(stage, "size_overflow", "FBX size arithmetic overflow", offset);
    return left * right;
}

std::uint32_t little32(std::span<const std::uint8_t> bytes, std::size_t offset, FbxStage stage) {
    if (offset > bytes.size() || bytes.size() - offset < 4u)
        fail(stage, "truncated", "truncated FBX 32-bit field", offset);
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

std::uint64_t little64(std::span<const std::uint8_t> bytes, std::size_t offset, FbxStage stage) {
    if (offset > bytes.size() || bytes.size() - offset < 8u)
        fail(stage, "truncated", "truncated FBX 64-bit field", offset);
    std::uint64_t result = 0;
    for (unsigned index = 0; index < 8u; ++index)
        result |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8u);
    return result;
}

std::uint16_t little16(std::span<const std::uint8_t> bytes, std::size_t offset, FbxStage stage) {
    if (offset > bytes.size() || bytes.size() - offset < 2u)
        fail(stage, "truncated", "truncated FBX 16-bit field", offset);
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1u] << 8u);
}

std::int16_t signed16(std::uint16_t bits) { return std::bit_cast<std::int16_t>(bits); }
std::int32_t signed32(std::uint32_t bits) { return std::bit_cast<std::int32_t>(bits); }
std::int64_t signed64(std::uint64_t bits) { return std::bit_cast<std::int64_t>(bits); }

float float32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    const auto bits = little32(bytes, offset, FbxStage::binary_dom);
    float result = 0;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

double float64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    const auto bits = little64(bytes, offset, FbxStage::binary_dom);
    double result = 0;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

void checkInput(std::span<const std::uint8_t> bytes, const FbxLimits& limits, std::string_view source) {
    if (bytes.size() > limits.parse.maxInputBytes)
        fail(FbxStage::header, "input_limit", "FBX input exceeds the configured input limit", 0);
    if (source.size() > limits.parse.maxStringBytes)
        fail(FbxStage::header, "source_limit", "FBX source name exceeds the configured limit", 0);
}

bool allZero(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t count) {
    if (offset > bytes.size() || count > bytes.size() - offset) return false;
    return std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + count),
                       [](std::uint8_t value) { return value == 0u; });
}

std::size_t binaryNullSize(std::uint32_t version) { return version >= 7500u ? 25u : 13u; }

std::size_t binaryHeaderSize(std::uint32_t version) { return version >= 7500u ? 25u : 13u; }

std::size_t binaryElementSize(char code) {
    switch (code) {
    case 'Y': return 2u;
    case 'C': return 1u;
    case 'I': case 'F': return 4u;
    case 'D': case 'L': return 8u;
    default: return 0u;
    }
}

class BinaryReader final {
public:
    BinaryReader(std::span<const std::uint8_t> bytes, std::uint32_t version,
                 const FbxLimits& limits) : bytes_(bytes), version_(version), limits_(limits) {}
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    void need(std::size_t count, std::string_view what) const {
        if (count > bytes_.size() - offset_) fail(FbxStage::binary_dom, "truncated", what, offset_);
    }
    std::uint32_t u32() { const auto value = little32(bytes_, offset_, FbxStage::binary_dom); offset_ += 4u; return value; }
    std::uint64_t u64() { const auto value = little64(bytes_, offset_, FbxStage::binary_dom); offset_ += 8u; return value; }
    std::string bytesString(std::size_t count, std::string_view what) {
        if (count > limits_.parse.maxStringBytes) fail(FbxStage::binary_dom, "string_limit", what, offset_);
        need(count, "truncated FBX string");
        std::string result(reinterpret_cast<const char*>(bytes_.data()+offset_), count); offset_ += count; return result;
    }
    std::span<const std::uint8_t> bytesSpan(std::size_t count, std::string_view what) {
        need(count, what); const auto result = bytes_.subspan(offset_, count); offset_ += count; return result;
    }
    [[nodiscard]] std::span<const std::uint8_t> all() const noexcept { return bytes_; }
    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
    [[nodiscard]] const FbxLimits& limits() const noexcept { return limits_; }
private:
    std::span<const std::uint8_t> bytes_;
    std::uint32_t version_ = 0;
    const FbxLimits& limits_;
    std::size_t offset_ = 0;
};

FbxValue binaryProperty(BinaryReader& reader, char code, std::size_t propertyOffset,
                        std::size_t& arrayElements, std::size_t& arrayBytes,
                        std::size_t& rawProperties, std::size_t& rawBytes,
                        std::size_t& aggregateBytes) {
    const auto account = [&](std::size_t bytes) {
        aggregateBytes = addSize(aggregateBytes, bytes, propertyOffset, FbxStage::binary_dom);
        if (aggregateBytes > reader.limits().maxAggregateBytes)
            fail(FbxStage::binary_dom, "allocation_limit", "aggregate FBX allocation budget exceeds its limit", propertyOffset);
    };
    const auto scalar = binaryElementSize(code);
    if (scalar != 0u) {
        reader.need(scalar, "truncated FBX scalar property");
        if (code == 'Y') { const auto value = signed16(little16(reader.all(), reader.offset(), FbxStage::binary_dom)); reader.bytesSpan(2u, "scalar"); return static_cast<std::int64_t>(value); }
        if (code == 'C') { const auto value = reader.all()[reader.offset()] != 0u; reader.bytesSpan(1u, "scalar"); return value; }
        if (code == 'I') { const auto value = signed32(reader.u32()); return static_cast<std::int64_t>(value); }
        if (code == 'F') { const auto value = float32(reader.all(), reader.offset()); if (!std::isfinite(value)) fail(FbxStage::binary_dom, "non_finite", "non-finite FBX scalar", propertyOffset); reader.bytesSpan(4u, "scalar"); return static_cast<double>(value); }
        if (code == 'D') {
            const auto value = float64(reader.all(), reader.offset());
            if (!std::isfinite(value)) fail(FbxStage::binary_dom, "non_finite", "non-finite FBX scalar", propertyOffset);
            reader.bytesSpan(8u, "scalar");
            return value;
        }
        return static_cast<std::int64_t>(signed64(reader.u64()));
    }
    if (code == 'S' || code == 'R') {
        const auto length = reader.u32();
        if (length > reader.limits().parse.maxStringBytes && code == 'S') fail(FbxStage::binary_dom, "string_limit", "FBX property string exceeds its limit", propertyOffset);
        if (length > reader.limits().maxPropertyBytes) fail(FbxStage::binary_dom, "property_limit", "FBX property exceeds its limit", propertyOffset);
        if (code == 'R') {
            if (rawProperties >= reader.limits().maxRawProperties)
                fail(FbxStage::binary_dom, "raw_property_limit",
                     "FBX raw property count exceeds its limit", propertyOffset);
            if (length > reader.limits().maxRawPropertyBytes)
                fail(FbxStage::binary_dom, "raw_property_limit",
                     "FBX raw property exceeds its byte limit", propertyOffset);
            const auto nextRawBytes = addSize(
                rawBytes, static_cast<std::size_t>(length), propertyOffset,
                FbxStage::binary_dom);
            if (nextRawBytes > reader.limits().maxRawBytes)
                fail(FbxStage::binary_dom, "raw_property_limit",
                     "aggregate FBX raw property bytes exceed their limit",
                     propertyOffset);
            ++rawProperties;
            rawBytes = nextRawBytes;
        }
        account(length);
        const auto value = reader.bytesSpan(length, "truncated FBX property bytes");
        if (code == 'S') { account(sizeof(std::string)); return std::string(reinterpret_cast<const char*>(value.data()), value.size()); }
        account(sizeof(std::vector<std::uint8_t>));
        return std::vector<std::uint8_t>(value.begin(), value.end());
    }
    if (code != 'f' && code != 'd' && code != 'l' && code != 'i' && code != 'b')
        fail(FbxStage::binary_dom, "unsupported_property", "unsupported FBX binary property type", propertyOffset);
    const auto count = static_cast<std::size_t>(reader.u32());
    const auto encoding = reader.u32(); const auto compressedLength = static_cast<std::size_t>(reader.u32());
    if (count > reader.limits().maxArrayElements) fail(FbxStage::binary_dom, "array_limit", "FBX array element count exceeds its limit", propertyOffset);
    const auto elementSize = code == 'd' || code == 'l' ? 8u : (code == 'b' ? 1u : 4u);
    const auto rawLength = mulSize(count, elementSize, propertyOffset, FbxStage::binary_dom);
    if (rawLength > reader.limits().maxArrayBytes || compressedLength > reader.limits().maxArrayBytes)
        fail(FbxStage::binary_dom, "array_limit", "FBX array byte count exceeds its limit", propertyOffset);
    const auto valueBytes = mulSize(count, (code == 'l' || code == 'd') ? 8u : (code == 'f' ? 4u : (code == 'i' ? 8u : 1u)), propertyOffset, FbxStage::binary_dom);
    account(rawLength);
    account(valueBytes);
    account(sizeof(FbxArray));
    const auto compressed = reader.bytesSpan(compressedLength, "truncated FBX array payload");
    std::vector<std::uint8_t> raw;
    if (encoding == 0u) {
        if (compressedLength != rawLength) fail(FbxStage::binary_dom, "array_size", "uncompressed FBX array size does not match its count", propertyOffset);
        raw.assign(compressed.begin(), compressed.end());
    } else if (encoding == 1u) {
        apex::core::ParseLimits deflateLimits = reader.limits().parse;
        deflateLimits.maxInputBytes = std::min(
            deflateLimits.maxInputBytes, reader.limits().maxArrayBytes);
        deflateLimits.maxOutputBytes = std::min(
            deflateLimits.maxOutputBytes, reader.limits().maxArrayBytes);
        try {
            raw = apex::core::inflateZlib(compressed, rawLength, "FBX array",
                                          deflateLimits, propertyOffset);
        }
        catch (const std::runtime_error& error) { fail(FbxStage::binary_dom, "compression", error.what(), propertyOffset); }
    } else fail(FbxStage::binary_dom, "compression", "unsupported FBX array encoding", propertyOffset);
    arrayElements = addSize(arrayElements, count, propertyOffset, FbxStage::binary_dom);
    arrayBytes = addSize(arrayBytes, rawLength, propertyOffset, FbxStage::binary_dom);
    if (arrayElements > reader.limits().maxArrayElements || arrayBytes > reader.limits().maxArrayBytes)
        fail(FbxStage::binary_dom, "array_limit", "aggregate FBX array budget exceeds its limit", propertyOffset);
    if (code == 'b') return FbxArray{std::vector<std::uint8_t>(raw.begin(), raw.end())};
    if (code == 'l') { std::vector<std::int64_t> values(count); for (std::size_t i=0;i<count;++i) values[i]=signed64(little64(raw,i*8u,FbxStage::binary_dom)); return FbxArray{std::move(values)}; }
    if (code == 'd') { std::vector<double> values(count); for (std::size_t i=0;i<count;++i) { values[i]=float64(raw,i*8u); if (!std::isfinite(values[i])) fail(FbxStage::binary_dom, "non_finite", "non-finite FBX array value", propertyOffset); } return FbxArray{std::move(values)}; }
    if (code == 'f') { std::vector<float> values(count); for (std::size_t i=0;i<count;++i) { values[i]=float32(raw,i*4u); if (!std::isfinite(values[i])) fail(FbxStage::binary_dom, "non_finite", "non-finite FBX array value", propertyOffset); } return FbxArray{std::move(values)}; }
    std::vector<std::int64_t> values(count); for (std::size_t i=0;i<count;++i) values[i]=static_cast<std::int64_t>(signed32(little32(raw,i*4u,FbxStage::binary_dom))); return FbxArray{std::move(values)};
}

struct BinaryState {
    std::size_t nodes = 0;
    std::size_t properties = 0;
    std::size_t propertyBytes = 0;
    std::size_t arrayElements = 0;
    std::size_t arrayBytes = 0;
    std::size_t rawProperties = 0;
    std::size_t rawBytes = 0;
    std::size_t aggregateBytes = 0;
};

FbxNode binaryNode(BinaryReader& reader, std::size_t depth, BinaryState& state) {
    if (depth >= reader.limits().maxDepth) fail(FbxStage::binary_dom, "depth_limit", "FBX node nesting exceeds its limit", reader.offset());
    ++state.nodes; if (state.nodes > reader.limits().maxNodes) fail(FbxStage::binary_dom, "node_limit", "FBX node count exceeds its limit", reader.offset());
    const auto start = reader.offset(); const auto endOffset = reader.version() >= 7500u ? reader.u64() : reader.u32();
    const auto propertyCount = reader.version() >= 7500u ? reader.u64() : reader.u32();
    const auto propertyLength = reader.version() >= 7500u ? reader.u64() : reader.u32();
    reader.need(1u, "truncated FBX node name length"); const auto nameLength = reader.all()[reader.offset()]; reader.bytesSpan(1u, "node name length");
    const auto name = reader.bytesString(nameLength, "FBX node name exceeds its limit");
    state.aggregateBytes = addSize(state.aggregateBytes, name.size(), start, FbxStage::binary_dom);
    if (endOffset < reader.offset() || endOffset > reader.all().size()) fail(FbxStage::binary_dom, "offset", "FBX node end offset is invalid", start);
    if (propertyCount > reader.limits().maxProperties || propertyLength > reader.limits().maxPropertyBytes)
        fail(FbxStage::binary_dom, "property_limit", "FBX node property budget exceeds its limit", start);
    const auto propertyEnd = addSize(reader.offset(), static_cast<std::size_t>(propertyLength), start, FbxStage::binary_dom);
    if (propertyEnd > endOffset) fail(FbxStage::binary_dom, "offset", "FBX property list exceeds node end offset", start);
    state.propertyBytes = addSize(state.propertyBytes, static_cast<std::size_t>(propertyLength), start, FbxStage::binary_dom);
    if (state.propertyBytes > reader.limits().maxPropertyBytes)
        fail(FbxStage::binary_dom, "property_limit", "aggregate FBX property budget exceeds its limit", start);
    state.aggregateBytes = addSize(state.aggregateBytes, static_cast<std::size_t>(propertyLength), start, FbxStage::binary_dom);
    if (state.aggregateBytes > reader.limits().maxAggregateBytes)
        fail(FbxStage::binary_dom, "allocation_limit", "aggregate FBX allocation budget exceeds its limit", start);
    state.aggregateBytes = addSize(state.aggregateBytes, sizeof(FbxNode), start, FbxStage::binary_dom);
    if (state.aggregateBytes > reader.limits().maxAggregateBytes)
        fail(FbxStage::binary_dom, "allocation_limit", "aggregate FBX allocation budget exceeds its limit", start);
    const auto propertyCapacityBytes = mulSize(static_cast<std::size_t>(propertyCount), sizeof(FbxProperty), start, FbxStage::binary_dom);
    state.aggregateBytes = addSize(state.aggregateBytes, propertyCapacityBytes, start, FbxStage::binary_dom);
    if (state.aggregateBytes > reader.limits().maxAggregateBytes)
        fail(FbxStage::binary_dom, "allocation_limit", "aggregate FBX allocation budget exceeds its limit", start);
    FbxNode node; node.name = name; node.properties.reserve(static_cast<std::size_t>(propertyCount));
    for (std::uint64_t index=0; index<propertyCount; ++index) {
        if (state.properties++ >= reader.limits().maxProperties) fail(FbxStage::binary_dom, "property_limit", "FBX property count exceeds its limit", reader.offset());
        reader.need(1u, "truncated FBX property type"); const auto code = static_cast<char>(reader.all()[reader.offset()]); reader.bytesSpan(1u, "property type");
        FbxProperty property; property.code = code; std::size_t before = reader.offset();
        state.aggregateBytes = addSize(state.aggregateBytes, sizeof(FbxValue), before, FbxStage::binary_dom);
        if (state.aggregateBytes > reader.limits().maxAggregateBytes)
            fail(FbxStage::binary_dom, "allocation_limit", "aggregate FBX allocation budget exceeds its limit", before);
        property.values.push_back(binaryProperty(
            reader, code, before, state.arrayElements, state.arrayBytes,
            state.rawProperties, state.rawBytes, state.aggregateBytes));
        node.properties.push_back(std::move(property));
    }
    if (reader.offset() != propertyEnd) fail(FbxStage::binary_dom, "property_size", "FBX property list length does not match its properties", propertyEnd);
    // Some 7.3-era Autodesk writers omit the null record for a leaf node:
    // the node end offset is exactly the end of its property list.  A null
    // record is still required for nodes that contain children.  Accepting
    // this narrow leaf form is safe because the enclosing end offset and the
    // document root terminator remain mandatory.
    bool terminated = reader.offset() == endOffset;
    while (reader.offset() < endOffset) {
        const auto remaining = endOffset - reader.offset(); const auto nullSize = binaryNullSize(reader.version());
        if (remaining >= nullSize && allZero(reader.all(), reader.offset(), nullSize)) { reader.bytesSpan(nullSize, "node terminator"); terminated = true; break; }
        if (remaining < binaryHeaderSize(reader.version())) fail(FbxStage::binary_dom, "truncated", "truncated FBX child node", reader.offset());
        node.children.push_back(binaryNode(reader, depth + 1u, state));
    }
    if (!terminated) fail(FbxStage::binary_dom, "truncated", "FBX node is missing its null terminator", endOffset);
    if (reader.offset() != endOffset) fail(FbxStage::binary_dom, "offset", "FBX child nodes exceed end offset", endOffset);
    return node;
}

enum class TokenKind {
    word,
    string,
    raw_string,
    number,
    colon,
    comma,
    open,
    close,
    line_end,
    end
};
struct Token { TokenKind kind = TokenKind::end; std::string text; std::size_t offset = 0; };

struct AsciiArrayValue {
    union {
        std::int64_t integer;
        double real;
    } value;
    bool is_real = false;

    AsciiArrayValue() : value{0} {}
};

int hexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

void appendUtf8(std::string& output, unsigned codepoint) {
    if (codepoint <= 0x7fu) output.push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    }
}

std::size_t maximumBase64Bytes(std::size_t decodedBytes) {
    const auto groups = decodedBytes / 3u;
    const auto remainder = decodedBytes % 3u;
    const auto encoded = mulSize(groups, 4u, 0u, FbxStage::ascii_tokens);
    return remainder == 0u
               ? encoded
               : addSize(encoded, 4u, 0u, FbxStage::ascii_tokens);
}

std::vector<Token> tokenizeAscii(std::span<const std::uint8_t> bytes, const FbxLimits& limits) {
    const auto tokenCapacity = std::min(limits.maxAsciiTokens, bytes.size() / 2u + 1u);
    const auto tokenStorage = mulSize(tokenCapacity, sizeof(Token), 0u, FbxStage::ascii_tokens);
    if (tokenStorage > limits.maxAggregateBytes)
        fail(FbxStage::ascii_tokens, "allocation_limit", "FBX ASCII token storage exceeds the aggregate limit", 0u);
    std::vector<Token> tokens; tokens.reserve(tokenCapacity);
    std::size_t index = 0;
    std::size_t aggregateBytes = tokenStorage;
    std::size_t lineTokenBegin = 0u;
    bool continuedContentPayload = false;
    std::optional<std::size_t> continuedContentToken;
    bool continuedLineHasPayload = false;
    bool continuedLineCreatedToken = false;
    bool continuedLineHasComma = false;
    const auto maxRawTokenBytes =
        maximumBase64Bytes(limits.maxRawPropertyBytes);
    const auto push = [&](TokenKind kind, std::size_t at, std::string value) {
        if (tokens.size() >= limits.maxAsciiTokens) fail(FbxStage::ascii_tokens, "token_limit", "FBX ASCII token count exceeds its limit", at);
        const auto maxTokenBytes = kind == TokenKind::raw_string
                                       ? maxRawTokenBytes
                                       : limits.maxAsciiTokenBytes;
        if (value.size() > maxTokenBytes) fail(FbxStage::ascii_tokens, "token_limit", "FBX ASCII token exceeds its limit", at);
        if (tokens.size() == tokens.capacity()) {
            const auto currentCapacity = tokens.capacity();
            const auto doubled = addSize(currentCapacity, currentCapacity, at, FbxStage::ascii_tokens);
            const auto growth = std::max<std::size_t>(currentCapacity + 1u, doubled);
            const auto nextCapacity = std::min(limits.maxAsciiTokens, growth);
            if (nextCapacity <= currentCapacity)
                fail(FbxStage::ascii_tokens, "token_limit", "FBX ASCII token count exceeds its limit", at);
            const auto additionalStorage = mulSize(nextCapacity - currentCapacity, sizeof(Token), at, FbxStage::ascii_tokens);
            aggregateBytes = addSize(aggregateBytes, additionalStorage, at, FbxStage::ascii_tokens);
            if (aggregateBytes > limits.maxAggregateBytes)
                fail(FbxStage::ascii_tokens, "allocation_limit", "FBX ASCII token storage exceeds the aggregate limit", at);
            tokens.reserve(nextCapacity);
        }
        aggregateBytes = addSize(aggregateBytes, value.size(), at, FbxStage::ascii_tokens);
        if (aggregateBytes > limits.maxAggregateBytes)
            fail(FbxStage::ascii_tokens, "allocation_limit", "FBX ASCII token bytes exceed the aggregate limit", at);
        tokens.push_back({kind, std::move(value), at});
    };
    const auto contentCommaPrefix = [&]() {
        return tokens.size() == lineTokenBegin + 3u &&
               tokens[lineTokenBegin].kind == TokenKind::word &&
               tokens[lineTokenBegin].text == "Content" &&
               tokens[lineTokenBegin + 1u].kind == TokenKind::colon &&
               tokens[lineTokenBegin + 2u].kind == TokenKind::comma;
    };
    const auto contentDirectString = [&]() {
        return tokens.size() == lineTokenBegin + 2u &&
               tokens[lineTokenBegin].kind == TokenKind::word &&
               tokens[lineTokenBegin].text == "Content" &&
               tokens[lineTokenBegin + 1u].kind == TokenKind::colon;
    };
    const auto finishLine = [&](std::size_t at, bool eof = false) {
        if (continuedContentPayload) {
            const auto expected =
                lineTokenBegin + (continuedLineCreatedToken ? 1u : 0u);
            if (!continuedLineHasPayload || tokens.size() != expected)
                fail(FbxStage::ascii_tokens,
                     eof ? "truncated" : "embedded_content",
                     "FBX ASCII embedded Content requires one quoted base64 line",
                     at);
            if (!continuedLineHasComma) {
                continuedContentPayload = false;
                continuedContentToken.reset();
            }
        } else if (contentCommaPrefix()) {
            continuedContentPayload = true;
        }
        if (!continuedContentPayload || !continuedLineHasComma)
            push(TokenKind::line_end, at, {});
        lineTokenBegin = tokens.size();
        continuedLineHasPayload = false;
        continuedLineCreatedToken = false;
        continuedLineHasComma = false;
    };
    while (index < bytes.size()) {
        const auto c = static_cast<char>(bytes[index]);
        if (c == '\r' || c == '\n') { const auto at=index++; if (c=='\r' && index<bytes.size() && bytes[index]=='\n') ++index; finishLine(at); continue; }
        if (c == ' ' || c == '\t' || c == '\f' || c == '\v') { ++index; continue; }
        if (continuedContentPayload && continuedContentToken.has_value() &&
            !continuedLineHasPayload && c != '"' && c != ';') {
            // A comma on the previous chunk can either continue the payload
            // or terminate it. A non-quoted next line selects termination.
            continuedContentPayload = false;
            continuedContentToken.reset();
            push(TokenKind::line_end, index, {});
            lineTokenBegin = tokens.size();
        }
        if (static_cast<unsigned char>(c) < 0x20u) fail(FbxStage::ascii_tokens, "control", "unsupported ASCII control character", index);
        if (c == ';') { while (index<bytes.size() && bytes[index]!='\n' && bytes[index]!='\r') ++index; continue; }
        if (c == '/') { if (index+1u<bytes.size() && bytes[index+1u]=='/') { while(index<bytes.size()&&bytes[index]!='\n'&&bytes[index]!='\r')++index; continue; } }
        if (c == ':' || c == ',' || c == '{' || c == '}') {
            if (c == ',' && continuedContentPayload &&
                continuedLineHasPayload && !continuedLineHasComma) {
                continuedLineHasComma = true;
                ++index;
                continue;
            }
            push(c==':'?TokenKind::colon:c==','?TokenKind::comma:c=='{'?TokenKind::open:TokenKind::close,index,{}); ++index; continue;
        }
        if (c == '"') {
            const auto at=index++;
            const bool rawString = continuedContentPayload ||
                                   contentDirectString() ||
                                   contentCommaPrefix();
            std::string value;
            while (index < bytes.size()) {
                const auto current = static_cast<char>(bytes[index++]);
                if (current == '"') break;
                if (static_cast<unsigned char>(current) < 0x20u) fail(FbxStage::ascii_tokens, "control", "control character in FBX ASCII string", at);
                if (current == '\\') {
                    if (index >= bytes.size()) fail(FbxStage::ascii_tokens, "truncated", "truncated FBX ASCII escape", at);
                    const auto escaped = static_cast<char>(bytes[index++]);
                    switch (escaped) {
                    case '"': case '\\': case '/': value.push_back(escaped); break;
                    case 'b': value.push_back('\b'); break;
                    case 'f': value.push_back('\f'); break;
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    case 'u': {
                        if (index + 4u > bytes.size()) fail(FbxStage::ascii_tokens, "truncated", "truncated FBX Unicode escape", at);
                        unsigned codepoint = 0u;
                        for (unsigned digit = 0; digit < 4u; ++digit) {
                            const auto valueDigit = hexDigit(static_cast<char>(bytes[index++]));
                            if (valueDigit < 0) fail(FbxStage::ascii_tokens, "escape", "invalid FBX Unicode escape", at);
                            codepoint = (codepoint << 4u) | static_cast<unsigned>(valueDigit);
                        }
                        if (codepoint >= 0xd800u && codepoint <= 0xdfffu)
                            fail(FbxStage::ascii_tokens, "escape", "surrogate FBX Unicode escape is unsupported", at);
                        appendUtf8(value, codepoint);
                        break;
                    }
                    default: fail(FbxStage::ascii_tokens, "escape", "unsupported FBX ASCII escape", at);
                    }
                } else value.push_back(current);
                const auto maxStringBytes = rawString ? maxRawTokenBytes
                                                      : limits.maxAsciiTokenBytes;
                if (value.size() > maxStringBytes) fail(FbxStage::ascii_tokens, "token_limit", "FBX ASCII string exceeds its limit", at);
            }
            if (index > bytes.size() || (index == bytes.size() && (bytes[index-1u] != '"'))) fail(FbxStage::ascii_tokens, "truncated", "unterminated FBX ASCII string", at);
            if (continuedContentPayload) {
                if (continuedLineHasPayload)
                    fail(FbxStage::ascii_tokens, "embedded_content",
                         "FBX ASCII embedded Content has multiple chunks on one line",
                         at);
                if (!continuedContentToken.has_value()) {
                    push(TokenKind::raw_string, at, std::move(value));
                    continuedContentToken = tokens.size() - 1u;
                    continuedLineCreatedToken = true;
                } else {
                    auto& payload = tokens[*continuedContentToken].text;
                    const auto combined = addSize(
                        payload.size(), value.size(), at,
                        FbxStage::ascii_tokens);
                    if (combined > maxRawTokenBytes)
                        fail(FbxStage::ascii_tokens, "token_limit",
                             "FBX ASCII embedded Content exceeds its encoded byte limit",
                             at);
                    aggregateBytes = addSize(
                        aggregateBytes, value.size(), at,
                        FbxStage::ascii_tokens);
                    if (aggregateBytes > limits.maxAggregateBytes)
                        fail(FbxStage::ascii_tokens, "allocation_limit",
                             "FBX ASCII token bytes exceed the aggregate limit",
                             at);
                    payload.append(value);
                }
                continuedLineHasPayload = true;
            } else {
                push(rawString ? TokenKind::raw_string : TokenKind::string,
                     at, std::move(value));
            }
            continue;
        }
        const auto at=index; while (index<bytes.size()) { const auto current=static_cast<char>(bytes[index]); if (current==' '||current=='\t'||current=='\r'||current=='\n'||current==':'||current==','||current=='{'||current=='}'||current==';') break; if (static_cast<unsigned char>(current) < 0x20u) fail(FbxStage::ascii_tokens, "control", "unsupported ASCII control character", index); ++index; }
        const auto value=std::string(reinterpret_cast<const char*>(bytes.data()+at), index-at);
        const bool number = !value.empty() && (std::isdigit(static_cast<unsigned char>(value.front()))!=0 || value.front()=='-' || value.front()=='+' || value.front()=='.');
        push(number?TokenKind::number:TokenKind::word, at, value);
    }
    if (!continuedContentPayload && contentCommaPrefix())
        fail(FbxStage::ascii_tokens, "truncated",
             "truncated FBX ASCII embedded Content", bytes.size());
    if (continuedContentPayload) {
        if (!continuedLineHasPayload && continuedContentToken.has_value()) {
            continuedContentPayload = false;
            continuedContentToken.reset();
            push(TokenKind::line_end, bytes.size(), {});
        } else {
            finishLine(bytes.size(), true);
        }
    }
    tokens.push_back({TokenKind::end, {}, bytes.size()}); return tokens;
}

bool asciiNonFinite(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    });
    return lower == "nan" || lower == "+nan" || lower == "-nan" || lower == "inf" ||
           lower == "+inf" || lower == "-inf" || lower == "infinity" ||
           lower == "+infinity" || lower == "-infinity";
}

int base64Digit(char value) noexcept {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

FbxValue asciiValue(const Token& token) {
    if ((token.kind == TokenKind::word || token.kind == TokenKind::number) && asciiNonFinite(token.text))
        fail(FbxStage::ascii_dom, "non_finite", "non-finite FBX ASCII number", token.offset);
    if (token.kind == TokenKind::string) return token.text;
    if (token.kind == TokenKind::word && (token.text == "T" || token.text == "true" || token.text == "True")) return true;
    if (token.kind == TokenKind::word && (token.text == "F" || token.text == "false" || token.text == "False")) return false;
    if (token.kind == TokenKind::number) {
        std::int64_t integer=0; const auto intResult=std::from_chars(token.text.data(), token.text.data()+token.text.size(), integer);
        if (intResult.ec == std::errc{} && intResult.ptr == token.text.data()+token.text.size()) return integer;
        double real=0; const auto realResult=std::from_chars(token.text.data(), token.text.data()+token.text.size(), real);
        if (realResult.ec == std::errc{} && realResult.ptr == token.text.data()+token.text.size()) {
            if (!std::isfinite(real)) fail(FbxStage::ascii_dom, "non_finite", "non-finite FBX ASCII number", token.offset);
            return real;
        }
        fail(FbxStage::ascii_dom, "numeric", "invalid FBX ASCII numeric token", token.offset);
    }
    return token.text;
}

class AsciiParser final {
public:
    AsciiParser(std::vector<Token> tokens, const FbxLimits& limits) : tokens_(std::move(tokens)), limits_(limits) {}
    std::vector<FbxNode> parse() {
        std::vector<FbxNode> roots;
        while (peek().kind != TokenKind::end) { skipLines(); if (peek().kind == TokenKind::end) break; roots.push_back(node(0)); }
        return roots;
    }
    std::size_t nodes() const noexcept { return nodes_; }
private:
    const Token& peek(std::size_t look=0) const { return tokens_[std::min(position_+look, tokens_.size()-1u)]; }
    Token take() { return tokens_[position_++]; }
    void skipLines() { while (peek().kind == TokenKind::line_end) ++position_; }
    void account(std::size_t bytes, std::size_t offset) {
        aggregateBytes_ = addSize(aggregateBytes_, bytes, offset, FbxStage::ascii_dom);
        if (aggregateBytes_ > limits_.maxAggregateBytes)
            fail(FbxStage::ascii_dom, "allocation_limit", "aggregate FBX allocation budget exceeds its limit", offset);
    }

    [[nodiscard]] std::vector<std::uint8_t> base64Content(
        const Token& token) {
        const auto size = token.text.size();
        if (size % 4u != 0u)
            fail(FbxStage::ascii_dom, "content_base64",
                 "FBX ASCII embedded Content has an incomplete base64 quartet",
                 token.offset);
        std::size_t padding = 0u;
        if (size != 0u && token.text[size - 1u] == '=') ++padding;
        if (size > 1u && token.text[size - 2u] == '=') ++padding;
        const auto groups = size / 4u;
        const auto decodedCapacity = mulSize(
            groups, 3u, token.offset, FbxStage::ascii_dom);
        if (padding > decodedCapacity)
            fail(FbxStage::ascii_dom, "content_base64",
                 "FBX ASCII embedded Content has invalid base64 padding",
                 token.offset);
        const auto decodedSize = decodedCapacity - padding;
        if (decodedSize > limits_.maxRawPropertyBytes)
            fail(FbxStage::ascii_dom, "raw_property_limit",
                 "FBX ASCII embedded Content exceeds its byte limit",
                 token.offset);
        if (rawProperties_ >= limits_.maxRawProperties)
            fail(FbxStage::ascii_dom, "raw_property_limit",
                 "FBX ASCII embedded Content count exceeds its limit",
                 token.offset);
        const auto nextRawBytes = addSize(
            rawBytes_, decodedSize, token.offset, FbxStage::ascii_dom);
        if (nextRawBytes > limits_.maxRawBytes)
            fail(FbxStage::ascii_dom, "raw_property_limit",
                 "aggregate FBX ASCII embedded Content bytes exceed their limit",
                 token.offset);
        account(sizeof(std::vector<std::uint8_t>), token.offset);
        account(decodedSize, token.offset);

        std::vector<std::uint8_t> output;
        output.reserve(decodedSize);
        for (std::size_t offset = 0u; offset < size; offset += 4u) {
            const bool last = offset + 4u == size;
            const char third = token.text[offset + 2u];
            const char fourth = token.text[offset + 3u];
            const int a = base64Digit(token.text[offset]);
            const int b = base64Digit(token.text[offset + 1u]);
            const int c = third == '=' ? 0 : base64Digit(third);
            const int d = fourth == '=' ? 0 : base64Digit(fourth);
            if (a < 0 || b < 0 || c < 0 || d < 0 ||
                (!last && (third == '=' || fourth == '=')) ||
                (third == '=' && fourth != '=') ||
                (third == '=' && (b & 0x0f) != 0) ||
                (fourth == '=' && third != '=' && (c & 0x03) != 0))
                fail(FbxStage::ascii_dom, "content_base64",
                     "FBX ASCII embedded Content has invalid base64 data",
                     token.offset + offset);
            output.push_back(static_cast<std::uint8_t>((a << 2) | (b >> 4)));
            if (third != '=')
                output.push_back(static_cast<std::uint8_t>(
                    ((b & 0x0f) << 4) | (c >> 2)));
            if (fourth != '=')
                output.push_back(static_cast<std::uint8_t>(
                    ((c & 0x03) << 6) | d));
        }
        if (output.size() != decodedSize)
            fail(FbxStage::ascii_dom, "content_base64",
                 "FBX ASCII embedded Content decoded to an invalid size",
                 token.offset);
        ++rawProperties_;
        rawBytes_ = nextRawBytes;
        return output;
    }

    [[nodiscard]] std::size_t arrayCount(const Token& marker) const {
        if (marker.text.size() <= 1u || marker.text.front() != '*')
            fail(FbxStage::ascii_dom, "array_syntax", "FBX ASCII array is missing its element count", marker.offset);
        std::size_t count = 0u;
        const auto result = std::from_chars(marker.text.data() + 1u,
                                            marker.text.data() + marker.text.size(), count);
        if (result.ec != std::errc{} || result.ptr != marker.text.data() + marker.text.size())
            fail(FbxStage::ascii_dom, "array_count", "FBX ASCII array element count is invalid", marker.offset);
        if (count > limits_.maxArrayElements)
            fail(FbxStage::ascii_dom, "array_limit", "FBX ASCII array element count exceeds its limit", marker.offset);
        return count;
    }

    [[nodiscard]] FbxValue asciiArray() {
        const auto marker = take();
        const auto count = arrayCount(marker);
        if (peek().kind != TokenKind::open)
            fail(FbxStage::ascii_dom, "array_syntax", "FBX ASCII array is missing its value block", marker.offset);

        const auto outputBytes = mulSize(count, sizeof(double), marker.offset, FbxStage::ascii_dom);
        if (outputBytes > limits_.maxArrayBytes)
            fail(FbxStage::ascii_dom, "array_limit", "FBX ASCII array byte count exceeds its limit", marker.offset);
        const auto nextElements = addSize(arrayElements_, count, marker.offset, FbxStage::ascii_dom);
        const auto nextBytes = addSize(arrayBytes_, outputBytes, marker.offset, FbxStage::ascii_dom);
        if (nextElements > limits_.maxArrayElements || nextBytes > limits_.maxArrayBytes)
            fail(FbxStage::ascii_dom, "array_limit", "aggregate FBX ASCII array budget exceeds its limit", marker.offset);
        // Keep a compact temporary value for each element while determining
        // whether the array is integral or floating point. The final vector
        // uses the same conservative double-sized accounting as binary arrays.
        account(mulSize(count, sizeof(AsciiArrayValue), marker.offset, FbxStage::ascii_dom), marker.offset);
        account(outputBytes, marker.offset);
        account(sizeof(FbxArray), marker.offset);
        arrayElements_ = nextElements;
        arrayBytes_ = nextBytes;

        take();
        skipLines();
        if (peek().kind != TokenKind::word || peek().text != "a")
            fail(FbxStage::ascii_dom, "array_syntax", "FBX ASCII array value block must start with 'a:'", marker.offset);
        take();
        if (peek().kind != TokenKind::colon)
            fail(FbxStage::ascii_dom, "array_syntax", "FBX ASCII array value block is missing ':'", marker.offset);
        take();

        std::vector<AsciiArrayValue> values;
        values.reserve(count);
        bool hasReal = false;
        while (true) {
            if (peek().kind == TokenKind::line_end || peek().kind == TokenKind::comma) {
                take();
                continue;
            }
            if (peek().kind == TokenKind::close) {
                take();
                break;
            }
            if (peek().kind == TokenKind::end)
                fail(FbxStage::ascii_dom, "truncated", "truncated FBX ASCII array value block", marker.offset);
            if (values.size() >= count)
                fail(FbxStage::ascii_dom, "array_size", "FBX ASCII array contains more values than declared", marker.offset);

            const auto token = take();
            const auto scalar = asciiValue(token);
            AsciiArrayValue value;
            if (const auto integer = std::get_if<std::int64_t>(&scalar)) {
                value.value.integer = *integer;
            } else if (const auto real = std::get_if<double>(&scalar)) {
                value.value.real = *real;
                value.is_real = true;
                hasReal = true;
            } else {
                fail(FbxStage::ascii_dom, "array_type", "FBX ASCII arrays contain only numeric values", token.offset);
            }
            values.push_back(value);
        }
        if (values.size() != count)
            fail(FbxStage::ascii_dom, "array_size", "FBX ASCII array value count does not match its declaration", marker.offset);
        if (!hasReal) {
            std::vector<std::int64_t> output;
            output.reserve(count);
            for (const auto& value : values) output.push_back(value.value.integer);
            return FbxArray{std::move(output)};
        }
        std::vector<double> output;
        output.reserve(count);
        for (const auto& value : values)
            output.push_back(value.is_real ? value.value.real : static_cast<double>(value.value.integer));
        return FbxArray{std::move(output)};
    }

    FbxNode node(std::size_t depth) {
        if (depth >= limits_.maxDepth) fail(FbxStage::ascii_dom, "depth_limit", "FBX ASCII nesting exceeds its limit", peek().offset);
        if (++nodes_ > limits_.maxNodes) fail(FbxStage::ascii_dom, "node_limit", "FBX ASCII node count exceeds its limit", peek().offset);
        const auto nameToken=take(); if (nameToken.kind != TokenKind::word && nameToken.kind != TokenKind::string) fail(FbxStage::ascii_dom, "syntax", "expected FBX ASCII node name", nameToken.offset);
        account(sizeof(FbxNode), nameToken.offset);
        account(nameToken.text.size(), nameToken.offset);
        FbxNode result; result.name=nameToken.text;
        if (peek().kind == TokenKind::colon) take(); else if (peek().kind != TokenKind::open) fail(FbxStage::ascii_dom, "syntax", "expected FBX ASCII colon", peek().offset);
        FbxProperty property; property.code=0;
        if (result.name == "Content" && peek().kind == TokenKind::comma) {
            const auto marker = take();
            if (peek().kind == TokenKind::raw_string) {
                const auto payload = take();
                if (peek().kind == TokenKind::comma) take();
                account(sizeof(FbxValue), payload.offset);
                property.code = 'R';
                property.values.push_back(base64Content(payload));
                if (++properties_ > limits_.maxProperties)
                    fail(FbxStage::ascii_dom, "property_limit",
                         "FBX ASCII property count exceeds its limit",
                         payload.offset);
            } else {
                if (peek().kind != TokenKind::line_end)
                    fail(FbxStage::ascii_dom, "embedded_content",
                         "FBX ASCII embedded Content continuation is malformed",
                         marker.offset);
                take();
                if (peek().kind != TokenKind::raw_string)
                    fail(FbxStage::ascii_dom, "truncated",
                         "truncated FBX ASCII embedded Content", marker.offset);
                const auto payload = take();
                if (peek().kind == TokenKind::comma) take();
                account(sizeof(FbxValue), payload.offset);
                property.code = 'R';
                property.values.push_back(base64Content(payload));
                if (++properties_ > limits_.maxProperties)
                    fail(FbxStage::ascii_dom, "property_limit",
                         "FBX ASCII property count exceeds its limit",
                         payload.offset);
            }
        } else if (result.name == "Content" &&
                   peek().kind == TokenKind::raw_string) {
            const auto payload = take();
            if (peek().kind == TokenKind::comma) take();
            account(sizeof(FbxValue), payload.offset);
            property.code = 'R';
            property.values.push_back(base64Content(payload));
            if (++properties_ > limits_.maxProperties)
                fail(FbxStage::ascii_dom, "property_limit",
                     "FBX ASCII property count exceeds its limit",
                     payload.offset);
        } else if (peek().kind == TokenKind::word && !peek().text.empty() && peek().text.front() == '*') {
            const auto marker = peek();
            if (peek(1u).kind != TokenKind::open)
                fail(FbxStage::ascii_dom, "array_syntax", "FBX ASCII array is missing its value block", marker.offset);
            account(sizeof(FbxValue), marker.offset);
            property.values.push_back(asciiArray());
            if (++properties_ > limits_.maxProperties)
                fail(FbxStage::ascii_dom, "property_limit", "FBX ASCII property count exceeds its limit", marker.offset);
        } else {
            while (peek().kind != TokenKind::open && peek().kind != TokenKind::line_end && peek().kind != TokenKind::end && peek().kind != TokenKind::close) {
                const auto token=take(); if (token.kind == TokenKind::comma) continue;
                account(sizeof(FbxValue), token.offset);
                property.values.push_back(asciiValue(token));
                if (++properties_ > limits_.maxProperties) fail(FbxStage::ascii_dom, "property_limit", "FBX ASCII property count exceeds its limit", token.offset);
            }
        }
        if (!property.values.empty()) { account(sizeof(FbxProperty), nameToken.offset); result.properties.push_back(std::move(property)); }
        if (peek().kind == TokenKind::line_end) take();
        if (peek().kind != TokenKind::open) return result;
        take(); skipLines();
        while (peek().kind != TokenKind::close && peek().kind != TokenKind::end) { result.children.push_back(node(depth+1u)); skipLines(); }
        if (peek().kind != TokenKind::close) fail(FbxStage::ascii_dom, "truncated", "unterminated FBX ASCII node block", nameToken.offset);
        take(); if (peek().kind == TokenKind::line_end) take(); return result;
    }
    std::vector<Token> tokens_; const FbxLimits& limits_; std::size_t position_=0; std::size_t nodes_=0; std::size_t properties_=0; std::size_t aggregateBytes_=0; std::size_t arrayElements_=0; std::size_t arrayBytes_=0; std::size_t rawProperties_=0; std::size_t rawBytes_=0;
};

} // namespace

FbxConversionCapability fbxConversionCapability() {
    return {true, false, FbxStage::conversion, "CONVERSION_UNIMPLEMENTED",
            "FBX binary/ASCII DOM parsing is available; scene conversion is a separate staged capability and is not implemented"};
}

FbxHeader inspectFbxHeader(std::span<const std::uint8_t> bytes, std::string source) {
    (void)source;
    const auto prefixLength = std::min<std::size_t>(bytes.size(), 256u);
    const auto prefix = std::string_view(reinterpret_cast<const char*>(bytes.data()), prefixLength);
    if (prefix.starts_with(kBinaryMagic) && bytes.size() >= 27u)
        return {FbxFormat::binary, little32(bytes, 23u, FbxStage::header)};
    std::size_t marker = std::string_view::npos;
    for (std::size_t candidate = 0; candidate + 10u <= prefix.size(); ++candidate) {
        const std::string_view word = prefix.substr(candidate, 10u);
        if (std::equal(word.begin(), word.end(), "FBXVersion", [](char left, char right) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(left))) ==
                       static_cast<char>(std::tolower(static_cast<unsigned char>(right)));
            })) { marker = candidate; break; }
    }
    if (marker != std::string_view::npos) {
        std::size_t index=marker+10u;
        while(index<prefix.size() && std::isspace(static_cast<unsigned char>(prefix[index])) != 0) ++index;
        if (index >= prefix.size() || prefix[index] != ':')
            fail(FbxStage::header, "header", "The file does not contain a recognized FBX header", 0);
        ++index;
        while(index<prefix.size() && std::isspace(static_cast<unsigned char>(prefix[index])) != 0) ++index;
        const auto begin=index; while(index<prefix.size() && prefix[index]>='0'&&prefix[index]<='9') ++index;
        if (index>begin) { std::uint32_t version=0; const auto result=std::from_chars(prefix.data()+begin,prefix.data()+index,version); if(result.ec==std::errc{}) return {FbxFormat::ascii,version}; }
    }
    fail(FbxStage::header, "header", "The file does not contain a recognized FBX header", 0);
}

FbxDocument parseFbx(std::span<const std::uint8_t> bytes, std::string source, FbxLimits limits) {
    checkInput(bytes, limits, source); const auto header=inspectFbxHeader(bytes, source);
    if (header.version < 6000u || header.version > 8000u) fail(FbxStage::header, "unsupported_version", "Unsupported FBX version", 23u);
    FbxDocument document; document.header=header;
    if (header.format == FbxFormat::ascii) {
        auto tokens=tokenizeAscii(bytes, limits); AsciiParser parser(std::move(tokens), limits); document.roots=parser.parse();
        if (document.roots.empty() || std::none_of(document.roots.begin(), document.roots.end(),
                                                    [](const FbxNode& node) { return !node.children.empty(); }))
            fail(FbxStage::ascii_dom, "empty", "FBX ASCII document contains no object block", 0);
        document.bytesRead=bytes.size(); document.trailingBytes=0; return document;
    }
    BinaryReader reader(bytes, header.version, limits); reader.bytesSpan(27u, "FBX binary header"); BinaryState state;
    const auto nullSize = binaryNullSize(header.version);
    while (reader.remaining() >= nullSize && !allZero(bytes, reader.offset(), nullSize)) {
        if (reader.remaining() < binaryHeaderSize(header.version))
            fail(FbxStage::binary_dom, "root_terminator", "truncated FBX root terminator", reader.offset());
        document.roots.push_back(binaryNode(reader, 0, state));
    }
    if (document.roots.empty()) fail(FbxStage::binary_dom, "empty", "FBX binary document contains no nodes", reader.offset());
    if (reader.remaining() < nullSize || !allZero(bytes, reader.offset(), nullSize))
        fail(FbxStage::binary_dom, "root_terminator", "FBX binary document is missing its root terminator", reader.offset());
    reader.bytesSpan(nullSize, "FBX root terminator");
    document.trailingBytes = reader.remaining();
    // Autodesk writers normally append a fixed opaque footer. We do not
    // interpret it, but reject ambiguous short tails and bound its storage.
    if (document.trailingBytes != 0u && document.trailingBytes < 16u)
        fail(FbxStage::binary_dom, "footer", "FBX trailing bytes are not a recognized footer", reader.offset());
    if (document.trailingBytes > limits.maxFooterBytes)
        fail(FbxStage::binary_dom, "footer_limit", "FBX footer exceeds its configured limit", reader.offset());
    document.bytesRead=bytes.size();
    return document;
}

} // namespace apex::formats
