#include <archive.h>
#include <archive_entry.h>
#include <hdf5.h>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include "extraction_rules.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
using namespace extraction_rules;
constexpr std::size_t kMaxArchiveMembers = 50000;
constexpr std::size_t kMaxTaskExtractedMembers = 100000;
constexpr std::size_t kMaxNestedArchives = 128;
constexpr std::size_t kMaxSourceFiles = 250000;
constexpr int kMaxNestedDepth = 3;
constexpr std::size_t kMaxParsedFileBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxDatasetBytes = 256 * 1024 * 1024;
constexpr std::uint64_t kMaxArchiveMemberBytes = 512ULL * 1024 * 1024;
constexpr std::uint64_t kMaxArchiveTotalBytes = 4ULL * 1024 * 1024 * 1024;
constexpr std::uint64_t kMaxTaskExpandedBytes = 2ULL * 1024 * 1024 * 1024;

struct Request {
    fs::path source;
    fs::path output;
    std::string packageId = "extracted-assets";
    std::string description = "Extracted Substance asset labels";
    bool ordinaryFilenames = true;
    bool folderNames = false;
    std::set<std::string> attributes;
    std::unordered_set<std::string> excluded;
};

struct State {
    Request request;
    std::set<std::string> terms;
    json failures = json::array();
    std::size_t nestedArchives = 0;
    std::size_t extractedMembers = 0;
    std::uint64_t expandedBytes = 0;
    std::size_t processed = 0;
    std::size_t total = 0;
};

void consumeExpandedBytes(State &state, std::uint64_t bytes) {
    if (bytes > kMaxTaskExpandedBytes - state.expandedBytes)
        throw std::runtime_error(
            "extraction task exceeds the cumulative size limit");
    state.expandedBytes += bytes;
}

void consumeExtractedMember(State &state) {
    if (++state.extractedMembers > kMaxTaskExtractedMembers)
        throw std::runtime_error(
            "extraction task contains too many expanded members");
}

std::string pathUtf8(const fs::path &path) {
#ifdef _WIN32
    const std::wstring wide = path.native();
    if (wide.empty())
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                          static_cast<int>(wide.size()), nullptr,
                                          0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
#else
    return path.string();
#endif
}

fs::path pathFromUtf8(const std::string &value) {
#ifdef _WIN32
    if (value.empty())
        return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()), nullptr,
                                          0);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), count);
    return fs::path(result);
#else
    return fs::path(value);
#endif
}

void emit(const json &message) {
    std::cout << message.dump() << '\n' << std::flush;
}

void addTerm(State &state, const std::string &raw) {
    const std::string value = trim(raw);
    if (validSource(value) && state.request.excluded.count(value) == 0)
        state.terms.insert(value);
}

bool ignoredDirectory(const std::string &name) {
    return name == "__pycache__" || name == "_unpacked_assets" ||
           (!name.empty() && name.front() == '.');
}

