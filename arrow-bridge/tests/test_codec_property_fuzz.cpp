// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-11 §3e: runtime codec property + fuzz coverage.
//
// Two deterministic, bounded standing guards over the Phase-1-hardened decode
// path (Reader::Read/ReadBytes bounds checks, list/map count guards, nested
// struct/list/map decode, top-level full-buffer-consumption rejection):
//
//   * Property.EncodeDecodeRoundTrip — generates random VALID Arrow rows over a
//     fixed schema corpus (no unions; GIR-10 CodecEdge owns those) and asserts
//     encode -> decode -> Equals plus encode/decode/encode byte determinism.
//
//   * Fuzz.DecodeRowSurvivesRandomTruncatedBuffers — feeds DecodeRow random
//     garbage, truncated-at-every-length valid rows, bit-flipped / byte-
//     overwritten / count-length-corrupted / trailing-byte-appended valid rows.
//     The ONLY accepted outcomes are a successful decode (with the right field
//     count) or std::invalid_argument (the hardened reject signal). Any other
//     exception type fails the test; a crash/abort (e.g. from .ValueOrDie()) is
//     a real decode-robustness finding.
//
// Both tests use FIXED seeds and BOUNDED corpora — no unseeded/wall-clock
// randomness. Because std::uniform_int/real_distribution is not portable across
// standard-library implementations, a failure on one toolchain may not
// reproduce the identical RNG sequence elsewhere; every failure message
// therefore prints the offending buffer as hex so it can be replayed directly
// via DecodeRow(hex_buffer) across toolchains.
//
#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <fletcher/arrow_bridge/codec.hpp>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

// "GIR11_PR" and "GIR11_FZ" as little-endian ASCII — fixed, so both tests are
// fully deterministic run-to-run.
constexpr uint64_t kPropertySeed = 0x47495231315F5052ULL;
constexpr uint64_t kFuzzSeed = 0x47495231315F465AULL;

constexpr int kPropertyIterationsPerSchema = 128;

constexpr int kValidSeedsPerSchema = 32;
constexpr int kRandomGarbageCasesPerSchema = 256;
constexpr int kBitFlipCasesPerSchema = 256;

// Recursive container-generation depth cap (safety net; the corpus below never
// nests containers within containers, so this stays a no-op in practice).
constexpr int kMaxDepth = 3;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string HexDump(const std::vector<uint8_t>& buf) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(buf.size() * 2);
    for (uint8_t b : buf) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

// Fixed schema corpus. Only faithfully-round-trippable types — NO unions:
// GIR-10 CodecEdge.{Dense,Sparse}Union* own exhaustive union coverage, and a
// sparse-union scalar with non-null inactive children would false-fail Equals
// here (decode nulls inactive children). See the GIR-11 design doc.
std::vector<std::shared_ptr<arrow::Schema>> PropertySchemas() {
    return {
        arrow::schema({
            arrow::field("b", arrow::boolean(), true),
            arrow::field("i32", arrow::int32(), true),
            arrow::field("i64", arrow::int64(), true),
            arrow::field("u64", arrow::uint64(), true),
            arrow::field("f32", arrow::float32(), true),
            arrow::field("f64", arrow::float64(), true),
            arrow::field("s", arrow::utf8(), true),
            arrow::field("bin", arrow::binary(), true),
            arrow::field("date", arrow::date32(), true),
            arrow::field("ts", arrow::timestamp(arrow::TimeUnit::NANO), true),
            arrow::field("dur", arrow::duration(arrow::TimeUnit::MICRO), true),
            arrow::field("dec", arrow::decimal128(18, 4), true),
        }),
        arrow::schema({
            arrow::field("st",
                         arrow::struct_({
                             arrow::field("x", arrow::int32(), true),
                             arrow::field("name", arrow::utf8(), true),
                         }),
                         true),
            arrow::field("ints", arrow::list(arrow::int32()), true),
            arrow::field("strings", arrow::list(arrow::utf8()), true),
            arrow::field("fixed", arrow::fixed_size_list(arrow::float32(), 3), true),
            arrow::field("map", arrow::map(arrow::utf8(), arrow::int32()), true),
        }),
    };
}

