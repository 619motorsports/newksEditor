#include "apex/formats/fbx.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

using apex::formats::FbxError;
using apex::formats::FbxFormat;
using apex::formats::FbxStage;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned index=0; index<4u; ++index) bytes.push_back(static_cast<std::uint8_t>(value >> (index*8u)));
}

void put64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned index=0; index<8u; ++index) bytes.push_back(static_cast<std::uint8_t>(value >> (index*8u)));
}

std::vector<std::uint8_t> binaryMinimal(std::uint32_t version = 7400u, bool property = false) {
    std::vector<std::uint8_t> bytes{'K','a','y','d','a','r','a',' ','F','B','X',' ','B','i','n','a','r','y',' ',' ',0x1a,0x00,0x00};
    put32(bytes, version);
    const auto start = bytes.size();
    const auto header = version >= 7500u ? 25u : 13u;
    const auto propertyBytes = property ? 5u : 0u;
    const auto nameBytes = 4u;
    const auto nullBytes = version >= 7500u ? 25u : 13u;
    const auto end = static_cast<std::uint64_t>(start + header + nameBytes + propertyBytes + nullBytes);
    if (version >= 7500u) { put64(bytes,end); put64(bytes,property ? 1u : 0u); put64(bytes,propertyBytes); }
    else { put32(bytes,static_cast<std::uint32_t>(end)); put32(bytes,property ? 1u : 0u); put32(bytes,propertyBytes); }
    bytes.push_back(4u); bytes.insert(bytes.end(), {'R','o','o','t'});
    if (property) { bytes.push_back('I'); put32(bytes, 42u); }
    bytes.resize(bytes.size() + nullBytes, 0u);
    bytes.resize(bytes.size() + nullBytes, 0u); // node terminator followed by root terminator
    return bytes;
}

std::vector<std::uint8_t> binaryLeafWithoutNodeTerminator() {
    auto bytes = binaryMinimal(7400u, true);
    // The fixture has a 13-byte node terminator immediately before its
    // 13-byte root terminator.  GT40 uses the legal leaf form with only the
    // root terminator after the property list.
    const auto oldEnd = static_cast<std::uint32_t>(bytes[27u]) |
                        (static_cast<std::uint32_t>(bytes[28u]) << 8u) |
                        (static_cast<std::uint32_t>(bytes[29u]) << 16u) |
                        (static_cast<std::uint32_t>(bytes[30u]) << 24u);
    const auto nodeEnd = oldEnd - 13u;
    for (unsigned index = 0; index < 4u; ++index) bytes[27u + index] = static_cast<std::uint8_t>(nodeEnd >> (index * 8u));
    bytes.erase(bytes.end() - 26, bytes.end() - 13);
    return bytes;
}

std::vector<std::uint8_t> binaryFloatArray(bool compressed) {
    std::vector<std::uint8_t> bytes{'K','a','y','d','a','r','a',' ','F','B','X',' ','B','i','n','a','r','y',' ',' ',0x1a,0x00,0x00};
    put32(bytes, 7400u);
    const auto start = bytes.size();
    const std::vector<std::uint8_t> raw = {0,0,128,63,0,0,0,64};
    const std::vector<std::uint8_t> encoded = compressed
        ? std::vector<std::uint8_t>{0x78u,0x01u,0x01u,0x08u,0x00u,0xf7u,0xffu,0,0,128,63,0,0,0,64,0x04u,0x83u,0x01u,0}
        : raw;
    const auto end = static_cast<std::uint32_t>(start + 13u + 4u + 1u + 12u + encoded.size() + 13u);
    put32(bytes, end); put32(bytes, 1u); put32(bytes, static_cast<std::uint32_t>(1u + 12u + encoded.size()));
    bytes.push_back(4u); bytes.insert(bytes.end(), {'R','o','o','t'}); bytes.push_back('f');
    put32(bytes, 2u); put32(bytes, compressed ? 1u : 0u); put32(bytes, static_cast<std::uint32_t>(encoded.size()));
    bytes.insert(bytes.end(), encoded.begin(), encoded.end()); bytes.resize(bytes.size()+13u, 0u); bytes.resize(bytes.size()+13u, 0u); return bytes;
}