bool safeRelative(const fs::path &path) {
    if (path.empty() || path.is_absolute() || path.has_root_name())
        return false;
    for (const auto &part : path) {
        const std::wstring value = part.native();
        if (part == "." || part == ".." || value.empty())
            return false;
        // Prevent NTFS alternate data streams and names that Windows silently
        // normalizes to another path (for example "name." -> "name").
        if (value.find_first_of(L"<>:\"|?*") != std::wstring::npos ||
            value.back() == L'.' || value.back() == L' ')
            return false;
        for (const wchar_t character : value) {
            if (character < 0x20)
                return false;
        }
        std::wstring device = value.substr(0, value.find(L'.'));
        std::transform(device.begin(), device.end(), device.begin(),
                       [](wchar_t character) {
                           return character >= L'a' && character <= L'z'
                               ? static_cast<wchar_t>(character - 32)
                               : character;
                       });
        const bool numberedDevice =
            device.size() == 4 && device.back() >= L'1' &&
            device.back() <= L'9' &&
            (device.substr(0, 3) == L"COM" ||
             device.substr(0, 3) == L"LPT");
        if (device == L"CON" || device == L"PRN" || device == L"AUX" ||
            device == L"NUL" || numberedDevice)
            return false;
    }
    return true;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool looksLikeXml(const fs::path &path) {
    if (lower(path.extension().string()) == ".xml")
        return true;
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    char buffer[4096]{};
    stream.read(buffer, sizeof(buffer));
    const std::string prefix(buffer, static_cast<std::size_t>(stream.gcount()));
    std::size_t position = 0;
    if (prefix.size() >= 3 &&
        static_cast<unsigned char>(prefix[0]) == 0xEFU &&
        static_cast<unsigned char>(prefix[1]) == 0xBBU &&
        static_cast<unsigned char>(prefix[2]) == 0xBFU)
        position = 3;
    while (position < prefix.size()) {
        const unsigned char c = static_cast<unsigned char>(prefix[position]);
        if (c != 0U && std::isspace(c) == 0)
            break;
        ++position;
    }
    return position < prefix.size() && prefix[position] == '<';
}

bool selectedAttribute(const State &state, const std::string &name) {
    if (state.request.attributes.count(name) != 0)
        return true;
    if (state.request.attributes.count("label") != 0) {
        static const std::regex labels(R"(^label\d*$)");
        return std::regex_match(name, labels);
    }
    return false;
}

void addXmlValue(State &state, const std::string &field,
                 const std::string &nodeName, const std::string &raw) {
    const std::string value = trim(raw);
    addTerm(state, value);
    const bool groupPath = field == "group" ||
        (field == "label" && nodeName == "guigroup");
    if (!groupPath)
        return;
    // Split only unspaced, non-numeric hierarchy slashes. Ratios such as 7/1
    // and captions such as Leno / Gauze remain intact.
    std::size_t start = 0;
    bool split = false;
    while (start < value.size()) {
        const std::size_t slash = value.find('/', start);
        if (slash == std::string::npos)
            break;
        const unsigned char before = slash > 0
            ? static_cast<unsigned char>(value[slash - 1]) : ' ';
        const unsigned char after = slash + 1 < value.size()
            ? static_cast<unsigned char>(value[slash + 1]) : ' ';
        if (!std::isspace(before) && !std::isspace(after) &&
            !std::isdigit(before) && !std::isdigit(after)) {
            addTerm(state, value.substr(start, slash - start));
            start = slash + 1;
            split = true;
        } else {
            start = slash + 1;
        }
    }
    if (split && start < value.size())
        addTerm(state, value.substr(start));
}

void collectXmlNode(State &state, pugi::xml_node node) {
    if (node.type() == pugi::node_element) {
        const std::string nodeName = node.name();

        // Designer .sbs files store metadata as an element whose value is in
        // its `v` attribute, for example <label v="3D Perlin noise"/>. Keep
        // supporting the older attribute-oriented Painter XML below as well.
        if (selectedAttribute(state, nodeName)) {
            const pugi::xml_attribute value = node.attribute("v");
            if (value)
                addXmlValue(state, nodeName, nodeName, value.value());
        }

        // Designer 参数如果没有显式 <label>，界面就显示其 <identifier>
        // 作为参数名。提取 identifier 作为 label 的兜底，避免漏掉这类
        // 参数名（如 Asymmetry）。
        if (nodeName == "paraminput" &&
            selectedAttribute(state, "label")) {
            bool hasLabel = false;
            for (pugi::xml_node child : node.children()) {
                if (std::string(child.name()) == "label") {
                    hasLabel = true;
                    break;
                }
            }
            if (!hasLabel) {
                const pugi::xml_node identifier =
                    node.child("identifier");
                const pugi::xml_attribute idValue =
                    identifier.attribute("v");
                if (idValue)
                    addXmlValue(state, "label", nodeName, idValue.value());
            }
        }

        // Widget component captions use an option pair instead of a label
        // element: <option><name v="label0"/><value v="X"/></option>.
        // Treat label0, label1, label2, ... as the common `label` field.
        if (nodeName == "option") {
            const pugi::xml_node nameNode = node.child("name");
            const pugi::xml_node valueNode = node.child("value");
            const pugi::xml_attribute field = nameNode.attribute("v");
            const pugi::xml_attribute value = valueNode.attribute("v");
            if (field && value && selectedAttribute(state, field.value()))
                addXmlValue(state, field.value(), nodeName, value.value());
        }

        for (pugi::xml_attribute attribute : node.attributes()) {
            const std::string name = attribute.name();
            if (!selectedAttribute(state, name))
                continue;
            addXmlValue(state, name, nodeName, attribute.value());
        }
    }
    for (pugi::xml_node child : node.children())
        collectXmlNode(state, child);
}

void parseXml(State &state, const fs::path &path) {
    std::error_code sizeError;
    const std::uintmax_t fileBytes = fs::file_size(path, sizeError);
    if (!sizeError && fileBytes > kMaxParsedFileBytes)
        throw std::runtime_error("XML metadata exceeds the size limit");
    pugi::xml_document document;
    const pugi::xml_parse_result loaded =
        document.load_file(path.c_str(), pugi::parse_default | pugi::parse_doctype);
    if (!loaded)
        throw std::runtime_error(std::string("XML parse error: ") + loaded.description());
    collectXmlNode(state, document);
}

void collectJsonMetadata(State &state, const json &value, int depth = 0) {
    if (depth > 64)
        return;
    if (value.is_object()) {
        for (const auto &[key, child] : value.items()) {
            if (child.is_string() && selectedAttribute(state, key)) {
                addTerm(state, child.get<std::string>());
            } else if (key == "values" &&
                       state.request.attributes.count("values") != 0 &&
                       child.is_object()) {
                for (const auto &[caption, ignored] : child.items()) {
                    (void)ignored;
                    addTerm(state, caption);
                }
            } else {
                collectJsonMetadata(state, child, depth + 1);
            }
        }
    } else if (value.is_array()) {
        for (const auto &child : value)
            collectJsonMetadata(state, child, depth + 1);
    }
}

void parseGlsl(State &state, const fs::path &path) {
    if (fs::file_size(path) > kMaxParsedFileBytes)
        throw std::runtime_error("GLSL metadata exceeds the size limit");
    std::ifstream stream(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(stream)), {});
    if (content.size() > kMaxParsedFileBytes)
        throw std::runtime_error("GLSL metadata exceeds the size limit");
    // Pair braces once instead of rescanning the tail for every unmatched '{'.
    // Balanced outer objects keep the old precedence; completed children of an
    // unclosed outer object are retained for the same recovery at EOF.
    struct Range { std::size_t start, end; };
    std::vector<std::size_t> opens;
    std::vector<Range> completed;
    bool quoted = false;
    bool escaped = false;
    auto collect = [&](const Range &range) {
        const json parsed = json::parse(
            content.begin() + static_cast<std::ptrdiff_t>(range.start),
            content.begin() + static_cast<std::ptrdiff_t>(range.end), nullptr, false);
        if (!parsed.is_discarded())
            collectJsonMetadata(state, parsed);
    };
    for (std::size_t i = 0; i < content.size(); ++i) {
        const char c = content[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            // Raw newlines invalidate JSON strings. Recover at the next line
            // rather than letting a malformed annotation hide later metadata.
            else if (c == '\n' || c == '\r') quoted = false;
            continue;
        }
        if (c == '"' && !opens.empty()) {
            quoted = true;
        } else if (c == '{') {
            if (opens.size() >= 1000000)
                throw std::runtime_error("GLSL metadata nesting exceeds the size limit");
            opens.push_back(i);
        } else if (c == '}' && !opens.empty()) {
            const Range range{opens.back(), i + 1};
            opens.pop_back();
            if (opens.empty()) {
                collect(range);
                completed.clear();
            } else {
                if (completed.size() >= 1000000)
                    throw std::runtime_error("GLSL metadata contains too many objects");
                completed.push_back(range);
            }
        }
    }
    std::sort(completed.begin(), completed.end(), [](const Range &a, const Range &b) {
        return a.start < b.start;
    });
    std::size_t consumed = 0;
    for (const Range &range : completed) {
        if (range.start >= consumed) {
            collect(range);
            consumed = range.end;
        }
    }
}

