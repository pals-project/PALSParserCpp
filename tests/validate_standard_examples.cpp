// Validate the standard PALS example files from pals-project/pals/examples
// against this implementation, using only the public C API.
//
//     validate_standard_examples --root <examples-dir> --manifest <manifest>
//
// Every *.pals.yaml file under --root is discovered automatically and is
// expected to expand with zero problems ("clean"). The manifest
// (tests/standard_examples_manifest.txt) only lists the exceptions; its
// header documents the entry format. Two kinds of files are exempted without
// a manifest entry:
//
// - *.subpals.yaml files: per the standard's notation section they are
//   sub-level include fragments, spliced into (and checked through) the file
//   that includes them;
// - files named in another file's `load` list: they are combined into (and
//   checked through) that file, and only their existence is checked. An
//   explicit manifest entry overrides this, for a file that is both loaded
//   and expected to stand on its own.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../src/yaml_c_wrapper.h"

namespace fs = std::filesystem;

namespace {

// Total number of elements in the expanded tree: every entry of every
// branch's `line`, summed over the root lattices. Includes the elements the
// expansion itself adds (fork-created branches, the trailing branch_end).
size_t count_elements(YAMLTreeHandle t) {
    size_t count = 0;
    YAMLNodeId root = get_root(t);
    for (size_t i = 0; i < get_size(t, root); i++) {
        YAMLNodeId lattice = get_child_by_index(t, root, i);
        YAMLNodeId branches = get_child_by_key(t, lattice, "branches");
        if (branches == YAML_NULL_ID) continue;
        for (size_t b = 0; b < get_size(t, branches); b++) {
            // A branch is a single-key map: {name: {line: [...], ...}}.
            YAMLNodeId item = get_child_by_index(t, branches, b);
            if (get_size(t, item) == 0) continue;
            YAMLNodeId branch = get_child_by_index(t, item, 0);
            YAMLNodeId line = get_child_by_key(t, branch, "line");
            if (line != YAML_NULL_ID) count += get_size(t, line);
        }
    }
    return count;
}

struct Entry {
    std::string mode;  // "clean", "lenient", "combine", or "member"
    long elements;     // expected element count; -1 when not pinned ("-")
};

bool parse_manifest(const std::string& path,
                    std::map<std::string, Entry>& out) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open manifest '%s'\n", path.c_str());
        return false;
    }
    std::string line;
    size_t lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        std::istringstream ss(line);
        std::string mode, count, file;
        if (!(ss >> mode >> count >> file) ||
            (mode != "clean" && mode != "lenient" && mode != "combine" &&
             mode != "member")) {
            std::fprintf(stderr, "%s:%zu: malformed manifest line: %s\n",
                         path.c_str(), lineno, line.c_str());
            return false;
        }
        out[file] = {mode, count == "-"
                               ? -1
                               : std::strtol(count.c_str(), nullptr, 10)};
    }
    return true;
}

