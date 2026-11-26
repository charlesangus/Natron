// Writes Natron's Cycles shader manifest (Engine/Nodes/Scene/Shaders/manifest/cycles.json) from the
// NodeType registry of the bundled Cycles. The image builds Cycles only into the hdCycles Hydra
// plug-in, with its symbols hidden, so this tool loads that plug-in and resolves
// ccl::NodeType::type_names and ccl::NodeType::find from the library's ELF symbol table. The
// registry's structures are read through the headers of the same Cycles commit.
//
// Usage: dump_cycles_manifest --library <hdCycles.so> [--source <hdCycles/SOURCE>] [--output <file>]

#include "graph/node_enum.h"
#include "graph/node_type.h"

#include <OpenImageIO/ustring.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <link.h>
#include <map>
#include <new>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using ccl::NodeEnum;
using ccl::NodeType;
using ccl::SocketType;
using OIIO::ustring;

constexpr const char* kTypeNamesSymbol = "_ZN3ccl8NodeType10type_namesEv";
constexpr const char* kFindSymbol = "_ZN3ccl8NodeType4findEN11OpenImageIO4v3_17ustringE";

struct UstringVector {
    ustring* begin;
    ustring* end;
    ustring* capacity;
};
static_assert(sizeof(UstringVector) == sizeof(ccl::vector<ustring>));

struct RawArray {
    const unsigned char* data;
    size_t size;
    size_t capacity;
};
static_assert(sizeof(RawArray) == sizeof(ccl::array<float>));

constexpr size_t kFloat3Stride = 16;
constexpr size_t kFloat2Stride = 8;
constexpr size_t kTransformFloats = 12;

class SymbolTable {
public:
    explicit SymbolTable(const std::string& path)
    {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("cannot open " + path);
        }
        struct stat st {};
        ::fstat(fd, &st);
        size_ = static_cast<size_t>(st.st_size);
        data_ = static_cast<const unsigned char*>(::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0));
        ::close(fd);
        if (data_ == MAP_FAILED) {
            throw std::runtime_error("cannot map " + path);
        }
        const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data_);
        if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 || ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
            throw std::runtime_error(path + " is not a 64-bit ELF file");
        }
        const auto* shdrs = reinterpret_cast<const Elf64_Shdr*>(data_ + ehdr->e_shoff);
        for (int i = 0; i < ehdr->e_shnum; ++i) {
            if (shdrs[i].sh_type != SHT_SYMTAB) {
                continue;
            }
            const Elf64_Shdr& strtab = shdrs[shdrs[i].sh_link];
            const auto* syms = reinterpret_cast<const Elf64_Sym*>(data_ + shdrs[i].sh_offset);
            const size_t count = shdrs[i].sh_size / sizeof(Elf64_Sym);
            const char* names = reinterpret_cast<const char*>(data_ + strtab.sh_offset);
            for (size_t s = 0; s < count; ++s) {
                if (ELF64_ST_TYPE(syms[s].st_info) == STT_FUNC && syms[s].st_value != 0) {
                    symbols_.emplace(names + syms[s].st_name, syms[s].st_value);
                }
            }
        }
        if (symbols_.empty()) {
            throw std::runtime_error(path + " has no symbol table (stripped)");
        }
    }

    ~SymbolTable() { ::munmap(const_cast<unsigned char*>(data_), size_); }

    uint64_t find(const std::string& name) const
    {
        const auto it = symbols_.find(name);
        if (it == symbols_.end()) {
            throw std::runtime_error("symbol " + name + " not found");
        }
        return it->second;
    }

private:
    const unsigned char* data_ = nullptr;
    size_t size_ = 0;
    std::map<std::string, uint64_t> symbols_;
};

