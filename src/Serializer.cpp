#include "Serializer.h"

#include <cstdio>
#include <cstring>

namespace {

// A cursor over the input bytes with bounds checks on every read.
struct Reader {
    const uint8_t* p;
    const uint8_t* end;

    explicit Reader(const std::string& data)
        : p(reinterpret_cast<const uint8_t*>(data.data())), end(p + data.size()) {}

    std::size_t remaining() const { return static_cast<std::size_t>(end - p); }
    bool readByte(uint8_t* out) {
        if (p >= end) {
            return false;
        }
        *out = *p++;
        return true;
    }
    bool readBytes(std::size_t len, const uint8_t** out) {
        if (remaining() < len) {
            return false;
        }
        *out = p;
        p += len;
        return true;
    }
    bool readVarint(uint64_t* out) {
        uint64_t value = 0;
        int shift = 0;
        for (int i = 0; i < 10; ++i) {
            uint8_t b = 0;
            if (!readByte(&b)) {
                return false;
            }
            value |= static_cast<uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) {
                *out = value;
                return true;
            }
            shift += 7;
        }
        return false;  // malformed: more than 10 bytes
    }
    bool readLengthDelimited(std::string* out) {
        uint64_t len = 0;
        if (!readVarint(&len) || len > remaining()) {
            return false;
        }
        const uint8_t* ptr = nullptr;
        if (!readBytes(static_cast<std::size_t>(len), &ptr)) {
            return false;
        }
        out->assign(reinterpret_cast<const char*>(ptr), static_cast<std::size_t>(len));
        return true;
    }
};

void writeVarint(std::string* out, uint64_t value) {
    while (value >= 0x80) {
        out->push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out->push_back(static_cast<char>(value));
}

uint64_t zigzagEncode(int64_t v) {
    return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}

int64_t zigzagDecode(uint64_t v) {
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
}

void writeStructFields(std::string* out, const Value::Struct& fields);
bool readStructFields(Reader* r, Value::Struct* out);

// value_type byte + payload; used for struct fields and array/map elements.
void writeTyped(std::string* out, const Value& v) {
    out->push_back(static_cast<char>(v.type()));
    switch (v.type()) {
        case Value::Type::Null:
            break;
        case Value::Type::Bool:
            out->push_back(v.asBool() ? 1 : 0);
            break;
        case Value::Type::Int64:
            writeVarint(out, zigzagEncode(v.asInt64()));
            break;
        case Value::Type::Uint64:
            writeVarint(out, v.asUint64());
            break;
        case Value::Type::Double: {
            double d = v.asDouble();
            uint64_t bits = 0;
            std::memcpy(&bits, &d, sizeof(bits));
            for (int i = 0; i < 8; ++i) {
                out->push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
            }
            break;
        }
        case Value::Type::String: {
            const std::string& s = v.asString();
            writeVarint(out, s.size());
            out->append(s);
            break;
        }
        case Value::Type::Array: {
            std::string payload;
            for (const auto& e : v.asArray()) {
                writeTyped(&payload, e);
            }
            writeVarint(out, payload.size());
            out->append(payload);
            break;
        }
        case Value::Type::Map: {
            std::string payload;
            for (const auto& kv : v.asMap()) {
                writeTyped(&payload, Value::makeString(kv.key));
                writeTyped(&payload, *kv.v);
            }
            writeVarint(out, payload.size());
            out->append(payload);
            break;
        }
        case Value::Type::Struct: {
            std::string payload;
            writeStructFields(&payload, v.asStruct());
            writeVarint(out, payload.size());
            out->append(payload);
            break;
        }
    }
}

bool readTyped(Reader* r, Value* out) {
    uint8_t type = 0;
    if (!r->readByte(&type)) {
        return false;
    }
    switch (static_cast<Value::Type>(type)) {
        case Value::Type::Null:
            *out = Value::makeNull();
            return true;
        case Value::Type::Bool: {
            uint8_t b = 0;
            if (!r->readByte(&b)) {
                return false;
            }
            *out = Value::makeBool(b != 0);
            return true;
        }
        case Value::Type::Int64: {
            uint64_t raw = 0;
            if (!r->readVarint(&raw)) {
                return false;
            }
            *out = Value::makeInt(zigzagDecode(raw));
            return true;
        }
        case Value::Type::Uint64: {
            uint64_t raw = 0;
            if (!r->readVarint(&raw)) {
                return false;
            }
            *out = Value::makeUint(raw);
            return true;
        }
        case Value::Type::Double: {
            const uint8_t* ptr = nullptr;
            if (!r->readBytes(8, &ptr)) {
                return false;
            }
            uint64_t bits = 0;
            std::memcpy(&bits, ptr, 8);
            double d = 0;
            std::memcpy(&d, &bits, 8);
            *out = Value::makeDouble(d);
            return true;
        }
        case Value::Type::String: {
            std::string s;
            if (!r->readLengthDelimited(&s)) {
                return false;
            }
            *out = Value::makeString(std::move(s));
            return true;
        }
        case Value::Type::Array: {
            std::string payload;
            if (!r->readLengthDelimited(&payload)) {
                return false;
            }
            Reader sub(payload);
            Value::Array arr;
            while (sub.remaining() > 0) {
                Value e;
                if (!readTyped(&sub, &e)) {
                    return false;
                }
                arr.push_back(std::move(e));
            }
            *out = Value::makeArray(std::move(arr));
            return true;
        }
        case Value::Type::Map: {
            std::string payload;
            if (!r->readLengthDelimited(&payload)) {
                return false;
            }
            Reader sub(payload);
            Value::Map map;
            while (sub.remaining() > 0) {
                Value key;
                Value val;
                if (!readTyped(&sub, &key) || !readTyped(&sub, &val)) {
                    return false;
                }
                if (key.type() != Value::Type::String) {
                    return false;  // map keys must be strings
                }
                Value::MapEntry entry;
                entry.key = key.asString();
                entry.v = std::make_shared<Value>(std::move(val));
                map.push_back(std::move(entry));
            }
            *out = Value::makeMap(std::move(map));
            return true;
        }
        case Value::Type::Struct: {
            std::string payload;
            if (!r->readLengthDelimited(&payload)) {
                return false;
            }
            Reader sub(payload);
            Value::Struct fields;
            if (!readStructFields(&sub, &fields)) {
                return false;
            }
            *out = Value::makeStruct(std::move(fields));
            return true;
        }
        default:
            return false;  // unknown value type
    }
}

// A struct field: varint tag = (field_number << 3) | 2, then a typed payload.
void writeStructFields(std::string* out, const Value::Struct& fields) {
    for (const auto& f : fields) {
        writeVarint(out, (static_cast<uint64_t>(f.id) << 3) | 2);
        writeTyped(out, *f.v);
    }
}

bool readStructFields(Reader* r, Value::Struct* out) {
    while (r->remaining() > 0) {
        uint64_t tag = 0;
        if (!r->readVarint(&tag)) {
            return false;
        }
        const uint32_t fieldId = static_cast<uint32_t>(tag >> 3);
        Value v;
        if (!readTyped(r, &v)) {
            return false;
        }
        // Forward compatibility: fields added by a newer version decode into
        // the dynamic struct without breaking older readers, which only look
        // up the field numbers they know about.
        out->push_back(Value::Field{fieldId, std::make_shared<Value>(std::move(v))});
    }
    return true;
}

}  // namespace