std::vector<std::string> lengthPrefixedStrings(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    const std::vector<unsigned char> data((std::istreambuf_iterator<char>(stream)), {});
    if (data.size() > kMaxParsedFileBytes)
        throw std::runtime_error("preset.bin exceeds the size limit");
    std::vector<std::string> result;
    for (std::size_t offset = 0; offset + 4 <= data.size();) {
        const std::uint32_t length = static_cast<std::uint32_t>(data[offset]) |
            (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
            (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
            (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
        if (length > 0U && length < 500U && offset + 4U + length <= data.size()) {
            std::string value(reinterpret_cast<const char *>(data.data() + offset + 4U),
                              static_cast<std::size_t>(length));
            const bool printable = std::all_of(value.begin(), value.end(), [](unsigned char c) {
                return c >= 0x20U || c == '\t' || c == '\r' || c == '\n';
            });
            if (printable) {
                result.push_back(std::move(value));
                offset += 4U + length;
                continue;
            }
        }
        ++offset;
    }
    return result;
}

void parsePresetBin(State &state, const fs::path &path) {
    const std::vector<std::string> strings = lengthPrefixedStrings(path);
    std::unordered_set<std::string> fieldNames = {
        "label", "name", "displayname", "display_name", "layername", "layer_name"};
    for (std::size_t index = 0; index < strings.size(); ++index) {
        const std::string value = trim(strings[index]);
        const std::string folded = lower(value);
        if (fieldNames.count(folded) != 0 && index + 1 < strings.size()) {
            addTerm(state, strings[index + 1]);
            continue;
        }
        if (value.size() < 2U || value.size() > 80U ||
            value.rfind("Data", 0) == 0 || value.rfind("GUI", 0) == 0 ||
            value.find("://") != std::string::npos)
            continue;
        const bool hasSpace = value.find(' ') != std::string::npos;
        const bool titleWord = std::isupper(static_cast<unsigned char>(value.front())) != 0 &&
            std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
                return std::isalpha(c) != 0;
            });
        if (hasSpace || titleWord)
            addTerm(state, value);
    }
}

bool archiveFormat(const fs::path &path) {
    archive *reader = archive_read_new();
    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);
    const int result = archive_read_open_filename_w(reader, path.c_str(), 10240);
    archive_read_free(reader);
    return result == ARCHIVE_OK;
}