std::vector<std::uint8_t> binaryRawProperty(
    std::vector<std::uint8_t> payload = {0u, 1u, 2u, 0xffu}) {
    std::vector<std::uint8_t> bytes{'K','a','y','d','a','r','a',' ','F','B','X',' ','B','i','n','a','r','y',' ',' ',0x1a,0x00,0x00};
    put32(bytes, 7400u);
    const auto start = bytes.size();
    const auto propertyBytes = 1u + 4u + payload.size();
    const auto end = static_cast<std::uint32_t>(
        start + 13u + 4u + propertyBytes + 13u);
    put32(bytes, end);
    put32(bytes, 1u);
    put32(bytes, static_cast<std::uint32_t>(propertyBytes));
    bytes.push_back(4u);
    bytes.insert(bytes.end(), {'R','o','o','t'});
    bytes.push_back('R');
    put32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    bytes.resize(bytes.size() + 13u, 0u);
    bytes.resize(bytes.size() + 13u, 0u);
    return bytes;
}

std::string asciiMinimal() {
    return "; FBX 7.5.0 project file\nFBXVersion: 7400\nObjects: {\n Model: 1, \"Model::Cube\", \"Mesh\" {\n  Version: 232\n }\n}\n";
}

std::string asciiArrayTriangle(std::string_view vertexCount = "9",
                               std::string_view vertices = "0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0,0.0",
                               std::string_view indexCount = "3",
                               std::string_view indices = "0,1,-3") {
    return "FBXVersion: 7400\nObjects: {\n Geometry: 100, \"Geometry::Triangle\", \"Mesh\" {\n  Vertices: *" +
        std::string(vertexCount) + " {\n   a: " + std::string(vertices) +
        "\n  }\n  PolygonVertexIndex: *" + std::string(indexCount) + " {\n   a: " +
        std::string(indices) + "\n  }\n }\n}\n";
}

std::string asciiEmbeddedContent(std::string_view payload = "AAEC/w==",
                                 bool continued = true,
                                 bool trailingComma = false) {
    return "FBXVersion: 7400\nObjects: {\n"
           " Video: 600, \"Video::Paint\", \"Clip\" {\n"
           "  Content: " +
           std::string(continued ? ",\n   \"" : "\"") +
           std::string(payload) + "\"" +
           std::string(trailingComma ? "," : "") + "\n }\n}\n";
}

template <typename Function>
void expectsFbxError(Function&& function, std::string_view code, FbxStage stage) {
    try { function(); }
    catch (const FbxError& error) {
        require(error.code() == code && error.stage() == stage,
                std::string("unexpected FBX diagnostic expected ") + std::string(code) + " got " + error.code());
        return;
    }
    throw std::runtime_error("malformed FBX input was accepted");
}

void inspectsHeadersExactly() {
    const auto binary = binaryMinimal();
    const auto header = apex::formats::inspectFbxHeader(binary);
    require(header.format == FbxFormat::binary && header.version == 7400u, "binary FBX header");
    const auto ascii = asciiMinimal();
    const auto asciiHeader = apex::formats::inspectFbxHeader({reinterpret_cast<const std::uint8_t*>(ascii.data()), ascii.size()});
    require(asciiHeader.format == FbxFormat::ascii && asciiHeader.version == 7400u, "ASCII FBX header");
    const std::string spaced = "fBxVeRsIoN \t:\n 7500";
    const auto spacedHeader = apex::formats::inspectFbxHeader({reinterpret_cast<const std::uint8_t*>(spaced.data()), spaced.size()});
    require(spacedHeader.format == FbxFormat::ascii && spacedHeader.version == 7500u, "ASCII FBX header whitespace");
    expectsFbxError([] {
        const std::string malformed = "FBXVersion 7400";
        (void)apex::formats::inspectFbxHeader({reinterpret_cast<const std::uint8_t*>(malformed.data()), malformed.size()});
    }, "header", FbxStage::header);
    std::vector<std::uint8_t> truncated(binary.begin(), binary.begin()+26);
    expectsFbxError([&] { (void)apex::formats::inspectFbxHeader(truncated); }, "header", FbxStage::header);
    expectsFbxError([] { (void)apex::formats::inspectFbxHeader(std::span<const std::uint8_t>{}); }, "header", FbxStage::header);
}

