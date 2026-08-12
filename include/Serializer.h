#ifndef MYRPCPROJECT_INCLUDE_SERIALIZER_H_
#define MYRPCPROJECT_INCLUDE_SERIALIZER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// Self-implemented, tag-based binary serializer with zero third-party
// dependencies (see docs/v2-design-draft.md section 1.2.2).
//
// The format is fully self-describing (no IDL / schema needed to decode):
//
//   struct := field*
//   field  := varint tag = (field_number << 3) | 2     (protobuf-style tag)
//             u8 value_type                            (Value::Type, see below)
//             payload                                  (encoded by value_type)
//
//   payload:
//     Null                  (empty)
//     Bool                  u8 (0/1)
//     Int64                 varint zigzag
//     Uint64                varint
//     Double                8 bytes little-endian
//     String                varint len + bytes
//     Array                 varint len + (value_type + payload)*
//     Map                   varint len + (value_type + payload of key, value)*
//     Struct                varint len + field*
//
// Compatibility rules:
// - fields added by a newer protocol version decode into the dynamic struct
//   without breaking older readers (they only look up known field numbers);
// - field numbers are immutable once released; deleting a field means
//   reserving its number forever;
// - numeric defaults are all zero.

class Value {
public:
    using Array = std::vector<Value>;
    // Recursive payloads (struct fields, map entries) are held through
    // shared_ptr so that Value can reference itself before being complete
    // (shared_ptr's deleter is captured at construction, so it is copyable and
    // destructible even while Value is incomplete).
    struct Field {
        uint32_t id{0};
        std::shared_ptr<Value> v;
    };
    struct MapEntry {
        std::string key;
        std::shared_ptr<Value> v;
    };
    using Map = std::vector<MapEntry>;
    using Struct = std::vector<Field>;

    enum class Type { Null, Bool, Int64, Uint64, Double, String, Array, Map, Struct };

    Value() = default;

    Type type() const { return static_cast<Type>(data_.index()); }

    static Value makeNull() { return Value(); }
    static Value makeBool(bool b) {
        Value v;
        v.data_ = b;
        return v;
    }
    static Value makeInt(int64_t i) {
        Value v;
        v.data_ = i;
        return v;
    }
    static Value makeUint(uint64_t u) {
        Value v;
        v.data_ = u;
        return v;
    }
    static Value makeDouble(double d) {
        Value v;
        v.data_ = d;
        return v;
    }
    static Value makeString(std::string s) {
        Value v;
        v.data_ = std::move(s);
        return v;
    }
    static Value makeArray(Array a) {
        Value v;
        v.data_ = std::move(a);
        return v;
    }
    static Value makeMap(Map m) {
        Value v;
        v.data_ = std::move(m);
        return v;
    }
    static Value makeStruct(Struct s) {
        Value v;
        v.data_ = std::move(s);
        return v;
    }

    bool asBool() const { return std::get<bool>(data_); }
    int64_t asInt64() const { return std::get<int64_t>(data_); }
    uint64_t asUint64() const { return std::get<uint64_t>(data_); }
    double asDouble() const { return std::get<double>(data_); }
    const std::string& asString() const { return std::get<std::string>(data_); }
    const Array& asArray() const { return std::get<Array>(data_); }
    const Map& asMap() const { return std::get<Map>(data_); }
    const Struct& asStruct() const { return std::get<Struct>(data_); }

    // Look up a struct field by id; returns nullptr when absent.
    const Value* find(uint32_t fieldId) const {
        if (type() != Type::Struct) {
            return nullptr;
        }
        for (const auto& f : asStruct()) {
            if (f.id == fieldId) {
                return f.v.get();
            }
        }
        return nullptr;
    }

private:
    std::variant<std::monostate, bool, int64_t, uint64_t, double, std::string, Array, Map, Struct> data_;
};

class Serializer {
public:
    // Encode a top-level struct value into bytes.
    static std::string encode(const Value& v);

    // Decode bytes into a struct value. Returns false on malformed input; the
    // content of *out is unspecified on failure.
    static bool decode(const std::string& data, Value* out);

    // Human-readable debug view, used for logging and troubleshooting. Not a
    // wire format (see docs/competitive-analysis.md section 5, json2pb idea).
    static std::string toJson(const Value& v);
};

#endif  // MYRPCPROJECT_INCLUDE_SERIALIZER_H_