void extractArchive(State &state, const fs::path &source,
                    const fs::path &destination) {
    const std::unique_ptr<archive, decltype(&archive_read_free)> owned(
        archive_read_new(), &archive_read_free);
    archive *reader = owned.get();
    if (!reader) throw std::runtime_error("cannot allocate archive reader");
    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);
    if (archive_read_open_filename_w(reader, source.c_str(), 10240) != ARCHIVE_OK) {
        const std::string message = archive_error_string(reader)
            ? archive_error_string(reader) : "unable to open archive";
        throw std::runtime_error(message);
    }
    std::size_t members = 0;
    std::uint64_t memberBytes = 0;
    std::uint64_t archiveBytes = 0;
    archive_entry *entry = nullptr;
    int headerStatus = ARCHIVE_OK;
    while ((headerStatus = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        if (++members > kMaxArchiveMembers) {
            throw std::runtime_error("archive contains more than 50000 entries");
        }
        consumeExtractedMember(state);
        memberBytes = 0;
        const wchar_t *wideName = archive_entry_pathname_w(entry);
        const char *utf8Name = archive_entry_pathname_utf8(entry);
        const fs::path relative = wideName ? fs::path(wideName)
            : pathFromUtf8(utf8Name ? utf8Name : "");
        if (!safeRelative(relative)) {
            throw std::runtime_error("unsafe archive path");
        }
        const auto type = archive_entry_filetype(entry);
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
            (type != AE_IFREG && type != AE_IFDIR)) {
            throw std::runtime_error(
                "archive links and special files are not allowed");
        }
        const fs::path output = destination / relative;
        if (type == AE_IFDIR) {
            fs::create_directories(output);
            continue;
        }
        fs::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("cannot create extracted file");
        }
        const void *buffer = nullptr;
        std::size_t size = 0;
        la_int64_t offset = 0;
        while (true) {
            const int status = archive_read_data_block(reader, &buffer, &size, &offset);
            if (status == ARCHIVE_EOF)
                break;
            if (status != ARCHIVE_OK) {
                const std::string message = archive_error_string(reader)
                    ? archive_error_string(reader) : "archive read failed";
                throw std::runtime_error(message);
            }
            memberBytes += size;
            archiveBytes += size;
            if (memberBytes > kMaxArchiveMemberBytes) {
                throw std::runtime_error(
                    "archive member exceeds the size limit");
            }
            if (archiveBytes > kMaxArchiveTotalBytes) {
                throw std::runtime_error(
                    "archive extraction exceeds the total size limit");
            }
            consumeExpandedBytes(state, size);
            if (offset < 0 || static_cast<std::uint64_t>(offset) > kMaxArchiveMemberBytes ||
                size > kMaxArchiveMemberBytes - static_cast<std::uint64_t>(offset))
                throw std::runtime_error("archive sparse extent exceeds the size limit");
            file.seekp(offset);
            file.write(static_cast<const char *>(buffer),
                       static_cast<std::streamsize>(size));
            if (!file) {
                throw std::runtime_error("failed to write extracted file");
            }
        }
        file.flush();
        if (!file) {
            throw std::runtime_error("failed to flush extracted file");
        }
    }
    if (headerStatus != ARCHIVE_EOF)
        throw std::runtime_error(archive_error_string(reader)
            ? archive_error_string(reader) : "archive header read failed");
}