void parsesBinaryAndAsciiDom() {
    const auto oldBinary = apex::formats::parseFbx(binaryMinimal(7400u, true));
    require(oldBinary.header.format == FbxFormat::binary && oldBinary.roots.size() == 1u &&
                oldBinary.roots[0].name == "Root" && std::get<std::int64_t>(oldBinary.roots[0].properties[0].values[0]) == 42,
            "32-bit binary FBX DOM");
    const auto newBinary = apex::formats::parseFbx(binaryMinimal(7600u));
    require(newBinary.header.version == 7600u && newBinary.roots[0].name == "Root", "64-bit binary FBX DOM");
    for (const auto compressed : {false, true}) {
        const auto arrayDocument = apex::formats::parseFbx(binaryFloatArray(compressed));
        const auto& value = arrayDocument.roots[0].properties[0].values[0];
        require(std::get<apex::formats::FbxArray>(value).index() == 2u &&
                    std::get<std::vector<float>>(std::get<apex::formats::FbxArray>(value))[1] == 2.0F,
                "bounded binary float array");
    }
    const auto ascii = asciiMinimal();
    const auto document = apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(ascii.data()), ascii.size()});
    require(document.header.format == FbxFormat::ascii && document.roots.size() == 2u,
            "ASCII FBX root DOM");
    require(document.roots[1].name == "Objects" && document.roots[1].children[0].name == "Model" &&
                std::get<std::int64_t>(document.roots[1].children[0].children[0].properties[0].values[0]) == 232,
            "ASCII FBX nested typed values");
}

void parsesBoundedAsciiNumericArrays() {
    const auto text = asciiArrayTriangle();
    const auto document = apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    const auto& geometry = document.roots[1].children[0];
    require(geometry.name == "Geometry" && geometry.properties.size() == 1u && geometry.properties[0].values.size() == 3u &&
                geometry.children.size() == 2u,
            "ASCII geometry properties are retained");
    const auto* vertices = std::get_if<apex::formats::FbxArray>(&geometry.children[0].properties[0].values[0]);
    require(vertices != nullptr && std::holds_alternative<std::vector<double>>(*vertices),
            "ASCII floating array is represented as FbxArray");
    require(std::get<std::vector<double>>(*vertices) == std::vector<double>{0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0},
            "ASCII vertex array values");
    const auto* indices = std::get_if<apex::formats::FbxArray>(&geometry.children[1].properties[0].values[0]);
    require(indices != nullptr && std::holds_alternative<std::vector<std::int64_t>>(*indices),
            "ASCII integral array is represented as FbxArray");
    require(std::get<std::vector<std::int64_t>>(*indices) == std::vector<std::int64_t>{0, 1, -3},
            "ASCII index array values");
}