// Deterministic UTF-8 tokens: empty string, ASCII, embedded NUL, and 2/3/4-byte
// code points. Concatenating whole tokens keeps every result valid UTF-8.
std::string RandomUtf8(std::mt19937_64& rng) {
    static const std::vector<std::string> kTokens = {
        "",
        "a",
        "abc",
        std::string("x\0y", 3),
        "\xC3\xA9",      // é
        "\xE2\x82\xAC",  // €
        "hello",
        "\xF0\x9F\x98\x80",  // 😀
    };
    int n = static_cast<int>(rng() % 4);  // 0..3 tokens (n == 0 -> empty string)
    std::string s;
    for (int i = 0; i < n; ++i) s += kTokens[rng() % kTokens.size()];
    return s;
}

std::string RandomBinary(std::mt19937_64& rng) {
    int len = static_cast<int>(rng() % 9);  // 0..8 bytes
    std::string s;
    s.reserve(static_cast<size_t>(len));
    for (int i = 0; i < len; ++i) s.push_back(static_cast<char>(rng() & 0xFF));
    return s;
}

std::shared_ptr<arrow::Scalar> RandomScalar(const std::shared_ptr<arrow::DataType>& type,
                                            std::mt19937_64& rng, int depth, bool allow_null);

// Build an element array for a list / fixed-size-list, applying per-element
// nullability recursively (null children exercise the element null bitfield).
std::shared_ptr<arrow::Array> RandomElementArray(const std::shared_ptr<arrow::DataType>& elem_type,
                                                 int count, std::mt19937_64& rng, int depth) {
    auto builder = arrow::MakeBuilder(elem_type).ValueOrDie();
    for (int i = 0; i < count; ++i) {
        auto child = RandomScalar(elem_type, rng, depth + 1, /*allow_null=*/true);
        arrow::Status st = child->is_valid ? builder->AppendScalar(*child) : builder->AppendNull();
        EXPECT_TRUE(st.ok()) << "RandomElementArray append failed: " << st.ToString();
    }
    return builder->Finish().ValueOrDie();
}