// HDF5 handles must also close when allocation or filesystem operations throw.
struct HdfHandle {
    hid_t id;
    herr_t (*close)(hid_t);
    HdfHandle(hid_t value, herr_t (*release)(hid_t)) : id(value), close(release) {}
    ~HdfHandle() { if (id >= 0) close(id); }
    HdfHandle(const HdfHandle &) = delete;
    HdfHandle &operator=(const HdfHandle &) = delete;
};

bool hdfHasVariableStorage(hid_t type, int depth = 0) {
    if (type < 0 || depth > 64)
        throw std::runtime_error("invalid or deeply nested HDF5 datatype");
    const H5T_class_t kind = H5Tget_class(type);
    if (kind == H5T_NO_CLASS)
        throw std::runtime_error("cannot inspect HDF5 datatype");
    if (kind == H5T_VLEN || (kind == H5T_STRING && H5Tis_variable_str(type) > 0))
        return true;
    if (kind == H5T_ARRAY) {
        const HdfHandle base(H5Tget_super(type), H5Tclose);
        return hdfHasVariableStorage(base.id, depth + 1);
    }
    if (kind == H5T_COMPOUND) {
        const int members = H5Tget_nmembers(type);
        if (members < 0) throw std::runtime_error("cannot inspect HDF5 members");
        for (int i = 0; i < members; ++i) {
            const HdfHandle member(H5Tget_member_type(type, static_cast<unsigned>(i)), H5Tclose);
            if (hdfHasVariableStorage(member.id, depth + 1)) return true;
        }
    }
    return false;
}

struct HdfContext {
    fs::path destination;
    State *state = nullptr;
    std::exception_ptr error;
};