void parsesBoundedEmbeddedContent() {
    for (const bool continued : {false, true}) {
        const auto text = asciiEmbeddedContent("AAEC/w==", continued);
        const auto document = apex::formats::parseFbx({
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
        const auto& content =
            document.roots[1].children[0].children[0];
        require(content.name == "Content" && content.properties.size() == 1u &&
                    content.properties[0].code == 'R' &&
                    std::get<std::vector<std::uint8_t>>(
                        content.properties[0].values[0]) ==
                        std::vector<std::uint8_t>{0u, 1u, 2u, 0xffu},
                "ASCII embedded Content decodes to bounded raw bytes");
    }
    const auto commaText = asciiEmbeddedContent("AAEC/w==", true, true);
    const auto commaDocument = apex::formats::parseFbx({
        reinterpret_cast<const std::uint8_t*>(commaText.data()),
        commaText.size()});
    require(std::get<std::vector<std::uint8_t>>(
                commaDocument.roots[1].children[0].children[0]
                    .properties[0].values[0]) ==
                std::vector<std::uint8_t>{0u, 1u, 2u, 0xffu},
            "ASCII embedded Content accepts the exporter trailing comma");
    auto sameLine = asciiEmbeddedContent();
    const auto continuation = sameLine.find(",\n   \"");
    require(continuation != std::string::npos,
            "ASCII embedded fixture has a continuation line");
    sameLine.replace(continuation, 6U, ", \"");
    const auto sameLineDocument = apex::formats::parseFbx({
        reinterpret_cast<const std::uint8_t*>(sameLine.data()),
        sameLine.size()});
    require(std::get<std::vector<std::uint8_t>>(
                sameLineDocument.roots[1].children[0].children[0]
                    .properties[0].values[0]) ==
                std::vector<std::uint8_t>{0u, 1u, 2u, 0xffu},
            "ASCII embedded Content accepts a same-line payload");

    const auto binary = apex::formats::parseFbx(binaryRawProperty());
    require(std::get<std::vector<std::uint8_t>>(
                binary.roots[0].properties[0].values[0]) ==
                std::vector<std::uint8_t>{0u, 1u, 2u, 0xffu},
            "binary raw property retains bounded bytes");
}

void rejectsMalformedEmbeddedContent() {
    for (const auto invalid : {"AA?C", "AA=A", "AAE"}) {
        const auto text = asciiEmbeddedContent(invalid);
        expectsFbxError([&] {
            (void)apex::formats::parseFbx({
                reinterpret_cast<const std::uint8_t*>(text.data()),
                text.size()});
        }, "content_base64", FbxStage::ascii_dom);
    }

    const std::string missing =
        "FBXVersion: 7400\nObjects: {\n Video: 600, \"Video::Paint\", \"Clip\" {\n"
        "  Content: ,\n }\n}\n";
    expectsFbxError([&] {
        (void)apex::formats::parseFbx({
            reinterpret_cast<const std::uint8_t*>(missing.data()),
            missing.size()});
    }, "embedded_content", FbxStage::ascii_tokens);

    auto rawLimits = apex::formats::FbxLimits{};
    rawLimits.maxRawPropertyBytes = 3u;
    const auto valid = asciiEmbeddedContent();
    expectsFbxError([&] {
        (void)apex::formats::parseFbx({
            reinterpret_cast<const std::uint8_t*>(valid.data()), valid.size()},
            "embedded.fbx", rawLimits);
    }, "token_limit", FbxStage::ascii_tokens);

    auto binary = binaryRawProperty();
    // Raw property length starts after the 27-byte file header, 13-byte node
    // header, four-byte node name, and one-byte property type.
    binary[45u] = 0xffu;
    expectsFbxError([&] { (void)apex::formats::parseFbx(binary); },
                    "truncated", FbxStage::binary_dom);

    const auto complete = asciiEmbeddedContent();
    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(complete.data()),
        complete.size());
    for (std::size_t length = 0u; length + 1u < bytes.size(); ++length) {
        bool rejected = false;
        try {
            (void)apex::formats::parseFbx(bytes.first(length),
                                          "embedded-prefix.fbx");
        } catch (const FbxError&) {
            rejected = true;
        }
        require(rejected, "ASCII embedded Content prefix truncation accepted");
    }
}

void rejectsMalformedAsciiNumericArrays() {
    const auto truncated = asciiArrayTriangle().substr(0, asciiArrayTriangle().find("\n  }\n  PolygonVertexIndex"));
    expectsFbxError([&] {
        (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(truncated.data()), truncated.size()});
    }, "truncated", FbxStage::ascii_dom);

    const auto countMismatch = asciiArrayTriangle("4");
    expectsFbxError([&] {
        (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(countMismatch.data()), countMismatch.size()});
    }, "array_size", FbxStage::ascii_dom);

    const auto tooMany = asciiArrayTriangle("8");
    expectsFbxError([&] {
        (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(tooMany.data()), tooMany.size()});
    }, "array_size", FbxStage::ascii_dom);

    const auto nonFinite = asciiArrayTriangle("9", "0,0,NaN,1,0,0,0,1,0");
    expectsFbxError([&] {
        (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(nonFinite.data()), nonFinite.size()});
    }, "non_finite", FbxStage::ascii_dom);

    const auto signedNonFinite = asciiArrayTriangle("9", "0,0,+inf,1,0,0,0,1,0");
    expectsFbxError([&] {
        (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(signedNonFinite.data()), signedNonFinite.size()});
    }, "non_finite", FbxStage::ascii_dom);

    const auto nonNumeric = asciiArrayTriangle("9", "0,0,not-a-number,1,0,0,0,1,0");
    expectsFbxError([&] {
        (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(nonNumeric.data()), nonNumeric.size()});
    }, "array_type", FbxStage::ascii_dom);

    auto elementLimits = apex::formats::FbxLimits{};
    elementLimits.maxArrayElements = 8u;
    const auto valid = asciiArrayTriangle();
    expectsFbxError([&] {
        (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(valid.data()), valid.size()},
                                      "ascii-array.fbx", elementLimits);
    }, "array_limit", FbxStage::ascii_dom);

    auto byteLimits = apex::formats::FbxLimits{};
    byteLimits.maxArrayBytes = 64u;
    expectsFbxError([&] {
        (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(valid.data()), valid.size()},
                                      "ascii-array.fbx", byteLimits);
    }, "array_limit", FbxStage::ascii_dom);

    auto aggregateArrayLimits = apex::formats::FbxLimits{};
    aggregateArrayLimits.maxArrayBytes = 80u;
    expectsFbxError([&] {
        (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(valid.data()), valid.size()},
                                      "ascii-array.fbx", aggregateArrayLimits);
    }, "array_limit", FbxStage::ascii_dom);
}