// Generate one random valid scalar of `type`. When `allow_null`, returns a null
// scalar ~20% of the time (applied recursively so nested struct/list/map values
// are also nulled, not just top-level fields).
std::shared_ptr<arrow::Scalar> RandomScalar(const std::shared_ptr<arrow::DataType>& type,
                                            std::mt19937_64& rng, int depth, bool allow_null) {
    using T = arrow::Type;

    if (allow_null && (rng() % 5 == 0)) return arrow::MakeNullScalar(type);

    switch (type->id()) {
        case T::BOOL:
            return std::make_shared<arrow::BooleanScalar>((rng() & 1u) != 0);
        case T::INT8:
            return std::make_shared<arrow::Int8Scalar>(static_cast<int8_t>(rng()));
        case T::INT16:
            return std::make_shared<arrow::Int16Scalar>(static_cast<int16_t>(rng()));
        case T::INT32:
            return std::make_shared<arrow::Int32Scalar>(static_cast<int32_t>(rng()));
        case T::INT64:
            return std::make_shared<arrow::Int64Scalar>(static_cast<int64_t>(rng()));
        case T::UINT8:
            return std::make_shared<arrow::UInt8Scalar>(static_cast<uint8_t>(rng()));
        case T::UINT16:
            return std::make_shared<arrow::UInt16Scalar>(static_cast<uint16_t>(rng()));
        case T::UINT32:
            return std::make_shared<arrow::UInt32Scalar>(static_cast<uint32_t>(rng()));
        case T::UINT64:
            return std::make_shared<arrow::UInt64Scalar>(static_cast<uint64_t>(rng()));
        case T::FLOAT: {
            // Finite only — Arrow Scalar::Equals is the value oracle here.
            // NaN/±Inf/-0.0 are covered bit-exactly by GIR-10.
            std::uniform_real_distribution<double> d(-1e12, 1e12);
            return std::make_shared<arrow::FloatScalar>(static_cast<float>(d(rng)));
        }
        case T::DOUBLE: {
            std::uniform_real_distribution<double> d(-1e12, 1e12);
            return std::make_shared<arrow::DoubleScalar>(d(rng));
        }
        case T::DATE32:
            return std::make_shared<arrow::Date32Scalar>(static_cast<int32_t>(rng()));
        case T::TIMESTAMP:
            return std::make_shared<arrow::TimestampScalar>(static_cast<int64_t>(rng()), type);
        case T::DURATION:
            return std::make_shared<arrow::DurationScalar>(static_cast<int64_t>(rng()), type);
        case T::DECIMAL128: {
            // Keep magnitude within precision 18 (|value| <= 10^18 - 1).
            const int64_t maxv = 999999999999999999LL;
            int64_t v = static_cast<int64_t>(rng() % (static_cast<uint64_t>(maxv) * 2 + 1)) - maxv;
            return std::make_shared<arrow::Decimal128Scalar>(arrow::Decimal128(v), type);
        }
        case T::STRING:
            return std::make_shared<arrow::StringScalar>(RandomUtf8(rng));
        case T::BINARY:
            return std::make_shared<arrow::BinaryScalar>(
                arrow::Buffer::FromString(RandomBinary(rng)));

        case T::STRUCT: {
            const auto& st = static_cast<const arrow::StructType&>(*type);
            arrow::ScalarVector children;
            children.reserve(static_cast<size_t>(st.num_fields()));
            for (int i = 0; i < st.num_fields(); ++i)
                children.push_back(
                    RandomScalar(st.field(i)->type(), rng, depth + 1, /*allow_null=*/true));
            return std::make_shared<arrow::StructScalar>(std::move(children), type);
        }
        case T::LIST: {
            const auto& lt = static_cast<const arrow::BaseListType&>(*type);
            int count = depth >= kMaxDepth ? 0 : static_cast<int>(rng() % 9);  // 0..8
            auto arr = RandomElementArray(lt.value_type(), count, rng, depth);
            return std::make_shared<arrow::ListScalar>(arr, type);
        }
        case T::FIXED_SIZE_LIST: {
            const auto& fsl = static_cast<const arrow::FixedSizeListType&>(*type);
            auto arr = RandomElementArray(fsl.value_type(), fsl.list_size(), rng, depth);
            return std::make_shared<arrow::FixedSizeListScalar>(arr, type);
        }
        case T::MAP: {
            const auto& mt = static_cast<const arrow::MapType&>(*type);
            int count = depth >= kMaxDepth ? 0 : static_cast<int>(rng() % 7);  // 0..6
            auto key_builder = arrow::MakeBuilder(mt.key_type()).ValueOrDie();
            auto item_builder = arrow::MakeBuilder(mt.item_type()).ValueOrDie();
            for (int i = 0; i < count; ++i) {
                // Map keys are never null.
                auto key = RandomScalar(mt.key_type(), rng, depth + 1, /*allow_null=*/false);
                arrow::Status kst = key_builder->AppendScalar(*key);
                EXPECT_TRUE(kst.ok()) << "map key append failed: " << kst.ToString();
                auto val = RandomScalar(mt.item_type(), rng, depth + 1, /*allow_null=*/true);
                arrow::Status vst =
                    val->is_valid ? item_builder->AppendScalar(*val) : item_builder->AppendNull();
                EXPECT_TRUE(vst.ok()) << "map value append failed: " << vst.ToString();
            }
            auto keys = key_builder->Finish().ValueOrDie();
            auto vals = item_builder->Finish().ValueOrDie();
            auto entries =
                arrow::StructArray::Make({keys, vals}, mt.value_type()->fields()).ValueOrDie();
            return std::make_shared<arrow::MapScalar>(entries, type);
        }
        default:
            ADD_FAILURE() << "RandomScalar: unhandled type " << type->ToString();
            return arrow::MakeNullScalar(type);
    }
}

