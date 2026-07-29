#include "lsystem/GrammarIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace plant::lsystem {
namespace {

/// ordered_json keeps insertion order, so a written file reads name, description,
/// axiom, constants, rules rather than alphabetically.
using Json = nlohmann::ordered_json;

/// Shortest decimal that reads back as exactly the same float.
///
/// Storing a float straight into JSON widens it to double and 0.16f dumps as
/// 0.1599999964237213, which is correct and unreadable. These files are meant
/// to be edited by hand, so try increasing precision until the value survives
/// the round trip and keep the first one that does.
double shortestRoundTrip(float value) {
    char buffer[40];
    for (int precision = 1; precision <= 8; ++precision) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, static_cast<double>(value));
        if (std::strtof(buffer, nullptr) == value) {
            return std::strtod(buffer, nullptr);
        }
    }
    return static_cast<double>(value);  // 9 digits always round-trips a float
}

std::string requireString(const Json& object, const char* field, bool optional = false) {
    const auto found = object.find(field);
    if (found == object.end()) {
        if (optional) {
            return {};
        }
        throw GrammarIOError(std::string("missing required field '") + field + "'");
    }
    if (!found->is_string()) {
        throw GrammarIOError(std::string("field '") + field + "' must be a string");
    }
    return found->get<std::string>();
}

}  // namespace

std::string toJson(const GrammarSource& source) {
    Json object;
    object["name"] = source.name;
    object["description"] = source.description;
    object["axiom"] = source.axiom;

    Json constants = Json::object();
    for (const auto& [name, value] : source.constants) {
        constants[name] = shortestRoundTrip(value);
    }
    object["constants"] = std::move(constants);

    object["rules"] = source.rules;
    return object.dump(2) + "\n";
}

GrammarSource fromJson(std::string_view text) {
    Json object;
    try {
        object = Json::parse(text);
    } catch (const Json::exception& error) {
        throw GrammarIOError(std::string("not valid JSON: ") + error.what());
    }
    if (!object.is_object()) {
        throw GrammarIOError("a grammar file must contain a JSON object");
    }

    GrammarSource source;
    source.name = requireString(object, "name");
    source.description = requireString(object, "description", /*optional=*/true);
    source.axiom = requireString(object, "axiom");
    if (source.name.empty()) {
        throw GrammarIOError("'name' must not be empty");
    }

    if (const auto constants = object.find("constants"); constants != object.end()) {
        if (!constants->is_object()) {
            throw GrammarIOError("'constants' must be an object of name/number pairs");
        }
        for (const auto& [name, value] : constants->items()) {
            if (!value.is_number()) {
                throw GrammarIOError("constant '" + name + "' must be a number");
            }
            source.constants[name] = value.get<float>();
        }
    }

    const auto rules = object.find("rules");
    if (rules == object.end() || !rules->is_array()) {
        throw GrammarIOError("'rules' must be an array of strings");
    }
    for (const Json& rule : *rules) {
        if (!rule.is_string()) {
            throw GrammarIOError("every entry of 'rules' must be a string");
        }
        source.rules.push_back(rule.get<std::string>());
    }
    return source;
}

GrammarSource loadGrammar(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw GrammarIOError("could not open " + path.string());
    }
    std::ostringstream contents;
    contents << file.rdbuf();

    try {
        return fromJson(contents.str());
    } catch (const GrammarIOError& error) {
        throw GrammarIOError(path.filename().string() + ": " + error.what());
    }
}

void saveGrammar(const std::filesystem::path& path, const GrammarSource& source) {
    if (path.has_parent_path() && !path.parent_path().empty()) {
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw GrammarIOError("could not open " + path.string() + " for writing");
    }
    const std::string text = toJson(source);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) {
        throw GrammarIOError("failed while writing " + path.string());
    }
}

GrammarDirectory scanGrammarDirectory(const std::filesystem::path& directory) {
    GrammarDirectory result;
    result.directory = directory;

    std::error_code ec;
    if (directory.empty() || !std::filesystem::is_directory(directory, ec)) {
        return result;  // no directory is not an error; the caller falls back
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    // Mixes each file's name and write time into one value. Not a checksum of
    // the contents -- it only has to change when an edit lands, which is what
    // the viewer polls for.
    std::uint64_t revision = 1469598103934665603ull;
    const auto mix = [&revision](std::uint64_t value) {
        revision = (revision ^ value) * 1099511628211ull;
    };

    for (const std::filesystem::path& file : files) {
        for (const char character : file.filename().string()) {
            mix(static_cast<std::uint64_t>(static_cast<unsigned char>(character)));
        }
        const auto written = std::filesystem::last_write_time(file, ec);
        if (!ec) {
            mix(static_cast<std::uint64_t>(written.time_since_epoch().count()));
        }

        try {
            result.grammars.push_back(loadGrammar(file));
        } catch (const GrammarIOError& error) {
            result.errors.emplace_back(error.what());
        }
    }

    result.revision = revision;
    return result;
}

std::filesystem::path findAssetDirectory(const std::filesystem::path& startingPoint,
                                         int maxLevels) {
    std::error_code ec;
    std::filesystem::path directory = std::filesystem::absolute(startingPoint, ec);
    if (ec) {
        return {};
    }

    for (int level = 0; level <= maxLevels; ++level) {
        const std::filesystem::path candidate = directory / "assets" / "presets";
        if (std::filesystem::is_directory(candidate, ec)) {
            return candidate;
        }
        if (!directory.has_parent_path() || directory.parent_path() == directory) {
            break;
        }
        directory = directory.parent_path();
    }
    return {};
}

}  // namespace plant::lsystem