void validatesBinaryBoundaryAndFooterRules() {
    const auto leaf = binaryLeafWithoutNodeTerminator();
    const auto leafDocument = apex::formats::parseFbx(leaf, "leaf.fbx");
    require(leafDocument.roots.size() == 1u && leafDocument.roots[0].name == "Root" &&
                std::get<std::int64_t>(leafDocument.roots[0].properties[0].values[0]) == 42,
            "32-bit binary leaf nodes may omit their node terminator");
    for (std::size_t length = 0; length < leaf.size(); ++length) {
        const auto prefix = std::span<const std::uint8_t>(leaf.data(), length);
        bool rejected = false;
        try { (void)apex::formats::parseFbx(prefix, "leaf-prefix.fbx"); }
        catch (const FbxError&) { rejected = true; }
        require(rejected, "every truncated leaf-node prefix is rejected");
    }
    auto missing = binaryMinimal();
    missing.resize(missing.size() - 13u);
    expectsFbxError([&] { (void)apex::formats::parseFbx(missing); }, "root_terminator", FbxStage::binary_dom);

    auto shortFooter = binaryMinimal();
    shortFooter.push_back(0u);
    expectsFbxError([&] { (void)apex::formats::parseFbx(shortFooter); }, "footer", FbxStage::binary_dom);

    auto footer = binaryMinimal();
    footer.insert(footer.end(), 16u, 0xa5u);
    const auto document = apex::formats::parseFbx(footer);
    require(document.trailingBytes == 16u, "opaque FBX footer is reported");

    auto limited = apex::formats::FbxLimits{};
    limited.maxFooterBytes = 8u;
    expectsFbxError([&] { (void)apex::formats::parseFbx(footer, "footer.fbx", limited); }, "footer_limit", FbxStage::binary_dom);
}