fletcher::ArrowRow RandomRow(const std::shared_ptr<arrow::Schema>& schema, std::mt19937_64& rng) {
    fletcher::ArrowRow row;
    row.reserve(static_cast<size_t>(schema->num_fields()));
    for (int i = 0; i < schema->num_fields(); ++i)
        row.push_back(
            RandomScalar(schema->field(i)->type(), rng, /*depth=*/0, /*allow_null=*/true));
    return row;
}

}  // namespace

// ---------------------------------------------------------------------------
// Property: valid rows round-trip by value, and re-encode byte-identically.
// ---------------------------------------------------------------------------

TEST(Property, EncodeDecodeRoundTrip) {
    std::mt19937_64 rng(kPropertySeed);
    auto schemas = PropertySchemas();

    for (size_t s = 0; s < schemas.size(); ++s) {
        fletcher::Codec codec(schemas[s]);
        for (int it = 0; it < kPropertyIterationsPerSchema; ++it) {
            auto row = RandomRow(schemas[s], rng);
            auto encoded1 = codec.EncodeRow(row);

            std::ostringstream ctx;
            ctx << "seed=0x" << std::hex << kPropertySeed << std::dec << " schema=" << s
                << " iter=" << it << " schema_str=" << schemas[s]->ToString()
                << " encoded=" << HexDump(encoded1);
            SCOPED_TRACE(ctx.str());

            auto decoded = codec.DecodeRow(encoded1);
            ASSERT_EQ(decoded.size(), row.size());
            for (size_t i = 0; i < row.size(); ++i) {
                EXPECT_TRUE(decoded[i]->Equals(*row[i]))
                    << "field " << i << " (" << schemas[s]->field(static_cast<int>(i))->ToString()
                    << "): decoded=" << decoded[i]->ToString()
                    << " original=" << row[i]->ToString();
            }
            auto encoded2 = codec.EncodeRow(decoded);
            EXPECT_EQ(encoded1, encoded2) << "encode->decode->encode not byte-identical";
        }
    }
}

// ---------------------------------------------------------------------------
// Fuzz: malformed buffers must never crash; only success or invalid_argument.
// ---------------------------------------------------------------------------