herr_t hdfVisitor(hid_t object, const char *name, const H5O_info2_t *info,
                  void *operatorData) {
    if (info->type != H5O_TYPE_DATASET)
        return 0;
    auto *context = static_cast<HdfContext *>(operatorData);
    try {
        consumeExtractedMember(*context->state);
        const fs::path relative = pathFromUtf8(name);
        if (!safeRelative(relative))
            throw std::runtime_error("unsafe HDF5 dataset path");
        const HdfHandle datasetHandle(H5Dopen2(object, name, H5P_DEFAULT), H5Dclose);
        const hid_t dataset = datasetHandle.id;
        if (dataset < 0)
            throw std::runtime_error("cannot open HDF5 dataset");
        const HdfHandle typeHandle(H5Dget_type(dataset), H5Tclose);
        const HdfHandle spaceHandle(H5Dget_space(dataset), H5Sclose);
        const hid_t type = typeHandle.id;
        const hid_t space = spaceHandle.id;
        if (type < 0 || space < 0) {
            throw std::runtime_error("cannot inspect HDF5 dataset");
        }
        // Variable data reads allocate pointer-backed payloads outside the
        // byte buffer/budget and cannot be exported as raw asset bytes.
        if (hdfHasVariableStorage(type))
            throw std::runtime_error("variable-length HDF5 datasets are not supported");
        const hssize_t points = H5Sget_simple_extent_npoints(space);
        const std::size_t width = H5Tget_size(type);
        if (points < 0 || width == 0 ||
            static_cast<unsigned long long>(points) >
                (std::numeric_limits<std::size_t>::max)() / width) {
            throw std::runtime_error("invalid HDF5 dataset size");
        }
        const std::size_t datasetBytes =
            static_cast<std::size_t>(points) * width;
        if (datasetBytes > kMaxDatasetBytes) {
            throw std::runtime_error("HDF5 dataset exceeds the size limit");
        }
        if (datasetBytes >
                kMaxTaskExpandedBytes - context->state->expandedBytes) {
            throw std::runtime_error(
                "extraction task exceeds the cumulative size limit");
        }
        context->state->expandedBytes += datasetBytes;
        std::vector<unsigned char> data(datasetBytes);
        if (!data.empty() && H5Dread(dataset, type, H5S_ALL, H5S_ALL,
                                    H5P_DEFAULT, data.data()) < 0) {
            throw std::runtime_error("cannot read HDF5 dataset");
        }
        const fs::path output = context->destination / relative;
        fs::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (!file)
            throw std::runtime_error("cannot create HDF5 output file");
        file.write(reinterpret_cast<const char *>(data.data()),
                   static_cast<std::streamsize>(data.size()));
        if (!file)
            throw std::runtime_error("cannot write HDF5 output file");
    } catch (...) {
        context->error = std::current_exception();
        return -1;
    }
    return 0;
}

void extractHdf5(State &state, const fs::path &source,
                 const fs::path &destination) {
    const hid_t file = H5Fopen(pathUtf8(source).c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file < 0)
        throw std::runtime_error("cannot open HDF5 container");
    HdfContext context{destination, &state, nullptr};
    const herr_t result = H5Ovisit3(file, H5_INDEX_NAME, H5_ITER_NATIVE,
                                    hdfVisitor, &context, H5O_INFO_BASIC);
    H5Fclose(file);
    if (context.error)
        std::rethrow_exception(context.error);
    if (result < 0)
        throw std::runtime_error("HDF5 traversal failed");
}

bool isHdf5(const fs::path &path) {
    return H5Fis_hdf5(pathUtf8(path).c_str()) > 0;
}

bool isContainer(const fs::path &path) {
    try {
        return isHdf5(path) || archiveFormat(path);
    } catch (...) {
        return false;
    }
}

void extractContainer(State &state, const fs::path &source,
                      const fs::path &destination) {
    fs::create_directories(destination);
    if (isHdf5(source))
        extractHdf5(state, source, destination);
    else
        extractArchive(state, source, destination);
}

bool isShader(const fs::path &path) {
    static const std::set<std::string> extensions = {
        ".glsl", ".glslfx", ".vert", ".frag", ".geom", ".tesc", ".tese", ".comp"};
    return extensions.count(lower(path.extension().string())) != 0;
}

void scanExtracted(State &state, const fs::path &root, int depth) {
    std::vector<fs::path> files;
    auto iterator = fs::recursive_directory_iterator(root);
    for (const auto &entry : iterator) {
        if (entry.is_directory()) {
            const std::string name = lower(entry.path().filename().string());
            if (ignoredDirectory(name)) {
                iterator.disable_recursion_pending();
                continue;
            }
        }
        if (entry.is_regular_file())
            files.push_back(entry.path());
    }
    for (const fs::path &path : files) {
        const bool shader = isShader(path);
        if (shader || looksLikeXml(path)) {
            try {
                if (shader) parseGlsl(state, path);
                else parseXml(state, path);
            } catch (const std::exception &error) {
                state.failures.push_back({{"file", pathUtf8(path)},
                                          {"message", error.what()}});
            }
        }
        if (lower(path.filename().string()) == "preset.bin") {
            try {
                parsePresetBin(state, path);
            } catch (const std::exception &error) {
                state.failures.push_back({{"file", pathUtf8(path)},
                                          {"message", error.what()}});
            }
        }
        if (shader || depth >= kMaxNestedDepth || !isContainer(path))
            continue;
        if (state.nestedArchives >= kMaxNestedArchives)
            throw std::runtime_error("more than 128 nested containers");
        const fs::path destination = root /
            ("_nested_" + std::to_string(++state.nestedArchives));
        try {
            extractContainer(state, path, destination);
            scanExtracted(state, destination, depth + 1);
        } catch (const std::exception &error) {
            state.failures.push_back({{"file", pathUtf8(path)},
                                      {"message", error.what()}});
        }
    }
}