void rejectsNonFiniteAndPreservesSignedBits() {
    auto scalar = binaryMinimal(7400u, true);
    scalar[44u] = static_cast<std::uint8_t>('F');
    scalar[45u] = 0u; scalar[46u] = 0u; scalar[47u] = 0xc0u; scalar[48u] = 0x7fu;
    expectsFbxError([&] { (void)apex::formats::parseFbx(scalar); }, "non_finite", FbxStage::binary_dom);

    auto signedValue = binaryMinimal(7400u, true);
    signedValue[45u] = 0xffu; signedValue[46u] = 0xffu; signedValue[47u] = 0xffu; signedValue[48u] = 0xffu;
    const auto signedDocument = apex::formats::parseFbx(signedValue);
    require(std::get<std::int64_t>(signedDocument.roots[0].properties[0].values[0]) == -1,
            "signed FBX scalar preserves its bit pattern");

    auto array = binaryFloatArray(false);
    // Array payload begins after the binary header, node header, name, type,
    // count, encoding, and payload-size fields.
    array[57u] = 0u; array[58u] = 0u; array[59u] = 0xc0u; array[60u] = 0x7fu;
    expectsFbxError([&] { (void)apex::formats::parseFbx(array); }, "non_finite", FbxStage::binary_dom);

    const std::string ascii = "FBXVersion: 7400\nObjects: {\n Model: NaN {\n }\n}\n";
    expectsFbxError([&] { (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(ascii.data()), ascii.size()}); },
                    "non_finite", FbxStage::ascii_dom);
}

void validatesAsciiLexicalSemanticsAndConversionStage() {
    const std::string escaped = "FBXVersion: 7400\nObjects: {\n Model: \"A\\u0042\\n\" {\n }\n}\n";
    const auto document = apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(escaped.data()), escaped.size()});
    require(document.roots[1].children[0].properties[0].values.size() == 1u &&
                std::get<std::string>(document.roots[1].children[0].properties[0].values[0]) == "AB\n",
            "FBX ASCII escapes are decoded strictly");

    const std::string badEscape = "FBXVersion: 7400\nObjects: {\n Model: \"A\\q\" { }\n}\n";
    expectsFbxError([&] { (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(badEscape.data()), badEscape.size()}); },
                    "escape", FbxStage::ascii_tokens);
    const std::string control = "FBXVersion: 7400\nObjects: {\n Model: 1\x01 { }\n}\n";
    expectsFbxError([&] { (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(control.data()), control.size()}); },
                    "control", FbxStage::ascii_tokens);

    const auto capability = apex::formats::fbxConversionCapability();
    require(capability.domSupported && !capability.sceneSupported && capability.stage == FbxStage::conversion &&
                capability.code == "CONVERSION_UNIMPLEMENTED", "FBX conversion capability is explicit");
}

void validatesWrappedDeflateIntegrity() {
    auto badChecksum = binaryFloatArray(true);
    badChecksum.back() = badChecksum.back() == 0u ? 1u : static_cast<std::uint8_t>(badChecksum.back() ^ 1u);
    // The final byte belongs to the root terminator, so locate the zlib Adler trailer.
    const auto zlibBegin = std::find(badChecksum.begin(), badChecksum.end(), static_cast<std::uint8_t>(0x78u));
    require(zlibBegin != badChecksum.end(), "zlib fixture header");
    const auto trailer = static_cast<std::size_t>(zlibBegin - badChecksum.begin()) + 18u;
    badChecksum[trailer] ^= 1u;
    expectsFbxError([&] { (void)apex::formats::parseFbx(badChecksum); }, "compression", FbxStage::binary_dom);

    auto badHeader = binaryFloatArray(true);
    const auto header = std::find(badHeader.begin(), badHeader.end(), static_cast<std::uint8_t>(0x78u));
    require(header != badHeader.end(), "zlib header fixture");
    badHeader[static_cast<std::size_t>(header - badHeader.begin()) + 1u] ^= 1u;
    expectsFbxError([&] { (void)apex::formats::parseFbx(badHeader); }, "compression", FbxStage::binary_dom);
}