TEST(Fuzz, DecodeRowSurvivesRandomTruncatedBuffers) {
    std::mt19937_64 rng(kFuzzSeed);
    auto schemas = PropertySchemas();

    for (size_t s = 0; s < schemas.size(); ++s) {
        fletcher::Codec codec(schemas[s]);
        const size_t num_fields = static_cast<size_t>(schemas[s]->num_fields());

        // Valid encoded seed rows from the same generator.
        std::vector<fletcher::EncodedRow> seeds;
        seeds.reserve(kValidSeedsPerSchema);
        for (int i = 0; i < kValidSeedsPerSchema; ++i)
            seeds.push_back(codec.EncodeRow(RandomRow(schemas[s], rng)));

        int case_index = 0;
        // Cross-toolchain repro context. The RNG is non-portable, so the hex
        // dump is the only key that replays identically elsewhere; attach it to
        // EVERY assertion (incl. the size-check) via SCOPED_TRACE.
        auto ctx = [&](const char* variant, const std::vector<uint8_t>& buf) {
            std::ostringstream os;
            os << "seed=0x" << std::hex << kFuzzSeed << std::dec << " schema=" << s
               << " variant=" << variant << " case=" << case_index << " buffer=" << HexDump(buf);
            return os.str();
        };

        // Malformed-input path: garbage / truncated / corrupted buffers. The
        // ONLY accepted outcomes are a successful decode (with num_fields
        // scalars) or std::invalid_argument (the hardened reject signal).
        auto attempt = [&](const std::vector<uint8_t>& buf, const char* variant) {
            SCOPED_TRACE(ctx(variant, buf));
            try {
                auto decoded = codec.DecodeRow(buf.data(), buf.size());
                // Success is allowed; a decode of arbitrary bytes must still
                // yield exactly one scalar per schema field.
                EXPECT_EQ(decoded.size(), num_fields) << "decoded field count mismatch";
            } catch (const std::invalid_argument&) {
                SUCCEED();  // the hardened reject signal
            } catch (const std::exception& e) {
                FAIL() << "DecodeRow threw non-invalid_argument exception: " << e.what();
            } catch (...) {
                FAIL() << "DecodeRow threw a non-std::exception type";
            }
            ++case_index;
        };

        // Exact-valid sanity path: a buffer straight from EncodeRow MUST decode
        // successfully into num_fields scalars. Unlike the malformed path, a
        // rejection here (std::invalid_argument or any other throw) is a codec
        // REGRESSION and fails the test — this is what proves the harness can
        // tell a genuine success from a reject.
        auto attempt_valid = [&](const std::vector<uint8_t>& buf) {
            SCOPED_TRACE(ctx("valid", buf));
            try {
                auto decoded = codec.DecodeRow(buf.data(), buf.size());
                EXPECT_EQ(decoded.size(), num_fields)
                    << "exact valid EncodeRow output must decode to num_fields scalars";
            } catch (const std::exception& e) {
                FAIL() << "exact valid buffer was REJECTED by DecodeRow (codec regression?): "
                       << e.what();
            } catch (...) {
                FAIL() << "exact valid buffer threw a non-std::exception type";
            }
            ++case_index;
        };

        // Strategy 1: random garbage (length 0..256, bytes 0..255).
        for (int i = 0; i < kRandomGarbageCasesPerSchema; ++i) {
            int len = static_cast<int>(rng() % 257);
            std::vector<uint8_t> buf(static_cast<size_t>(len));
            for (int j = 0; j < len; ++j)
                buf[static_cast<size_t>(j)] = static_cast<uint8_t>(rng() & 0xFF);
            attempt(buf, "garbage");
        }

        // Strategy 2: every truncated prefix of each valid row (permissive),
        // plus the exact valid row (strict — must decode successfully).
        for (const auto& seed : seeds) {
            for (size_t p = 0; p < seed.size(); ++p)
                attempt(std::vector<uint8_t>(seed.begin(), seed.begin() + static_cast<long>(p)),
                        "truncated");
            attempt_valid(seed);
        }

        // Strategy 3: corrupted valid rows — bit flips, byte overwrites,
        // multi-byte 0xFF count/length corruption, and appended trailing bytes.
        for (int i = 0; i < kBitFlipCasesPerSchema; ++i) {
            std::vector<uint8_t> buf = seeds[rng() % seeds.size()];
            int mode = static_cast<int>(rng() % 4);
            if (buf.empty()) mode = 3;  // only append is meaningful on empty
            switch (mode) {
                case 0: {  // single-bit flip
                    size_t bitpos = rng() % (buf.size() * 8);
                    buf[bitpos / 8] ^= static_cast<uint8_t>(1u << (bitpos % 8));
                    break;
                }
                case 1: {  // byte overwrite
                    buf[rng() % buf.size()] = static_cast<uint8_t>(rng() & 0xFF);
                    break;
                }
                case 2: {  // 0xFF..FF over a 4-byte window (LEN/COUNT corruption)
                    if (buf.size() >= 4) {
                        size_t pos = rng() % (buf.size() - 3);
                        buf[pos] = buf[pos + 1] = buf[pos + 2] = buf[pos + 3] = 0xFF;
                    } else {
                        for (auto& b : buf) b = 0xFF;
                    }
                    break;
                }
                case 3: {  // append trailing bytes (full-consumption rejection)
                    int extra = 1 + static_cast<int>(rng() % 4);
                    for (int k = 0; k < extra; ++k)
                        buf.push_back(static_cast<uint8_t>(rng() & 0xFF));
                    break;
                }
            }
            attempt(buf, "corrupted");
        }
    }
}