// All *.pals.yaml files under root, as sorted root-relative paths.
// *.subpals.yaml include fragments do not match and stay exempt.
std::vector<std::string> discover(const std::string& root) {
    const std::string suffix = ".pals.yaml";
    std::vector<std::string> files;
    for (const auto& p : fs::recursive_directory_iterator(root)) {
        if (!p.is_regular_file()) continue;
        const std::string rel =
            fs::relative(p.path(), root).lexically_normal().generic_string();
        if (rel.size() < suffix.size() ||
            rel.compare(rel.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        files.push_back(rel);
    }
    std::sort(files.begin(), files.end());
    return files;
}

// The root-relative paths named in the PALS `load` list of `rel`, resolved
// against the file's own directory the way loading resolves them.
std::vector<std::string> load_list(const std::string& root,
                                   const std::string& rel) {
    std::vector<std::string> loaded;
    YAMLTreeHandle t = parse_file((root + "/" + rel).c_str());
    if (!t) return loaded;
    YAMLNodeId pals = get_child_by_key(t, get_root(t), "PALS");
    YAMLNodeId load = get_child_by_key(t, pals, "load");
    for (size_t i = 0; i < get_size(t, load); i++) {
        char* s = as_string(t, get_child_by_index(t, load, i));
        if (s && std::strcmp(s, "SELF") != 0) {
            const fs::path target =
                (fs::path(root) / fs::path(rel).parent_path() / s)
                    .lexically_normal();
            loaded.push_back(
                fs::relative(target, root).lexically_normal().generic_string());
        }
        yaml_free_string(s);
    }
    delete_tree(t);
    return loaded;
}

void free_all(struct lattices& lat) {
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
    free_lattice_problems(lat.problems);
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string root, manifest;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--root" && i + 1 < argc) root = argv[++i];
        else if (arg == "--manifest" && i + 1 < argc) manifest = argv[++i];
        else {
            std::fprintf(stderr,
                         "usage: %s --root <examples-dir> --manifest <file>\n",
                         argv[0]);
            return 2;
        }
    }
    if (root.empty() || manifest.empty()) {
        std::fprintf(stderr, "--root and --manifest are required\n");
        return 2;
    }

    std::map<std::string, Entry> exceptions;
    if (!parse_manifest(manifest, exceptions)) return 2;

    const std::vector<std::string> files = discover(root);
    const std::set<std::string> present(files.begin(), files.end());

    // A file another file loads is checked through that file, unless the
    // manifest says otherwise.
    std::set<std::string> loaded;
    for (const std::string& rel : files)
        for (const std::string& l : load_list(root, rel)) loaded.insert(l);

    // A manifest entry for a file that does not exist is itself a failure --
    // it means the corpus lost a file the expectations still describe.
    int failures = 0;
    for (const auto& [path, e] : exceptions) {
        (void)e;
        if (!present.count(path)) {
            std::printf("FAIL  %s\n      in the manifest, but not found\n",
                        path.c_str());
            failures++;
        }
    }

    for (const std::string& rel : files) {
        const auto it = exceptions.find(rel);
        const Entry e = it != exceptions.end()
                            ? it->second
                            : Entry{loaded.count(rel) ? "member" : "clean", -1};

        if (e.mode == "member") {
            std::printf("PASS  [member]  %s\n", rel.c_str());
            continue;
        }

        const std::string full = root + "/" + rel;
        struct lattices lat = parse_and_expand_PALS(full.c_str(), nullptr);
        const size_t nprob = lat.problems.count;
        const bool no_lattice_only =
            nprob == 1 && std::strcmp(lat.problems.items[0],
                                      "no lattice found to expand") == 0;
        const size_t elements =
            lat.full_expanded ? count_elements(lat.full_expanded) : 0;

        bool ok = true;
        std::string why;
        if (e.mode == "combine") {
            if (!no_lattice_only) {
                ok = false;
                why = "expected only 'no lattice found to expand'";
            }
        } else {
            if (e.mode == "clean" && nprob != 0) {
                ok = false;
                why = "expected zero problems";
            }
            if (e.elements < 0 ? elements == 0
                               : (size_t)e.elements != elements) {
                ok = false;
                if (!why.empty()) why += "; ";
                why += e.elements < 0
                           ? "expanded to no elements"
                           : "expected " + std::to_string(e.elements) +
                                 " elements, found " + std::to_string(elements);
            }
        }

        std::printf("%s  [%s]  %s\n", ok ? "PASS" : "FAIL", e.mode.c_str(),
                    rel.c_str());
        if (!ok || e.mode == "lenient")
            for (size_t i = 0; i < nprob; i++)
                std::printf("      problem: %s\n", lat.problems.items[i]);
        if (!ok) {
            if (e.elements >= 0 || elements > 0)
                std::printf("      elements found: %zu\n", elements);
            std::printf("      %s\n", why.c_str());
            failures++;
        }
        free_all(lat);
    }

    std::printf("%zu files checked, %d failures\n", files.size(), failures);
    return failures == 0 ? 0 : 1;
}