void rejectsEveryBinaryPrefix() {
    const auto valid = binaryMinimal(7600u, true);
    for (std::size_t length = 0; length < valid.size(); ++length) {
        std::vector<std::uint8_t> prefix(valid.begin(), valid.begin()+static_cast<std::ptrdiff_t>(length));
        bool rejected = false;
        try { (void)apex::formats::parseFbx(prefix); }
        catch (const FbxError&) { rejected = true; }
        require(rejected, "binary prefix truncation accepted");
    }
}

void rejectsEveryAsciiPrefix() {
    const auto text = asciiMinimal();
    const auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    for (std::size_t length = 0; length + 1u < bytes.size(); ++length) {
        bool rejected = false;
        try { (void)apex::formats::parseFbx(bytes.first(length)); }
        catch (const FbxError&) { rejected = true; }
        require(rejected, "ASCII prefix truncation accepted");
    }
}

void rejectsEveryAsciiArrayPrefix() {
    const auto text = asciiArrayTriangle();
    const auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    for (std::size_t length = 0; length + 1u < bytes.size(); ++length) {
        bool rejected = false;
        try { (void)apex::formats::parseFbx(bytes.first(length)); }
        catch (const FbxError&) { rejected = true; }
        require(rejected, "ASCII array prefix truncation accepted");
    }
}

void rejectsMaliciousStructureAndLimits() {
    auto offset = binaryMinimal();
    offset[27] = 0xffu; offset[28] = 0xffu; offset[29] = 0xffu; offset[30] = 0x7fu;
    expectsFbxError([&] { (void)apex::formats::parseFbx(offset); }, "offset", FbxStage::binary_dom);
    auto properties = binaryMinimal(7400u);
    properties[31] = 0xffu; properties[32] = 0xffu; properties[33] = 0xffu; properties[34] = 0x7fu;
    expectsFbxError([&] { (void)apex::formats::parseFbx(properties); }, "property_limit", FbxStage::binary_dom);
    auto depth = binaryMinimal();
    apex::formats::FbxLimits limits; limits.maxDepth = 0;
    expectsFbxError([&] { (void)apex::formats::parseFbx(depth, "depth.fbx", limits); }, "depth_limit", FbxStage::binary_dom);
    auto unsupported = binaryMinimal(9000u);
    expectsFbxError([&] { (void)apex::formats::parseFbx(unsupported); }, "unsupported_version", FbxStage::header);
    auto ascii = asciiMinimal(); apex::formats::FbxLimits asciiLimits; asciiLimits.maxAsciiTokenBytes = 4;
    expectsFbxError([&] { (void)apex::formats::parseFbx({reinterpret_cast<const std::uint8_t*>(ascii.data()), ascii.size()}, "ascii.fbx", asciiLimits); }, "token_limit", FbxStage::ascii_tokens);
    auto aggregateLimits = apex::formats::FbxLimits{}; aggregateLimits.maxAggregateBytes = 2u;
    expectsFbxError([&] { (void)apex::formats::parseFbx(binaryMinimal(), "aggregate.fbx", aggregateLimits); }, "allocation_limit", FbxStage::binary_dom);
    auto compressed = binaryFloatArray(true); compressed[54] = 0xffu; compressed[55] = 0xffu;
    expectsFbxError([&] { (void)apex::formats::parseFbx(compressed); }, "truncated", FbxStage::binary_dom);
}

} // namespace

int main() {
    try {
        inspectsHeadersExactly();
        parsesBinaryAndAsciiDom();
        parsesBoundedAsciiNumericArrays();
        parsesBoundedEmbeddedContent();
        rejectsMalformedEmbeddedContent();
        rejectsMalformedAsciiNumericArrays();
        validatesBinaryBoundaryAndFooterRules();
        rejectsNonFiniteAndPreservesSignedBits();
        validatesAsciiLexicalSemanticsAndConversionStage();
        validatesWrappedDeflateIntegrity();
        rejectsEveryBinaryPrefix();
        rejectsEveryAsciiPrefix();
        rejectsEveryAsciiArrayPrefix();
        rejectsMaliciousStructureAndLimits();
        std::cout << "fbx tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fbx tests failed: " << error.what() << '\n';
        return 1;
    }
}