std::string Serializer::encode(const Value& v) {
    std::string out;
    if (v.type() == Value::Type::Struct) {
        writeStructFields(&out, v.asStruct());
    } else {
        // Non-struct top-level values are wrapped into a single field 1.
        Value::Struct fields;
        fields.push_back(Value::Field{1, std::make_shared<Value>(v)});
        writeStructFields(&out, fields);
    }
    return out;
}

bool Serializer::decode(const std::string& data, Value* out) {
    Reader r(data);
    Value::Struct fields;
    if (!readStructFields(&r, &fields)) {
        return false;
    }
    *out = Value::makeStruct(std::move(fields));
    return true;
}

std::string Serializer::toJson(const Value& v) {
    switch (v.type()) {
        case Value::Type::Null:
            return "null";
        case Value::Type::Bool:
            return v.asBool() ? "true" : "false";
        case Value::Type::Int64:
            return std::to_string(v.asInt64());
        case Value::Type::Uint64:
            return std::to_string(v.asUint64());
        case Value::Type::Double:
            return std::to_string(v.asDouble());
        case Value::Type::String: {
            std::string out = "\"";
            for (char c : v.asString()) {
                switch (c) {
                    case '"':
                        out += "\\\"";
                        break;
                    case '\\':
                        out += "\\\\";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else {
                            out += c;
                        }
                }
            }
            out += "\"";
            return out;
        }
        case Value::Type::Array: {
            std::string out = "[";
            bool first = true;
            for (const auto& e : v.asArray()) {
                if (!first) {
                    out += ",";
                }
                first = false;
                out += toJson(e);
            }
            out += "]";
            return out;
        }
        case Value::Type::Map: {
            std::string out = "{";
            bool first = true;
            for (const auto& kv : v.asMap()) {
                if (!first) {
                    out += ",";
                }
                first = false;
                out += toJson(Value::makeString(kv.key)) + ":" + toJson(*kv.v);
            }
            out += "}";
            return out;
        }
        case Value::Type::Struct: {
            std::string out = "{";
            bool first = true;
            for (const auto& f : v.asStruct()) {
                if (!first) {
                    out += ",";
                }
                first = false;
                out += "\"" + std::to_string(f.id) + "\":" + toJson(*f.v);
            }
            out += "}";
            return out;
        }
    }
    return "null";
}
