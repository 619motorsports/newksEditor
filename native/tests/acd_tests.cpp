#include "apex/formats/acd.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using apex::core::ParseError;
using apex::core::ParseLimits;
using apex::formats::AcdArchive;
using apex::formats::AcdKey;
using apex::formats::createAcdKey;
using apex::formats::findAcdEntry;
using apex::formats::parseAcd;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void appendI32(std::vector<std::uint8_t>& bytes, std::int32_t value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    bytes.push_back(static_cast<std::uint8_t>(bits));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 8u));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 16u));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 24u));
}

using Record = std::pair<std::string, std::vector<std::uint8_t>>;

std::vector<std::uint8_t> pack(std::string_view assetName, const std::vector<Record>& records,
                               std::optional<std::int32_t> header = std::nullopt) {
    const AcdKey key = createAcdKey(assetName);
    std::vector<std::uint8_t> bytes;
    if (header.has_value()) {
        appendI32(bytes, -1111);
        appendI32(bytes, *header);
    }
    for (const auto& [name, data] : records) {
        appendI32(bytes, static_cast<std::int32_t>(name.size()));
        bytes.insert(bytes.end(), name.begin(), name.end());
        appendI32(bytes, static_cast<std::int32_t>(data.size()));
        for (std::size_t index = 0; index < data.size(); ++index) {
            const auto passwordByte = static_cast<unsigned char>(key.password[index % key.password.size()]);
            bytes.push_back(static_cast<std::uint8_t>(data[index] + passwordByte));
            bytes.push_back(0);
            bytes.push_back(0);
            bytes.push_back(0);
        }
    }
    return bytes;
}

template <typename Function>
void expectsParseError(Function&& function, std::string_view code = {}) {
    try {
        function();
    } catch (const ParseError& error) {
        if (!code.empty()) require(error.code() == code, "unexpected ACD error code");
        require(error.format() == "ACD", "error format attribution");
        require(error.offset() <= 0x1000000, "error offset is unreasonable");
        return;
    }
    throw std::runtime_error("malformed ACD input was accepted");
}

void derivesTheReferencePassword() {
    const auto key = createAcdKey(" KS_AUDI_SPORT_QUATTRO ");
    require(key.assetName == "ks_audi_sport_quattro", "trimmed lower-case asset name");
    require(key.octets == std::array<std::uint8_t, 8>{230, 190, 101, 8, 154, 33, 64, 112},
            "C# int32 key derivation");
    require(key.password == "230-190-101-8-154-33-64-112", "decimal password");
}

void readsHeaderedAndUnheaderedRecords() {
    for (const auto header : {std::optional<std::int32_t>{}, std::optional<std::int32_t>{243591}}) {
        const auto bytes = pack("example_car", {{"car.ini", {'o', 'k'}}, {"sub/file.bin", {0, 255, 17}}}, header);
        const AcdArchive archive = parseAcd(bytes, "example_car", "fixture/data.acd");
        require(archive.header == header, "optional ACD header");
        require(archive.entries.size() == 2, "record count");
        require(archive.bytesRead == archive.byteLength && archive.bytesRead == bytes.size(), "bytes read");
        const auto* ini = findAcdEntry(archive, "CAR.INI");
        const auto* binary = findAcdEntry(archive, "sub\\file.bin");
        require(ini != nullptr && ini->bytes().size() == 2 && ini->bytes()[0] == 'o', "virtual text entry");
        require(binary != nullptr && binary->bytes().size() == 3 && binary->bytes()[1] == 255,
                "virtual binary entry");
        require(ini->recordOffset == (header.has_value() ? 8u : 0u), "record offset");
        require(ini->payloadOffset > ini->recordOffset, "payload offset");
    }
}

void diagnosesUnsafeAndDuplicatePaths() {
    const auto bytes = pack("example_car", {{"../escape.ini", {'b'}}, {"Car.ini", {'f'}},
                                             {"car.ini", {'s'}}, {"folder//file", {'x'}},
                                             {"C:/absolute.ini", {'x'}}, {"/root.ini", {'x'}},
                                             {"./dot.ini", {'x'}}});
    const auto archive = parseAcd(bytes, "example_car", "unsafe.acd");
    require(archive.entries.size() == 7, "all records retained for diagnostics");
    require(archive.byPath.size() == 1, "only safe first path indexed");
    require(findAcdEntry(archive, "car.ini") != nullptr && findAcdEntry(archive, "car.ini")->data[0] == 'f',
            "first duplicate wins");
    require(archive.warnings.size() == 6, "unsafe and duplicate warnings");
    require(archive.warnings[0].find("unsafe archive path") != std::string::npos, "unsafe warning");
    require(archive.warnings[1].find("duplicate archive path") != std::string::npos, "duplicate warning");
    for (const auto& entry : archive.entries)
        if (!entry.safe) require(entry.bytes().data() != nullptr || entry.bytes().empty(), "entry bytes span");
}