void processAsset(State &state, const fs::path &asset) {
    static const std::set<std::string> knownContainers = {
        ".sbsar", ".spsm", ".sppr", ".spp", ".sbsprs", ".sbsasm",
        ".zip", ".7z"};
    const std::string extension = lower(asset.extension().string());
    // libarchive's mtree detector may accept ordinary shader text as an
    // archive. Known shader formats go directly to their metadata parser;
    // this also avoids opening/probing every shader twice.
    const bool shader = isShader(asset);
    const bool container = !shader && isContainer(asset);
    if (container || knownContainers.count(extension) != 0 ||
        state.request.ordinaryFilenames)
        addTerm(state, pathUtf8(asset.stem()));
    if (container) {
        // The nested-archive guard is a per-asset safety limit: reset the
        // counter so extracting one container cannot consume the 128-archive
        // budget for every later asset in the same run.
        state.nestedArchives = 0;
        const fs::path temporary = fs::temp_directory_path() /
            ("sp_translation_extract_" +
             std::to_string(std::chrono::high_resolution_clock::now()
                                .time_since_epoch().count()));
        try {
            extractContainer(state, asset, temporary);
            scanExtracted(state, temporary, 1);
            fs::remove_all(temporary);
        } catch (...) {
            std::error_code ignored;
            fs::remove_all(temporary, ignored);
            throw;
        }
    } else if (shader) {
        parseGlsl(state, asset);
    } else if (looksLikeXml(asset)) {
        parseXml(state, asset);
    }
}

Request loadRequest(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot open request JSON");
    const json payload = json::parse(stream);
    Request request;
    request.source = pathFromUtf8(payload.at("source").get<std::string>());
    request.output = pathFromUtf8(payload.at("output").get<std::string>());
    request.packageId = payload.value("package_id", request.packageId);
    request.description = payload.value("description", request.description);
    request.ordinaryFilenames = payload.value("ordinary_filenames", true);
    request.folderNames = payload.value("folder_names", false);
    for (const auto &attribute : payload.value("attributes", json::array()))
        request.attributes.insert(attribute.get<std::string>());
    for (const auto &source : payload.value("excluded", json::array()))
        request.excluded.insert(source.get<std::string>());
    return request;
}

json loadExisting(const fs::path &path) {
    if (!fs::exists(path))
        return json::object();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read existing translation package");
    const json payload = json::parse(stream);
    if (!payload.is_object() || payload.value("$schema", "") != "sp-translation-v1" ||
        payload.value("language", "") != "zh-CN" || !payload.contains("translations") ||
        !payload["translations"].is_object())
        throw std::runtime_error("existing translation package is invalid; output unchanged");
    for (const auto &value : payload["translations"])
        if (!value.is_string())
            throw std::runtime_error("existing translation value is not a string; output unchanged");
    return payload["translations"];
}

void writeResult(const State &state) {
    json translations = loadExisting(state.request.output);
    for (const std::string &term : state.terms) {
        if (!translations.contains(term))
            translations[term] = "";
    }
    json sorted = json::object();
    std::vector<std::string> keys;
    for (const auto &[key, value] : translations.items()) {
        // 只要求已有词条本身是字符串：不按 validSource 再次过滤，
        // 避免重复提取时把用户手工加入的中文键/数字键等条目静默丢掉。
        if (value.is_string())
            keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), [](const std::string &a,
                                           const std::string &b) {
        return lower(a) < lower(b);
    });
    for (const std::string &key : keys)
        sorted[key] = translations[key];
    const json payload = {
        {"$schema", "sp-translation-v1"},
        {"id", state.request.packageId},
        {"language", "zh-CN"},
        {"description", state.request.description},
        {"extraction", {{"asset_count", state.total},
                        {"failed_count", state.failures.size()},
                        {"term_count", sorted.size()},
                        {"attributes", state.request.attributes},
                        {"ordinary_filenames", state.request.ordinaryFilenames},
                        {"container_filenames", true},
                        {"folder_names", state.request.folderNames},
                        {"glsl_metadata", true}}},
        {"translations", sorted}};
    fs::create_directories(state.request.output.parent_path());
    const fs::path temporary = state.request.output.parent_path() /
        (state.request.output.filename().wstring() + L".tmp");
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << payload.dump(2) << '\n';
        stream.flush();
        if (!stream)
            throw std::runtime_error("failed to write output JSON");
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), state.request.output.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("failed to replace output JSON");
#else
    fs::rename(temporary, state.request.output);