std::string
quoted(const std::string& s)
{
    std::string out = "\"";
    for (const unsigned char c : s) {
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
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

std::string
number(float v)
{
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    std::string s(buf, res.ptr);
    if (s.find_first_of(".eEn") == std::string::npos) {
        s += ".0";
    }
    const auto e = s.find('e');
    if (e != std::string::npos) {
        std::string mantissa = s.substr(0, e);
        std::string exponent = s.substr(e + 1);
        const bool negative = !exponent.empty() && exponent[0] == '-';
        if (negative || exponent[0] == '+') {
            exponent = exponent.substr(1);
        }
        while (exponent.size() < 2) {
            exponent = "0" + exponent;
        }
        s = mantissa + "e" + (negative ? "-" : "+") + exponent;
    }
    return s;
}

// A JSON value rendered in the layout of generate_moonray_manifest.py's format_json (Python's
// json.dumps(indent=2), with lists of scalars on one line), so both renderers' manifests read alike.
struct Json {
    enum Kind { Scalar,
                Object,
                Array };
    Kind kind = Scalar;
    std::string scalar;
    std::vector<std::pair<std::string, Json>> members;
    std::vector<Json> items;

    static Json raw(std::string s)
    {
        Json j;
        j.scalar = std::move(s);
        return j;
    }
    static Json str(const std::string& s) { return raw(quoted(s)); }
    static Json boolean(bool b) { return raw(b ? "true" : "false"); }
    static Json object()
    {
        Json j;
        j.kind = Object;
        return j;
    }
    static Json array()
    {
        Json j;
        j.kind = Array;
        return j;
    }

    Json& set(const std::string& key, Json value)
    {
        members.emplace_back(key, std::move(value));
        return *this;
    }
    Json& push(Json value)
    {
        items.push_back(std::move(value));
        return *this;
    }

    void write(std::ostream& os, int indent) const
    {
        const std::string pad(indent + 2, ' ');
        if (kind == Scalar) {
            os << scalar;
        } else if (kind == Object) {
            if (members.empty()) {
                os << "{}";
                return;
            }
            os << "{\n";
            for (size_t i = 0; i < members.size(); ++i) {
                os << pad << quoted(members[i].first) << ": ";
                members[i].second.write(os, indent + 2);
                os << (i + 1 < members.size() ? ",\n" : "\n");
            }
            os << std::string(indent, ' ') << "}";
        } else {
            if (items.empty()) {
                os << "[]";
                return;
            }
            if (std::all_of(items.begin(), items.end(), [](const Json& j) { return j.kind == Scalar; })) {
                os << "[";
                for (size_t i = 0; i < items.size(); ++i) {
                    os << (i ? ", " : "") << items[i].scalar;
                }
                os << "]";
                return;
            }
            os << "[\n";
            for (size_t i = 0; i < items.size(); ++i) {
                os << pad;
                items[i].write(os, indent + 2);
                os << (i + 1 < items.size() ? ",\n" : "\n");
            }
            os << std::string(indent, ' ') << "]";
        }
    }
};

Json
floats(const float* v, size_t n)
{
    Json out = Json::array();
    for (size_t i = 0; i < n; ++i) {
        out.push(Json::raw(number(v[i])));
    }
    return out;
}

const char*
typeName(SocketType::Type type)
{
    static const char* const names[] = {
        "undefined", "boolean", "float", "int", "uint", "uint64", "color", "vector", "point",
        "normal", "point2", "closure", "string", "enum", "transform", "node", "array_boolean",
        "array_float", "array_int", "array_color", "array_vector", "array_point", "array_normal",
        "array_point2", "array_string", "array_transform", "array_node"
    };
    static_assert(sizeof(names) / sizeof(names[0]) == SocketType::NUM_TYPES);
    return (type >= 0 && type < SocketType::NUM_TYPES) ? names[type] : "undefined";
}

bool
isArray(SocketType::Type type)
{
    return type >= SocketType::BOOLEAN_ARRAY && type < SocketType::NUM_TYPES;
}

Json
enumValues(const NodeEnum& values)
{
    std::vector<std::pair<int, std::string>> entries;
    for (auto it = values.begin(); it != values.end(); ++it) {
        entries.emplace_back(it->second, it->first.string());
    }
    std::sort(entries.begin(), entries.end());
    Json out = Json::array();
    for (const auto& [value, name] : entries) {
        out.push(Json::object().set("name", Json::str(name)).set("value", Json::raw(std::to_string(value))));
    }
    return out;
}

Json
arrayDefault(const SocketType& socket)
{
    const auto* arr = static_cast<const RawArray*>(socket.default_value);
    Json out = Json::array();
    for (size_t i = 0; i < arr->size; ++i) {
        switch (socket.type) {
        case SocketType::BOOLEAN_ARRAY:
            out.push(Json::boolean(reinterpret_cast<const bool*>(arr->data)[i]));
            break;
        case SocketType::FLOAT_ARRAY:
            out.push(Json::raw(number(reinterpret_cast<const float*>(arr->data)[i])));
            break;
        case SocketType::INT_ARRAY:
            out.push(Json::raw(std::to_string(reinterpret_cast<const int*>(arr->data)[i])));
            break;
        case SocketType::COLOR_ARRAY:
        case SocketType::VECTOR_ARRAY:
        case SocketType::POINT_ARRAY:
        case SocketType::NORMAL_ARRAY:
            out.push(floats(reinterpret_cast<const float*>(arr->data + i * kFloat3Stride), 3));
            break;
        case SocketType::POINT2_ARRAY:
            out.push(floats(reinterpret_cast<const float*>(arr->data + i * kFloat2Stride), 2));
            break;
        case SocketType::STRING_ARRAY:
            out.push(Json::str(reinterpret_cast<const ustring*>(arr->data)[i].string()));
            break;
        case SocketType::TRANSFORM_ARRAY:
            out.push(floats(reinterpret_cast<const float*>(arr->data) + i * kTransformFloats,
                            kTransformFloats));
            break;
        default:
            break;
        }
    }
    return out;
}

// Returns false when the socket holds no value (closures and node references).
bool
defaultValue(const SocketType& socket, Json& out)
{
    const void* v = socket.default_value;
    if (v == nullptr) {
        return false;
    }
    switch (socket.type) {
    case SocketType::BOOLEAN:
        out = Json::boolean(*static_cast<const bool*>(v));
        return true;
    case SocketType::FLOAT:
        out = Json::raw(number(*static_cast<const float*>(v)));
        return true;
    case SocketType::INT:
        out = Json::raw(std::to_string(*static_cast<const int*>(v)));
        return true;
    case SocketType::UINT:
        out = Json::raw(std::to_string(*static_cast<const unsigned*>(v)));
        return true;
    case SocketType::UINT64:
        out = Json::raw(std::to_string(*static_cast<const uint64_t*>(v)));
        return true;
    case SocketType::COLOR:
    case SocketType::VECTOR:
    case SocketType::POINT:
    case SocketType::NORMAL:
        out = floats(static_cast<const float*>(v), 3);
        return true;
    case SocketType::POINT2:
        out = floats(static_cast<const float*>(v), 2);
        return true;
    case SocketType::STRING:
        out = Json::str(static_cast<const ustring*>(v)->string());
        return true;
    case SocketType::ENUM: {
        const int value = *static_cast<const int*>(v);
        if (socket.enum_values != nullptr && socket.enum_values->exists(value)) {
            out = Json::str((*socket.enum_values)[value].string());
        } else {
            out = Json::raw(std::to_string(value));
        }
        return true;
    }
    case SocketType::TRANSFORM:
        out = floats(static_cast<const float*>(v), kTransformFloats);
        return true;
    case SocketType::CLOSURE:
    case SocketType::NODE:
    case SocketType::NODE_ARRAY:
    case SocketType::UNDEFINED:
    case SocketType::NUM_TYPES:
        return false;
    default:
        out = arrayDefault(socket);
        return true;
    }
}

const char*
defaultLink(int flags)
{
    static const std::pair<int, const char*> links[] = {
        { SocketType::LINK_TEXTURE_GENERATED, "texture_generated" },
        { SocketType::LINK_TEXTURE_NORMAL, "texture_normal" },
        { SocketType::LINK_TEXTURE_UV, "texture_uv" },
        { SocketType::LINK_TEXTURE_INCOMING, "texture_incoming" },
        { SocketType::LINK_INCOMING, "incoming" },
        { SocketType::LINK_NORMAL, "normal" },
        { SocketType::LINK_POSITION, "position" },
        { SocketType::LINK_TANGENT, "tangent" },
        { SocketType::LINK_OSL_INITIALIZER, "osl_initializer" },
    };
    for (const auto& [bit, name] : links) {
        if (flags & bit) {
            return name;
        }
    }
    return nullptr;
}

bool
hasClosureOutput(const NodeType& type)
{
    return std::any_of(type.outputs.begin(), type.outputs.end(),
                       [](const SocketType& s) { return s.type == SocketType::CLOSURE; });
}

bool
endsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string
kindOf(const std::string& name, const NodeType& type)
{
    if (name == "displacement" || name == "vector_displacement") {
        return "displacement";
    }
    if (hasClosureOutput(type)) {
        return "shader";
    }
    if (endsWith(name, "_texture")) {
        return "texture";
    }
    return "utility";
}

// Shader node types the registry holds that are not user nodes: the graph's own output node, the
// implicit type conversions it inserts, the mix weight its optimiser splits closures with, its
// internal proxy and bump helpers, and OSL script nodes.
bool
isUserNode(const std::string& name)
{
    if (name == "output" || name == "proxy" || name == "set_normal" || name == "mix_closure_weight") {
        return false;
    }
    if (name.rfind("convert_", 0) == 0 || name.rfind("osl", 0) == 0) {
        return false;
    }
    return true;
}

Json
nodeEntry(const std::string& name, const NodeType& type)
{
    Json attributes = Json::array();
    for (const SocketType& socket : type.inputs) {
        if (socket.flags & SocketType::INTERNAL) {
            continue;
        }
        const bool linkable = (socket.flags & SocketType::LINKABLE) != 0;
        const bool portOnly = socket.type == SocketType::CLOSURE || socket.type == SocketType::NODE || socket.type == SocketType::NODE_ARRAY;
        Json attr = Json::object();
        attr.set("name", Json::str(socket.name.string()));
        attr.set("ui_name", Json::str(socket.ui_name.string()));
        attr.set("type", Json::str(typeName(socket.type)));
        Json def;
        if (!portOnly && defaultValue(socket, def)) {
            attr.set("default", std::move(def));
        }
        if (socket.type == SocketType::ENUM && socket.enum_values != nullptr) {
            attr.set("enum", enumValues(*socket.enum_values));
        }
        attr.set("linkable", Json::boolean(linkable || portOnly));
        attr.set("port_only", Json::boolean(portOnly));
        attr.set("array", Json::boolean(isArray(socket.type)));
        if (const char* link = defaultLink(socket.flags)) {
            attr.set("default_link", Json::str(link));
        }
        attributes.push(std::move(attr));
    }
    Json outputs = Json::array();
    for (const SocketType& socket : type.outputs) {
        outputs.push(Json::object()
                         .set("name", Json::str(socket.name.string()))
                         .set("ui_name", Json::str(socket.ui_name.string()))
                         .set("type", Json::str(typeName(socket.type)))
                         .set("array", Json::boolean(isArray(socket.type))));
    }
    return Json::object()
        .set("id", Json::str("cycles_" + name))
        .set("name", Json::str(name))
        .set("kind", Json::str(kindOf(name, type)))
        .set("targets", Json::array().push(Json::str("cycles")))
        .set("attributes", std::move(attributes))
        .set("outputs", std::move(outputs));
}

std::string
readFile(const std::string& path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// hdCycles/SOURCE starts "hdCycles is built from <repository>, commit <sha>".
std::pair<std::string, std::string>
parseSource(const std::string& text)
{
    const std::string from = "built from ";
    const std::string commit = ", commit ";
    const auto a = text.find(from);
    const auto b = text.find(commit, a == std::string::npos ? 0 : a);
    if (a == std::string::npos || b == std::string::npos) {
        return {};
    }
    const auto repoStart = a + from.size();
    const auto shaStart = b + commit.size();
    auto shaEnd = text.find_first_not_of("0123456789abcdef", shaStart);
    if (shaEnd == std::string::npos) {
        shaEnd = text.size();
    }
    return { text.substr(repoStart, b - repoStart), text.substr(shaStart, shaEnd - shaStart) };
}

int
usage()
{
    std::cerr << "usage: dump_cycles_manifest --library <hdCycles.so> [--source <SOURCE>] [--output <file>]\n";
    return 2;
}

} // namespace

int
main(int argc, char** argv)
{
    std::string library;
    std::string source;
    std::string output;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            return usage();
        }
        if (arg == "--library") {
            library = argv[++i];
        } else if (arg == "--source") {
            source = argv[++i];
        } else if (arg == "--output") {
            output = argv[++i];
        } else {
            return usage();
        }
    }
    if (library.empty()) {
        return usage();
    }

    try {
        SymbolTable symbols(library);
        void* handle = ::dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            std::cerr << "dump_cycles_manifest: " << ::dlerror() << "\n";
            return 1;
        }
        link_map* map = nullptr;
        if (::dlinfo(handle, RTLD_DI_LINKMAP, &map) != 0 || map == nullptr) {
            std::cerr << "dump_cycles_manifest: cannot find the load address of " << library << "\n";
            return 1;
        }
        const auto base = static_cast<uintptr_t>(map->l_addr);
        using TypeNamesFn = ccl::vector<ustring> (*)();
        using FindFn = const NodeType* (*)(ustring);
        const auto typeNames = reinterpret_cast<TypeNamesFn>(base + symbols.find(kTypeNamesSymbol));
        const auto find = reinterpret_cast<FindFn>(base + symbols.find(kFindSymbol));

        // The vector's allocator lives inside the plug-in, so it is never destroyed here.
        alignas(ccl::vector<ustring>) unsigned char storage[sizeof(ccl::vector<ustring>)];
        new (storage) ccl::vector<ustring>(typeNames());
        const auto* names = reinterpret_cast<const UstringVector*>(storage);

        std::vector<std::string> sorted;
        for (const ustring* it = names->begin; it != names->end; ++it) {
            sorted.push_back(it->string());
        }
        std::sort(sorted.begin(), sorted.end());

        Json nodes = Json::array();
        size_t attributeCount = 0;
        for (const std::string& name : sorted) {
            const NodeType* type = find(ustring(name));
            if (type == nullptr || type->type != NodeType::SHADER || !isUserNode(name)) {
                continue;
            }
            Json entry = nodeEntry(name, *type);
            attributeCount += entry.members[4].second.items.size();
            nodes.push(std::move(entry));
        }

        Json src = Json::object();
        src.set("library", Json::str("hdCycles.so (Cycles NodeType registry)"));
        if (!source.empty()) {
            const auto [repo, sha] = parseSource(readFile(source));
            if (!repo.empty()) {
                src.set("repository", Json::str(repo));
                src.set("commit", Json::str(sha));
            }
        }
        src.set("generator", Json::str("tools/shaders/dump_cycles_manifest.cpp"));

        const size_t nodeCount = nodes.items.size();
        Json root = Json::object()
                        .set("schema", Json::raw("1"))
                        .set("renderer", Json::str("cycles"))
                        .set("source", std::move(src))
                        .set("nodes", std::move(nodes));

        std::ostringstream text;
        root.write(text, 0);
        text << "\n";
        if (output.empty()) {
            std::cout << text.str();
        } else {
            std::ofstream out(output, std::ios::binary);
            out << text.str();
            if (!out) {
                std::cerr << "dump_cycles_manifest: cannot write " << output << "\n";
                return 1;
            }
        }
        std::cerr << "dump_cycles_manifest: " << nodeCount << " shaders, " << attributeCount << " attributes\n";
        // Exit without unloading the plug-in: its static destructors would run against the
        // registry this process leaves allocated.
        std::cout.flush();
        std::_Exit(0);
    } catch (const std::exception& e) {
        std::cerr << "dump_cycles_manifest: " << e.what() << "\n";
        return 1;
    }
}