void rejectsEveryTruncatedPrefix() {
    const auto bytes = pack("example_car", {{"car.ini", {'v', 'a', 'l', 'u', 'e'}},
                                             {"sub/file.bin", {0, 255, 17}}}, 243591);
    const auto complete = parseAcd(bytes, "example_car", "prefix.acd");
    require(complete.entries.size() == 2, "prefix fixture record count");
    const auto firstRecordEnd = complete.entries[1].recordOffset;
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        const std::vector<std::uint8_t> prefix(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
        try {
            const auto parsed = parseAcd(prefix, "example_car", "prefix.acd");
            // ACD has no record count, so a prefix ending exactly between
            // records is itself a valid shorter archive.  Every other prefix
            // must fail at the touched variable-length boundary.
            require(length == firstRecordEnd, "only complete record boundary may parse");
            require(parsed.entries.size() == 1, "complete prefix record count");
        } catch (const ParseError&) {
            continue;
        }
    }
    require(parseAcd(bytes, "example_car").bytesRead == bytes.size(), "complete archive prefix proof");
}

void rejectsMalformedLengthsAndLimits() {
    std::vector<std::uint8_t> negativeName;
    appendI32(negativeName, -1);
    expectsParseError([&] { (void)parseAcd(negativeName, "example_car"); }, "NEGATIVE_LENGTH");

    std::vector<std::uint8_t> negativePayload;
    appendI32(negativePayload, 1);
    negativePayload.push_back('x');
    appendI32(negativePayload, -1);
    expectsParseError([&] { (void)parseAcd(negativePayload, "example_car"); }, "NEGATIVE_LENGTH");

    std::vector<std::uint8_t> invalidUtf8;
    appendI32(invalidUtf8, 1);
    invalidUtf8.push_back(0xff);
    appendI32(invalidUtf8, 0);
    expectsParseError([&] { (void)parseAcd(invalidUtf8, "example_car"); }, "INVALID_UTF8");

    auto valid = pack("example_car", {{"value", {1, 2, 3}}});
    ParseLimits outputLimit;
    outputLimit.maxOutputBytes = 2;
    expectsParseError([&] { (void)parseAcd(valid, "example_car", "limit.acd", outputLimit); }, "OUTPUT_TOO_LARGE");

    ParseLimits entryLimit;
    entryLimit.maxTracks = 1;
    const auto twoEntries = pack("example_car", {{"one", {1}}, {"two", {2}}});
    expectsParseError([&] { (void)parseAcd(twoEntries, "example_car", "count.acd", entryLimit); }, "COUNT_LIMIT");

    ParseLimits inputLimit;
    inputLimit.maxInputBytes = valid.size() - 1;
    expectsParseError([&] { (void)parseAcd(valid, "example_car", "input.acd", inputLimit); }, "INPUT_TOO_LARGE");
}

void rejectsImpossibleDecryptions() {
    const std::vector<std::uint8_t> stored = {1, 0, 0, 0};
    expectsParseError([&] {
        (void)apex::formats::decryptAcdPayload(stored, 2, "key", {}, "payload.acd", 9);
    }, "TRUNCATED");
    expectsParseError([&] {
        (void)apex::formats::decryptAcdPayload(stored, 1, "", {}, "payload.acd", 9);
    }, "INVALID_PASSWORD");
    ParseLimits limit;
    limit.maxOutputBytes = 0;
    expectsParseError([&] {
        (void)apex::formats::decryptAcdPayload(stored, 1, "key", limit, "payload.acd", 9);
    }, "OUTPUT_TOO_LARGE");
}

} // namespace

int main() {
    try {
        derivesTheReferencePassword();
        readsHeaderedAndUnheaderedRecords();
        diagnosesUnsafeAndDuplicatePaths();
        rejectsEveryTruncatedPrefix();
        rejectsMalformedLengthsAndLimits();
        rejectsImpossibleDecryptions();
        std::cout << "ACD tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ACD tests failed: " << error.what() << '\n';
        return 1;
    }
}