#endif

    fs::path failureLog = state.request.output;
    failureLog.replace_filename(state.request.output.stem().wstring() +
                                L"_failures.txt");
    if (state.failures.empty()) {
        std::error_code ignored;
        fs::remove(failureLog, ignored);
    } else {
        std::ofstream failures(failureLog, std::ios::binary | std::ios::trunc);
        for (const auto &failure : state.failures)
            failures << failure.value("file", "") << '\t'
                     << failure.value("message", "") << '\n';
    }
}

int run(const fs::path &requestPath) {
    State state{loadRequest(requestPath)};
    if (!fs::is_directory(state.request.source))
        throw std::runtime_error("source directory does not exist");
    // 拒绝扫描磁盘根目录（如 C:\），防止误操作扫描整个分区。
    if (state.request.source == state.request.source.root_path())
        throw std::runtime_error(
            "source directory must not be a drive root");
    std::vector<fs::path> assets;
    auto iterator = fs::recursive_directory_iterator(
        state.request.source, fs::directory_options::skip_permission_denied);
    for (const auto &entry : iterator) {
        if (entry.is_directory()) {
            const std::string name = lower(entry.path().filename().string());
            if (ignoredDirectory(name)) {
                iterator.disable_recursion_pending();
                continue;
            }
            if (state.request.folderNames)
                addTerm(state, pathUtf8(entry.path().filename()));
        } else if (entry.is_regular_file() &&
                   fs::absolute(entry.path()) != fs::absolute(state.request.output)) {
            assets.push_back(entry.path());
            if (assets.size() > kMaxSourceFiles)
                throw std::runtime_error(
                    "source directory contains more than 250000 files");
        }
    }
    std::sort(assets.begin(), assets.end());
    state.total = assets.size();
    if (assets.empty()) {
        emit({{"type", "fatal"},
              {"message",
               "资产目录中没有可提取的文件（目录为空，或仅包含被忽略的"
               "隐藏/缓存目录）。"}});
        return 1;
    }
    for (const fs::path &asset : assets) {
        ++state.processed;
        emit({{"type", "progress"}, {"current", state.processed},
              {"total", state.total}, {"file", pathUtf8(asset)}});
        try {
            const std::size_t before = state.terms.size();
            processAsset(state, asset);
            emit({{"type", "success"}, {"file", pathUtf8(asset)},
                  {"terms", static_cast<std::int64_t>(state.terms.size() - before)}});
        } catch (const std::exception &error) {
            state.failures.push_back({{"file", pathUtf8(asset)},
                                      {"message", error.what()}});
            emit({{"type", "warning"}, {"file", pathUtf8(asset)},
                  {"message", error.what()}});
        }
    }
    writeResult(state);
    emit({{"type", "finished"}, {"terms", state.terms.size()},
          {"failures", state.failures.size()},
          {"output", pathUtf8(state.request.output)}});
    return 0;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
    try {
#ifdef _WIN32
        const bool hasRequest =
            argc == 3 && std::wstring(argv[1]) == L"--request";
#else
        const bool hasRequest =
            argc == 3 && std::string(argv[1]) == "--request";
#endif
        if (!hasRequest) {
            std::cerr << "usage: translator_extractor --request request.json\n";
            return 2;
        }
        return run(fs::path(argv[2]));
    } catch (const std::exception &error) {
        emit({{"type", "fatal"}, {"message", error.what()}});
        return 1;
    }
}
